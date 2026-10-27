// The type-release lock (rpe/core/TypeRelease.h) on its own:
//   • a release waits for another thread's read section to end, and holds new
//     ones off while its handlers run;
//   • it never needs another thread to RUN — a thread blocked outside any read
//     section (a GUI thread joining the releasing thread) doesn't stall it;
//   • a thread inside a read section may release itself (no self-deadlock), and
//     read sections nest;
//   • handlers run on the releasing thread, a removed handler never runs, and a
//     handler may release more types (nested).
#include <rpe/core/TypeRelease.h>

#include <rttr/registration.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

struct Alpha
{
    int a = 0;
};
struct Beta
{
    int b = 0;
};

RTTR_REGISTRATION
{
    rttr::registration::class_<Alpha>("Alpha").property("a", &Alpha::a);
    rttr::registration::class_<Beta>("Beta").property("b", &Beta::b);
}

static int g_fails = 0;
static void check(const char* n, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", n);
    if (!ok)
        ++g_fails;
}

using namespace std::chrono_literals;

int main()
{
    const rttr::type alpha = rttr::type::get<Alpha>();
    const rttr::type beta = rttr::type::get<Beta>();

    // ── handlers ───────────────────────────────────────────────────────────────
    {
        std::thread::id ranOn;
        int calls = 0;
        const int h = rpe::addTypeReleaseHandler([&](const std::vector<rttr::type>& r) {
            ++calls;
            ranOn = std::this_thread::get_id();
            check("the handler gets the released types", r.size() == 1 && r[0] == alpha);
        });
        const uint64_t before = rpe::typeReleaseCount();
        std::thread([&] { rpe::detail::releaseTypes({ alpha }); }).join();
        check("a handler runs on the releasing thread", calls == 1 && ranOn != std::this_thread::get_id());
        check("typeReleaseCount counts releases", rpe::typeReleaseCount() == before + 1);
        rpe::removeTypeReleaseHandler(h);
        rpe::detail::releaseTypes({ alpha });
        check("a removed handler never runs", calls == 1);
    }

    // ── a release waits for another thread's read section ──────────────────────
    {
        std::atomic<bool> released { false };
        std::thread t;
        {
            rpe::TypeUseScope reading;
            t = std::thread([&] {
                rpe::detail::releaseTypes({ alpha });
                released = true;
            });
            std::this_thread::sleep_for(50ms);
            check("a release waits while another thread is reading", !released.load());
            {
                rpe::TypeUseScope nested; // nesting never blocks, even with a release waiting
                check("read sections nest while a release waits", !released.load());
            }
        }
        t.join();
        check("...and runs once the read section ends", released.load());
    }

    // ── new readers wait while handlers run ────────────────────────────────────
    {
        std::atomic<bool> inHandler { false }, readerEntered { false }, readerSawHandler { false };
        const int h = rpe::addTypeReleaseHandler([&](const std::vector<rttr::type>&) {
            inHandler = true;
            std::this_thread::sleep_for(60ms);
            inHandler = false;
        });
        std::thread rel([&] { rpe::detail::releaseTypes({ alpha }); });
        while (!inHandler)
            std::this_thread::yield();
        std::thread reader([&] {
            rpe::TypeUseScope s;
            readerEntered = true;
            readerSawHandler = inHandler.load();
        });
        rel.join();
        reader.join();
        check("a reader arriving mid-release waits for the handlers to finish",
              readerEntered.load() && !readerSawHandler.load());
        rpe::removeTypeReleaseHandler(h);
    }

    // ── the releasing thread may itself be reading ─────────────────────────────
    {
        bool done = false;
        std::thread([&] {
            rpe::TypeUseScope s;
            rpe::detail::releaseTypes({ beta }); // would self-deadlock on a plain RW lock
            done = true;
        }).join();
        check("a thread inside a read section can release (no self-deadlock)", done);
    }

    // ── a blocked thread outside any read section doesn't stall a release ──────
    {
        bool done = false;
        std::thread worker([&] {
            rpe::detail::releaseTypes({ alpha });
            done = true;
        });
        worker.join(); // this thread is blocked, as a GUI thread waiting for its sim
        check("a release completes while the other thread is blocked waiting for it", done);
    }

    // ── a gap lets a thread wait for a lock whose owner is releasing ───────────
    {
        // Direct mode with a world guard: the GUI, inside a read section, needs the
        // world lock to write an edit; the sim holds that lock and unregisters a
        // type. Without the gap each waits for the other forever.
        std::mutex worldLock;
        std::atomic<bool> simHasLock { false }, simDone { false };
        bool guiWrote = false;
        std::thread sim([&] {
            std::lock_guard<std::mutex> lk(worldLock);
            simHasLock = true;
            std::this_thread::sleep_for(30ms); // the GUI is now waiting in its gap
            rpe::detail::releaseTypes({ alpha });
            simDone = true;
        });
        while (!simHasLock)
            std::this_thread::yield();
        {
            rpe::TypeUseScope reading;
            {
                rpe::TypeUseGap gap;
                std::lock_guard<std::mutex> lk(worldLock);
                rpe::TypeUseScope inside;
                guiWrote = true;
            }
        }
        sim.join();
        check("a TypeUseGap around a lock avoids the reader/lock-owner deadlock", guiWrote && simDone.load());
    }

    // ── two releases at once, one from inside a read section ───────────────────
    {
        // A is in a read section. B starts a release and waits for A to leave it.
        // A then starts its own release: it must step out while it waits for B, or
        // B waits for A's read section and A waits for B's release — forever.
        std::atomic<bool> aDone { false }, bDone { false };
        std::thread a([&] {
            rpe::TypeUseScope reading;
            std::this_thread::sleep_for(40ms); // B is the writer by now, waiting for us
            rpe::detail::releaseTypes({ alpha });
            aDone = true;
        });
        std::this_thread::sleep_for(10ms); // A holds its read section first
        std::thread b([&] {
            rpe::detail::releaseTypes({ beta });
            bDone = true;
        });
        for (int i = 0; i < 200 && !(aDone && bDone); ++i)
            std::this_thread::sleep_for(10ms);
        check("a release from inside a read section while another waits doesn't deadlock", aDone && bDone);
        if (aDone && bDone)
        {
            a.join();
            b.join();
        }
        else
        {
            fflush(stdout);
            std::_Exit(1); // the threads are stuck: report and leave
        }
    }

    // ── nested release from a handler ──────────────────────────────────────────
    {
        int betaSeen = 0;
        const int h = rpe::addTypeReleaseHandler([&](const std::vector<rttr::type>& r) {
            if (r[0] == alpha)
                rpe::detail::releaseTypes({ beta });
            else if (r[0] == beta)
                ++betaSeen;
        });
        rpe::detail::releaseTypes({ alpha });
        check("a handler may release more types (nested)", betaSeen == 1);
        rpe::removeTypeReleaseHandler(h);
    }

    // ── isReleased ─────────────────────────────────────────────────────────────
    {
        const std::vector<rttr::type> rel { alpha };
        check("isReleased: the type itself", rpe::isReleased(alpha, rel));
        check("isReleased: a pointer to it", rpe::isReleased(rttr::type::get<Alpha*>(), rel));
        check("isReleased: another type is not", !rpe::isReleased(beta, rel));
        check("isReleased: an invalid type is not", !rpe::isReleased(rttr::type::get_by_name(std::string()), rel));
    }

    printf(g_fails ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_fails);
    return g_fails ? 1 : 0;
}
