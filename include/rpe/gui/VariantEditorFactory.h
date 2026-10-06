#pragma once

#include <QString>
#include <QWidget>

#include <optional>

#include <rttr/type.h>
#include <rttr/variant.h>

namespace rpe::varedit
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  VariantEditorFactory — the model-independent core of type-appropriate inline
    //  editors: build the widget for a declared rttr::type (+ optional hints), push a
    //  value into it, and read the edited value back out.
    //
    //  Both the property grid's PropertyDelegate (which sources hints from
    //  PropertyModel roles) and the pinned-properties list's delegate (which has only
    //  the last mirrored value's type) share this factory, so their editors stay
    //  identical: bool → check box, number → spin box, enum → combo, QColor → picker,
    //  path → file/dir picker, string → line edit / multi-line, etc.
    // ─────────────────────────────────────────────────────────────────────────────

    // Optional numeric metadata (min/max/step/decimals) and the bitmask-enum flag.
    // Empty optionals fall back to per-editor defaults inside makeEditor().
    struct EditorHints
    {
        std::optional<double> min;
        std::optional<double> max;
        std::optional<double> step;
        std::optional<int> decimals;
        bool flags = false; // enum is a bitmask (use FlagsEditor instead of a combo)
    };

    // Build the editor widget for a leaf of declared type `t` with the given editor
    // hint (an rpe::editor::* string, or empty). Returns nullptr for expandable /
    // unsupported types that aren't inline-editable.
    QWidget* makeEditor(rttr::type t, const QString& editorHint, const EditorHints& hints, QWidget* parent);

    // Populate `editor` from `value` (unwrapped and optional-extracted internally, so
    // the caller passes the raw mirrored/model variant).
    void setEditorData(QWidget* editor, const rttr::variant& value);

    // Read `editor` back into a variant of the declared type `t`. Returns an invalid
    // variant when the input can't be parsed (caller should not commit).
    rttr::variant readEditorData(QWidget* editor, rttr::type t);

    // "Did the user change anything?" — for delegates. Call rememberOpeningValue
    // right after setEditorData: it records what the editor SHOWS, read back the
    // same way a commit would be (so after any range clamping). At commit time,
    // unchangedSinceOpen(editor, newVal) is true when the user left it as it was
    // — then don't write. Otherwise opening an editor on a value outside the
    // editor's range (Min/Max) and pressing Enter would silently write the
    // clamped value back.
    void rememberOpeningValue(QWidget* editor, rttr::type t);

    // `value` as a variant of the numeric type `t` (float, double, or an integer
    // type — rounded, and range-checked by RTTR's conversion). Invalid when it
    // can't be represented.
    rttr::variant numberAs(double value, rttr::type t);
    bool unchangedSinceOpen(QWidget* editor, const rttr::variant& newVal);

} // namespace rpe::varedit
