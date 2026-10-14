#pragma once
#include <reflect/Reflect.h>

// Deliberately NOT self-contained, like many headers in big projects:
//  - std::string comes from the force-included prelude (/FI or -include),
//  - std::vector from the target's PRECOMPILE_HEADERS,
//  - GAME_CXX_ONLY from a $<COMPILE_LANGUAGE:CXX> compile definition.
#if !defined(GAME_PRELUDE_LOADED) || !defined(GAME_CXX_ONLY)
#error "prelude or language-specific define missing"
#endif

namespace game
{
    struct REFLECT(component) Named
    {
        PROP(label = "Name") std::string name;
        PROP() std::vector<int> tags;
    };
}
