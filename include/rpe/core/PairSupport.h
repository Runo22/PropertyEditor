#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  std::pair support.
//
//  RTTR 0.9.6 does not reflect std::pair on its own, and nothing in rpe keys
//  off this header: plain RTTR registration of "first"/"second" is all the
//  grid needs, so registering by hand works identically:
//
//      rttr::registration::class_<std::pair<int, double>>("std::pair<int,double>")
//          .property("first",  &std::pair<int, double>::first)
//          .property("second", &std::pair<int, double>::second);
//
//  The helpers below just save the boilerplate. Primary form — a template
//  function, called next to your other registrations:
//
//      rpe::registerPair<int, double>();
//
//  The registered pair behaves like a two-row struct everywhere — inside
//  containers (std::vector<std::pair<…>>) and nested paths ("range.first").
//  Collapsed, the row shows the compact "[first, second]" summary the grid
//  gives every small struct; expanded, the two elements edit individually.
// ─────────────────────────────────────────────────────────────────────────────

#include "rpe/core/rttr_prelude.h"

#include <rttr/registration.h>

#include <string>
#include <utility>

namespace rpe
{

    template <class A, class B>
    void registerPair()
    {
        using P = std::pair<A, B>;
        // Name derived from RTTR's own type names, so the same instantiation
        // always registers under the same name regardless of call site.
        const std::string name = "std::pair<"
            + rttr::type::get<A>().get_name().to_string() + ","
            + rttr::type::get<B>().get_name().to_string() + ">";
        rttr::registration::class_<P>(name)
            .property("first", &P::first)
            .property("second", &P::second);
    }

} // namespace rpe

// Macro shorthand for the same registration, if you prefer that form.
#define RPE_REGISTER_PAIR(A, B) ::rpe::registerPair<A, B>()
