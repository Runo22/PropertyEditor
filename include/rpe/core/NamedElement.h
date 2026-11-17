#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QHash>
#include <QString>
#include <QStringList>

// ─────────────────────────────────────────────────────────────────────────────
//  Named elements — an array of name/value structs shown by NAME.
//
//      template <class N, class V> struct Stat { N name; V value; };
//      std::vector<Stat<std::string, float>> stats;   // speed, armor, …
//
//  Marked (any one is enough; the runtime ones win over the metadata):
//      class_<Stat<std::string, float>>("Stat<std::string,float>")(
//          metadata(rpe::hint::ElementLabel, "name"), metadata(rpe::hint::ElementValue, "value"))
//      rpe::TypeBridge::setNamedElement<Stat<std::string, float>>("name", "value");
//      rpe::TypeBridge::setNamedElementTemplate("Stat", "name", "value");   // every Stat<…>
//
//  the array shows one row per element — "armor   7" — with the value edited on
//  that row (other fields, if any, below it). The name can be any type with a
//  text: std::string, std::string_view, const char*, QString, an enum (its
//  name), a number. Names are shown, never edited.
//
//  Paths address an element by name with a "{name}" step: "stats.{armor}.value"
//  is the value of the FIRST element named "armor" — a pin made on a named row
//  uses it, so it follows the element when the array is reordered or resized.
//  A name can't contain '.' or '}' to be addressed this way.
// ─────────────────────────────────────────────────────────────────────────────
namespace rpe
{

    namespace detail
    {
        inline rttr::property noProperty()
        {
            return rttr::type::get<void>().get_property(std::string());
        }
    } // namespace detail

    struct NamedElement
    {
        rttr::property label = detail::noProperty(); // the name field
        rttr::property value = detail::noProperty(); // the value field (invalid: none)
        bool isValid() const
        {
            return label.is_valid();
        }
    };

    // The fields of `elementType` (wrappers looked through) when it's a named
    // element whose label field exists; invalid otherwise. A value field that was
    // named but doesn't exist is left invalid (the row then expands to the fields).
    NamedElement namedElement(rttr::type elementType);

    // An element's name as text ("" if it can't be read).
    QString namedElementLabel(const NamedElement& ne, const rttr::variant& element);

    // The index of the first element of `container` (a sequential container value)
    // named `label`, or -1 (none, or not an array of named elements).
    int findNamedElement(const rttr::variant& container, const QString& label);

    // A path for showing to people: a "{name}" step becomes the name, and the
    // named element's value field right after it is dropped —
    // "stats.{armor}.value" → "stats.armor". `root` is the path's root type.
    QString displayPath(rttr::type root, const QString& path);

    // The names of every named-element array inside `value` (itself, its fields,
    // its elements, nested — a few levels deep), as text, keyed by each array's
    // path (`path` is `value`'s own). A source that hands values to ANOTHER thread
    // sends these along: a string_view / const char* name in the copy still points
    // into the source's memory (see PropertyModel::setPropertyValue).
    void collectNamedLabels(const rttr::variant& value, const QString& path, QHash<QString, QStringList>& out);

    // True if `path` (from `root`) ends at a named element's NAME field.
    bool isNamedLabelPath(rttr::type root, const QString& path);

    // A string_view / wstring_view / const char* / char* value as an owned
    // QString variant (anything else as it is): safe to hand to another thread.
    rttr::variant ownedText(const rttr::variant& v);

} // namespace rpe
