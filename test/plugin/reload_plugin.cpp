// A stand-in plugin that is UNLOADED and loaded again while rpe shows its
// component — the reload cycle a host runs on every start. Loaded with dlopen /
// LoadLibrary, so it really leaves the process. On Linux it is built the way every
// Windows DLL behaves: no GNU "unique" symbols (which would pin it in memory) and
// hidden visibility (so its RTTR registration manager is its own, not interposed
// by the host's — with a static RTTR, RTTR_LOCAL is empty).
#include <rpe/core/EditorHints.h>
#include <rpe/core/TypeBridge.h>

#include <rttr/registration.h>

#include <flecs.h>

#include <string>
#include <vector>

namespace rp
{
    enum class Mode
    {
        Idle,
        Run,
        Boost
    };
    struct Inner
    {
        float a = 1.5f;
        float b = 2.5f;
    };
    struct Engine
    {
        double rpm = 900;
        Mode mode = Mode::Run;
        Inner inner;
        std::vector<int> gears { 1, 2, 3 };
        std::string label = "v8";
    };
} // namespace rp

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::enumeration<rp::Mode>("rp::Mode")(value("Idle", rp::Mode::Idle), value("Run", rp::Mode::Run),
                                                    value("Boost", rp::Mode::Boost));
    registration::class_<rp::Inner>("rp::Inner").property("a", &rp::Inner::a).property("b", &rp::Inner::b);
    registration::class_<rp::Engine>("rp::Engine")
        .property("rpm", &rp::Engine::rpm)(metadata(rpe::hint::Min, 0.0), metadata(rpe::hint::Max, 9000.0))
        .property("mode", &rp::Engine::mode)
        .property("inner", &rp::Engine::inner)
        .property("gears", &rp::Engine::gears)
        .property("label", &rp::Engine::label);
}

#if defined(_WIN32)
#define RP_EXPORT extern "C" __declspec(dllexport)
#else
#define RP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

RP_EXPORT void reloadPluginLoad(flecs::world* w, double rpm)
{
    w->component<rp::Engine>("rp::Engine");
    rpe::TypeBridge::registerType<rp::Engine>();
    rp::Engine e;
    e.rpm = rpm;
    w->entity("Car").set<rp::Engine>(e);
}

// The plugin's unload path: its data leaves the world, its types leave rpe.
RP_EXPORT void reloadPluginUnload(flecs::world* w)
{
    w->remove_all<rp::Engine>();
    w->component<rp::Engine>().destruct();
    rpe::TypeBridge::unregisterType<rp::Engine>();
}
