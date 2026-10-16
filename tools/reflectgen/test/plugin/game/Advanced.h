#pragma once
#include <reflect/Reflect.h>

#include <string>

namespace game
{
    // Template alias: the instantiation is what gets reflected.
    template <class T>
    struct Vec2T
    {
        PROP(step = 0.5) T x{};
        PROP() T y{};
        T notProp{};
    };
    using Vec2f REFLECT(meta) = Vec2T<float>;

    // Base fields are registered on every REFLECT struct deriving from it.
    struct REFLECT() Base
    {
        PROP(min = 0) int hp = 10;

    protected:
        PROP() int armor = 2; // Base's own registration needs the friend; Unit's needs its own
        REFLECT_FRIEND
    };

    struct REFLECT(component) Unit : Base
    {
        PROP() float speed = 1;
        // Accessor properties: read/write through getter + setter, or read-only.
        PROP(setter = setPower, max = 100) float getPower() const { return power_; }
        void setPower(float v) { power_ = v; }
        PROP(name = "label") const std::string& getLabel() const { return label_; }
        PROP() Vec2f pos;

    private:
        float power_ = 5;
        std::string label_ = "unit";
        REFLECT_FRIEND
    };
}
