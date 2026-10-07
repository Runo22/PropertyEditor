#pragma once
#include <reflect/Reflect.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace game
{
    inline constexpr double kMaxSpeed = 250.0;

    enum class REFLECT() Shape { Circle, Square, Count NOPROP };

    enum class REFLECT(flags) Damage : uint32_t { None = 0, Fire = 1, Poison = 2, Ice = 4 };

    struct REFLECT(meta) Vec3 // value type used as a field: RTTR + flecs meta, no editor bridge
    {
        PROP(step = 0.1) float x = 0;
        PROP(step = 0.1) float y = 0;
        PROP(step = 0.1) float z = 0;
    };

    // explicit mode (default): only PROP fields are registered
    struct REFLECT(component, meta) Light
    {
        PROP(min = 0, max = 100, step = 0.5, decimals = 2, slider) float intensity = 1.f;
        PROP(file, label = "Icon", tooltip = "Shown in the outliner") std::string iconPath;
        PROP(max = kMaxSpeed) double speed = 1.0;   // constant from the namespace
        PROP() Shape shape = Shape::Circle;
        PROP() Damage damage = Damage::None;        // flags hint added automatically
        PROP() Vec3 offset;
        float cache = 0;                            // no PROP → not registered
    };

    // all mode: every public field, PROP adds hints, NOPROP opts out
    struct REFLECT(component, all, name = "ai::Stats") Stats
    {
        PROP(min = 0, max = 1000) int hp = 100;
        std::optional<float> shield;
        std::vector<int> inventory;
        NOPROP int debugCounter = 0;

    private:
        PROP(readonly) int secret = 7;              // reachable via REFLECT_FRIEND
        REFLECT_FRIEND
    };

    struct REFLECT(component) Selected {};          // empty → flecs tag

    namespace inner
    {
        struct REFLECT(component, all) Stats { int level = 1; }; // same leaf name, other namespace
    }
}
