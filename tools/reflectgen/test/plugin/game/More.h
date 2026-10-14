#pragma once
#include <reflect/Reflect.h>

namespace game
{
    // A second reflected header: its generated file shares a translation unit with
    // Components.h's when the plugin is built with UNITY_BUILD.
    struct REFLECT(component) Velocity
    {
        PROP(min = -10, max = 10) float dx = 0;
        PROP() float dy = 0;
    };
}
