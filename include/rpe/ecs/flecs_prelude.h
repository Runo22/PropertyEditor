#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  flecs_prelude — includes <flecs.h> with Qt's keyword macros temporarily
//  neutralised.
//
//  flecs' C++ API has methods named emit()/signals/slots, which collide with
//  Qt's `emit`, `signals`, `slots` keyword macros when a Qt header is included
//  first. We push and undef those macros around the flecs include, then restore
//  them so our own Q_OBJECT code keeps working normally.
// ─────────────────────────────────────────────────────────────────────────────

#pragma push_macro("emit")
#pragma push_macro("signals")
#pragma push_macro("slots")
#undef emit
#undef signals
#undef slots

#include <flecs.h>

#pragma pop_macro("slots")
#pragma pop_macro("signals")
#pragma pop_macro("emit")
