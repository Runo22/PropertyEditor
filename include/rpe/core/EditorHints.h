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
//  All keys are plain string literals so they work with RTTR 0.9.x metadata,
//  which keys on rttr::variant (here, const char*).
// ─────────────────────────────────────────────────────────────────────────────

namespace rpe::hint
{

    // Numeric constraints (value: double / int).
    inline constexpr const char* Min = "rpe.min";
    inline constexpr const char* Max = "rpe.max";
    inline constexpr const char* Step = "rpe.step";
    inline constexpr const char* Decimals = "rpe.decimals";

    // Explicit editor selection (value: one of rpe::editor::*).
    inline constexpr const char* Editor = "rpe.editor";

    // Human-friendly display label (value: const char*). Falls back to property name.
    inline constexpr const char* Label = "rpe.label";

    // Tooltip / description (value: const char*).
    inline constexpr const char* Tooltip = "rpe.tooltip";

    // Mark a property read-only even in an editable view (value: bool).
    inline constexpr const char* ReadOnly = "rpe.readOnly";

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
