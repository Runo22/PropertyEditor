// A stand-in PLUGIN: a separate shared library that registers its own type with
// rpe hints. Its string literals live in THIS module — which is exactly what used
// to make rpe miss its metadata (RTTR compared const char* keys by address).
#include <rpe/core/EditorHints.h>
#include <rpe/core/TypeBridge.h>

#include <rttr/registration.h>

namespace plug
{
    struct Gauge
    {
        double value = 5;
        int locked = 1;
    };
    struct Frozen
    {
        int a = 1;
    };
}

RTTR_REGISTRATION
{
    using namespace rttr;
    registration::class_<plug::Gauge>("plug::Gauge")
        .property("value", &plug::Gauge::value)(metadata(rpe::hint::Min, 0.0), metadata(rpe::hint::Max, 10.0),
                                                metadata(rpe::hint::Label, "Gauge value"))
        .property("locked", &plug::Gauge::locked)(metadata(rpe::hint::ReadOnly, true));
    registration::class_<plug::Frozen>("plug::Frozen")(metadata(rpe::hint::ReadOnly, true))
        .property("a", &plug::Frozen::a);
}

extern "C"
#if defined(_WIN32)
    __declspec(dllexport)
#endif
    void hintPluginRegister()
{
    rpe::TypeBridge::registerType<plug::Gauge>();
    rpe::TypeBridge::registerType<plug::Frozen>();
}
