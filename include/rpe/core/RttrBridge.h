#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QString>
#include <QStringList>

// ─────────────────────────────────────────────────────────────────────────────
//  RttrBridge — pure (Qt-free except QString) helpers for traversing and
//  mutating an RTTR object graph by a dot-separated path.
//
//  Path grammar:   segment ('.' segment)*
//    segment   :=  <property-name> | '[' <index> ']'
//    e.g.        "position.x", "forces.[2]", "bones.[0].rotation.y"
//
//  Write-back (setValueByPath) correctly handles value-type sub-objects by
//  reading the sub-object, mutating the copy, and writing it back up the chain —
//  so editing "position.x" on a component actually updates the component.
// ─────────────────────────────────────────────────────────────────────────────
namespace rpe::bridge
{

    // Split "a.b.[0].c" into ["a", "b", "[0]", "c"].
    QStringList splitPath(const QString& path);

    // Read the value at a dot path relative to `root`. Returns an invalid variant on failure.
    rttr::variant getValueByPath(const rttr::instance& root, const QString& path);

    // Write `value` at the dot path relative to `root`. Returns true on success.
    bool setValueByPath(rttr::instance root, const QString& path, const rttr::variant& value);

    // Convert `value` to match `target` type as closely as possible (in place copy).
    // Useful before set_value so e.g. a double from a spinbox lands in a float field.
    rttr::variant coerce(rttr::variant value, rttr::type target);

} // namespace rpe::bridge
