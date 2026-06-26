#include "rpe/core/TypeBridge.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace rpe
{

    namespace
    {

        struct Entry
        {
            rttr::type type = rttr::type::get<void>();
            TypeBridge::Wrapper wrap;
            TypeBridge::Cloner clone;
        };

        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<rttr::type::type_id, Entry> map;
        };

        Registry& registry()
        {
            static Registry r;
            return r;
        }

        // The segment after the last "::" (or the whole string if none).
        std::string shortName(const std::string& s)
        {
            const auto pos = s.rfind("::");
            return pos == std::string::npos ? s : s.substr(pos + 2);
        }

    } // namespace

    void TypeBridge::registerEntry(rttr::type t, Wrapper wrap, Cloner clone)
    {
        if (!t.is_valid() || !wrap || !clone)
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.map[t.get_id()] = Entry { t, std::move(wrap), std::move(clone) };
    }

    void TypeBridge::unregisterType(rttr::type t)
    {
        if (!t.is_valid())
        {
            return;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        r.map.erase(t.get_id());
    }

    rttr::type TypeBridge::resolveByName(const char* flecsName)
    {
        if (!flecsName || flecsName[0] == '\0')
        {
            return rttr::type::get_by_name(std::string()); // invalid
        }
        const std::string name = flecsName;

        // Fast path: exact RTTR name that is also bridged.
        rttr::type direct = rttr::type::get_by_name(name);
        if (direct.is_valid() && has(direct))
        {
            return direct;
        }

        // Fallback: match a bridged type by exact or short name (handles flecs
        // short names vs. scoped RTTR registrations, and differing names).
        const std::string target = shortName(name);
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        for (const auto& [id, entry] : r.map)
        {
            const std::string rn = entry.type.get_name().to_string();
            if (rn == name || shortName(rn) == target)
            {
                return entry.type;
            }
        }
        return rttr::type::get_by_name(std::string()); // invalid
    }

    rttr::variant TypeBridge::wrap(rttr::type t, void* obj)
    {
        if (!t.is_valid() || !obj)
        {
            return {};
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        const auto it = r.map.find(t.get_id());
        return it != r.map.end() ? it->second.wrap(obj) : rttr::variant();
    }

    rttr::variant TypeBridge::clone(rttr::type t, void* obj)
    {
        if (!t.is_valid() || !obj)
        {
            return {};
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        const auto it = r.map.find(t.get_id());
        return it != r.map.end() ? it->second.clone(obj) : rttr::variant();
    }

    bool TypeBridge::has(rttr::type t)
    {
        if (!t.is_valid())
        {
            return false;
        }
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        return r.map.find(t.get_id()) != r.map.end();
    }

    std::vector<rttr::type> TypeBridge::registeredTypes()
    {
        std::vector<rttr::type> out;
        auto& r = registry();
        std::lock_guard<std::mutex> lk(r.mutex);
        out.reserve(r.map.size());
        for (const auto& [id, entry] : r.map)
        {
            out.push_back(entry.type);
        }
        return out;
    }

} // namespace rpe
