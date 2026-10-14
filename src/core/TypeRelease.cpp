#include "rpe/core/TypeRelease.h"

#include <QColor>
#include <QDateTime>
#include <QString>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace rpe
{

    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::condition_variable cv;
            int readers = 0; // threads inside an outermost TypeUseScope
            bool writer = false;
            std::thread::id writerId;

            std::mutex handlerMutex;
            std::map<int, TypeReleaseHandler> handlers;
            int nextHandle = 1;

            std::atomic<uint64_t> releases { 0 };
        };

        // Leaked on purpose: scopes and handler removals may run during static
        // destruction (widgets destroyed at exit), after a function-local static
        // would already be gone.
        State& state()
        {
            static State* s = new State;
            return *s;
        }

        thread_local int t_depth = 0;

        // RTTR gives a type's data to the FIRST module that asks for it; every
        // later module's type::get<T>() is a static bound to that data. If a plugin
        // were first to use QColor (say), unloading it would free QColor's type data
        // under rpe's — and the host's — cached handles for good. Asking for every
        // type rpe names, as rpe_core loads (before any plugin can), makes rpe_core
        // their owner for the life of the process.
        struct OwnCommonTypes
        {
            OwnCommonTypes()
            {
                using namespace std::chrono;
                (void)rttr::type::get<void>();
                (void)rttr::type::get<bool>();
                (void)rttr::type::get<char>();
                (void)rttr::type::get<signed char>();
                (void)rttr::type::get<unsigned char>();
                (void)rttr::type::get<short>();
                (void)rttr::type::get<unsigned short>();
                (void)rttr::type::get<int>();
                (void)rttr::type::get<unsigned int>();
                (void)rttr::type::get<long>();
                (void)rttr::type::get<unsigned long>();
                (void)rttr::type::get<long long>();
                (void)rttr::type::get<unsigned long long>();
                (void)rttr::type::get<float>();
                (void)rttr::type::get<double>();
                (void)rttr::type::get<long double>();
                (void)rttr::type::get<const char*>();
                (void)rttr::type::get<std::string>();
                (void)rttr::type::get<std::wstring>();
                (void)rttr::type::get<std::u16string>();
                (void)rttr::type::get<std::u32string>();
                (void)rttr::type::get<std::string_view>();
                (void)rttr::type::get<std::wstring_view>();
                (void)rttr::type::get<std::filesystem::path>();
                (void)rttr::type::get<nanoseconds>();
                (void)rttr::type::get<microseconds>();
                (void)rttr::type::get<milliseconds>();
                (void)rttr::type::get<seconds>();
                (void)rttr::type::get<minutes>();
                (void)rttr::type::get<hours>();
                (void)rttr::type::get<QString>();
                (void)rttr::type::get<QColor>();
                (void)rttr::type::get<QDateTime>();
            }
        };
        const OwnCommonTypes g_ownCommonTypes;
    } // namespace

    TypeUseScope::TypeUseScope()
    {
        if (t_depth++ > 0)
        {
            return;
        }
        State& s = state();
        std::unique_lock<std::mutex> lk(s.mutex);
        const auto me = std::this_thread::get_id();
        s.cv.wait(lk, [&] { return !s.writer || s.writerId == me; });
        ++s.readers;
    }

    TypeUseScope::~TypeUseScope()
    {
        if (--t_depth > 0)
        {
            return;
        }
        State& s = state();
        {
            std::lock_guard<std::mutex> lk(s.mutex);
            --s.readers;
        }
        s.cv.notify_all();
    }

    TypeUseGap::TypeUseGap()
        : _depth(t_depth)
    {
        if (_depth == 0)
        {
            return;
        }
        t_depth = 0;
        State& s = state();
        {
            std::lock_guard<std::mutex> lk(s.mutex);
            --s.readers;
        }
        s.cv.notify_all();
    }

    TypeUseGap::~TypeUseGap()
    {
        if (_depth == 0)
        {
            return;
        }
        State& s = state();
        std::unique_lock<std::mutex> lk(s.mutex);
        const auto me = std::this_thread::get_id();
        s.cv.wait(lk, [&] { return !s.writer || s.writerId == me; });
        ++s.readers;
        t_depth = _depth;
    }

    int addTypeReleaseHandler(TypeReleaseHandler handler)
    {
        State& s = state();
        std::lock_guard<std::mutex> lk(s.handlerMutex);
        const int handle = s.nextHandle++;
        s.handlers.emplace(handle, std::move(handler));
        return handle;
    }

    void removeTypeReleaseHandler(int handle)
    {
        // The scope keeps a release on another thread from calling the handler
        // while (or after) its owner is destroyed.
        TypeUseScope scope;
        State& s = state();
        std::lock_guard<std::mutex> lk(s.handlerMutex);
        s.handlers.erase(handle);
    }

    uint64_t typeReleaseCount()
    {
        return state().releases.load(std::memory_order_acquire);
    }

    bool isReleased(rttr::type t, const std::vector<rttr::type>& released)
    {
        if (!t.is_valid())
        {
            return false;
        }
        // Called while the released types' module is still loaded, so following
        // t's own links is safe.
        const rttr::type raw = t.get_raw_type();
        const rttr::type wrapped = t.is_wrapper() ? t.get_wrapped_type().get_raw_type() : raw;
        for (const rttr::type& r : released)
        {
            if (t == r || raw == r || wrapped == r)
            {
                return true;
            }
        }
        return false;
    }

    namespace detail
    {
        void releaseTypes(const std::vector<rttr::type>& types)
        {
            if (types.empty())
            {
                return;
            }
            State& s = state();
            const auto me = std::this_thread::get_id();
            bool nested = false;
            {
                std::unique_lock<std::mutex> lk(s.mutex);
                if (s.writer && s.writerId == me)
                {
                    nested = true; // a handler unregistering more types
                }
                else
                {
                    s.cv.wait(lk, [&] { return !s.writer; });
                    s.writer = true;
                    s.writerId = me;
                    // Wait for every OTHER thread's read section to end (this thread
                    // may itself be inside one).
                    const int mine = t_depth > 0 ? 1 : 0;
                    s.cv.wait(lk, [&] { return s.readers == mine; });
                }
            }
            s.releases.fetch_add(1, std::memory_order_acq_rel);

            std::vector<TypeReleaseHandler> handlers;
            {
                std::lock_guard<std::mutex> lk(s.handlerMutex);
                handlers.reserve(s.handlers.size());
                for (const auto& h : s.handlers)
                {
                    handlers.push_back(h.second);
                }
            }
            for (const TypeReleaseHandler& h : handlers)
            {
                try
                {
                    if (h)
                    {
                        h(types);
                    }
                }
                catch (...)
                {
                    // A handler must not leave the release half-done (the lock held).
                }
            }

            if (!nested)
            {
                {
                    std::lock_guard<std::mutex> lk(s.mutex);
                    s.writer = false;
                }
                s.cv.notify_all();
            }
        }
    } // namespace detail

} // namespace rpe
