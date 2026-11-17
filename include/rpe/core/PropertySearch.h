#pragma once

#include "rpe/core/rttr_prelude.h"

#include <QString>

#include <vector>

namespace rpe
{

    // ─────────────────────────────────────────────────────────────────────────────
    //  PropertySearch — find the rows of an object's property tree whose NAME or
    //  VALUE text contains a needle (case-insensitive), the same rule as the
    //  property editor's filter box. Walks the object the way the tree shows it:
    //  display names (the Label hint) for the name, the rendered value text for the
    //  value; nested structs are walked. A container is reported as ONE hit — its
    //  value, so a view can lay its elements out — when its name matches or any row
    //  inside it does: an element's text, or, for elements that are structs (and
    //  map values, nested containers), their fields' names and values. `matches`
    //  says how many rows inside matched.
    //
    //  Used by "search all components": the mirror runs it on the simulation thread
    //  for the selected entity's components, the browser in direct mode. Bounded by
    //  `Limits`, so a huge array or a deep object can't make one search expensive.
    // ─────────────────────────────────────────────────────────────────────────────
    struct PropertyHit
    {
        QString path;        // dotted, as the tree's paths ("position.x", "gears")
        rttr::variant value; // the row's value (a struct / container as a whole)
        int matches = 1;     // rows it stands for: a container's matching rows inside
    };

    struct PropertySearchLimits
    {
        int maxRows = 4000;     // rows visited per object before giving up
        int maxElements = 256;  // elements looked at per container
        int maxDepth = 8;       // containers / structs nested inside a container
    };

    // Appends the hits to `out`; returns how many were added. `type` is the object's
    // declared type (for its properties), `obj` an instance of it.
    int searchProperties(const rttr::instance& obj, rttr::type type, const QString& needle,
                         std::vector<PropertyHit>& out, const PropertySearchLimits& limits = {});

} // namespace rpe
