#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  std::pair support.
//
//  RTTR 0.9.6 does not reflect std::pair on its own. One macro call makes a
//  given instantiation inspectable/editable everywhere in the property grid —
//  as a tiny two-row struct ("first" / "second"), including inside containers
//  (std::vector<std::pair<…>>) and nested paths ("range.first"):
//
//      RPE_REGISTER_PAIR(int, double);   // next to your other registrations
//
//  Collapsed, the row shows the compact "[first, second]" summary the grid
//  gives every small struct; expanded, the two elements edit individually.
//
//  NOTE: the arguments are pasted into a name and a template — for types that
//  themselves contain commas (std::pair<std::map<K,V>, T>) introduce a typedef
//  first and pass that.
// ─────────────────────────────────────────────────────────────────────────────

#include "rpe/core/rttr_prelude.h"

#include <rttr/registration.h>

#include <utility>

#define RPE_REGISTER_PAIR(A, B)                                             \
    rttr::registration::class_<std::pair<A, B>>("std::pair<" #A "," #B ">") \
        .property("first", &std::pair<A, B>::first)                         \
        .property("second", &std::pair<A, B>::second)
