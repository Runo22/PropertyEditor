#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Reflect.h — source annotations read by `reflectgen`.
//
//  The macros expand to NOTHING for the real compiler (MSVC, GCC, Clang): they
//  only become [[clang::annotate]] attributes while reflectgen parses the header
//  (it defines REFLECT_PARSE). The generator keys on the annotation TEXT, not on
//  the macro names, so rename the macros freely if they clash with your code.
//
//      struct REFLECT(component) Light            // a flecs component
//      {
//          PROP(min = 0, max = 100, slider) float intensity = 1.f;
//          PROP(color)                      Color tint;
//          PROP(file, label = "Icon")       std::string iconPath;
//          float cache;                     // not a PROP → skipped (explicit mode)
//      };
//
//      struct REFLECT(component, all) Stats    // `all`: every public field
//      {
//          PROP(min = 0) int hp = 100;          // still takes per-field hints
//          NOPROP int debugCounter = 0;        // opt one field out
//      };
//
//      enum class REFLECT(flags) Damage : uint32_t { Fire = 1, Poison = 2, Count NOPROP };
//
//  Hint values are copied verbatim into the generated C++ (inside the type's
//  namespace), so constants work: PROP(max = kMaxSpeed).
// ─────────────────────────────────────────────────────────────────────────────

#if defined(REFLECT_PARSE)
#define REFLECT(...) [[clang::annotate("reflect:" #__VA_ARGS__)]]
#define PROP(...) [[clang::annotate("prop:" #__VA_ARGS__)]]
#define NOPROP [[clang::annotate("noprop:")]]
#else
#define REFLECT(...)
#define PROP(...)
#define NOPROP
#endif

namespace reflect
{
    // Generated registration code lives in specialisations of this template.
    template <class T>
    struct Access;
} // namespace reflect

// Put inside a class to let the generated code reach private/protected fields.
// Deliberately NOT RTTR_ENABLE(): that adds a vtable, which a flecs component
// (moved around with memcpy-like semantics, size-sensitive) must not get.
#define REFLECT_FRIEND        \
    template <class>          \
    friend struct ::reflect::Access;
