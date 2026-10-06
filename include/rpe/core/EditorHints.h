#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  EditorHints — RTTR metadata keys understood by the property editor.
//
//  Register them on properties to drive richer, constrained editors (UE5-style):
//
//      registration::class_<Light>("Light")
//          .property("intensity", &Light::intensity)
//          (
//              metadata(rpe::hint::Min,      0.0),
//              metadata(rpe::hint::Max,      100.0),
//              metadata(rpe::hint::Step,     0.5),
//              metadata(rpe::hint::Decimals, 2)
//          )
//          .property("iconPath", &Light::iconPath)
//          (
//              metadata(rpe::hint::Editor, rpe::editor::FilePath)
//          )
//          .property("tint", &Light::tint)
//          (
//              metadata(rpe::hint::Editor, rpe::editor::Color)
//          );
//
//  KEYS are 64-bit values (a compile-time hash of the key's name), not string
//  literals. RTTR compares a `const char*` metadata key by ADDRESS, and every
//  module — the executable, each plugin DLL, rpe itself — has its own copy of a
//  literal, so a hint registered in a plugin was never found by rpe: plugin-side
//  Min/Max/Editor/ReadOnly/… hints were silently ignored. A value compares equal
//  across modules. Nothing changes at the registration site.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

namespace rpe::hint
{

    using Key = std::uint64_t;

    // FNV-1a over the key's name; constexpr, so a key is a compile-time constant
    // in every module (no static-initialisation-order concerns inside
    // RTTR_REGISTRATION blocks).
    constexpr Key makeKey(const char* name)
    {
        Key h = 1469598103934665603ull;
        while (*name)
        {
            h ^= static_cast<unsigned char>(*name++);
            h *= 1099511628211ull;
        }
        return h;
    }

    // Numeric constraints (value: double / int).
    inline constexpr Key Min = makeKey("rpe.min");
    inline constexpr Key Max = makeKey("rpe.max");
    inline constexpr Key Step = makeKey("rpe.step");
    inline constexpr Key Decimals = makeKey("rpe.decimals");

    // Explicit editor selection (value: one of rpe::editor::*).
    inline constexpr Key Editor = makeKey("rpe.editor");

    // Human-friendly display label (value: const char*). Falls back to property name.
    inline constexpr Key Label = makeKey("rpe.label");

    // Tooltip / description (value: const char*).
    inline constexpr Key Tooltip = makeKey("rpe.tooltip");

    // Mark a property read-only even in an editable view (value: bool).
    inline constexpr Key ReadOnly = makeKey("rpe.readOnly");

    // Treat an enumeration property as a BITMASK (value: bool). The value is shown
    // decomposed ("Fire | Poison", plus "0xNN" for any un-named leftover bits) and
    // edited through a multi-check editor instead of a single-choice combo. Enums
    // are NOT auto-detected as flags (a plain enum whose values happen to be powers
    // of two would be misread) — this opt-in is the signal. Editing combined values
    // additionally needs RPE_REGISTER_FLAGS(T) (see rpe/core/FlagsSupport.h): RTTR
    // 0.9.6 cannot build an enum from an integer without the compile-time type.
    inline constexpr Key Flags = makeKey("rpe.flags");

} // namespace rpe::hint

namespace rpe::editor
{

    // Values for the hint::Editor metadata key.
    inline constexpr const char* Default = "default";   // pick automatically by type
    inline constexpr const char* FilePath = "file";     // line edit + "Browse…" (open file)
    inline constexpr const char* SaveFile = "savefile"; // line edit + "Browse…" (save file)
    inline constexpr const char* Directory = "dir";     // line edit + "Browse…" (pick folder)
    inline constexpr const char* Color = "color";       // color swatch + picker
    inline constexpr const char* Multiline = "text";    // multi-line plain text
    inline constexpr const char* Slider = "slider";     // slider + spin (needs Min/Max)

} // namespace rpe::editor
