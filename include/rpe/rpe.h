#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  rpe — RTTR Property Editor
//
//  Umbrella header. Include this to pull in the whole library, or include the
//  individual headers you need.
//
//  Layers:
//    core/  engine-agnostic RTTR logic (no Qt widgets)
//    gui/   reusable Qt property-grid widgets
//    ecs/   optional flecs integration (compiled only with RPE_WITH_FLECS)
// ─────────────────────────────────────────────────────────────────────────────

// core
#include "rpe/core/AccessGuard.h"
#include "rpe/core/EditorHints.h"
#include "rpe/core/PropertyNode.h"
#include "rpe/core/RttrBridge.h"
#include "rpe/core/RttrVariantWrapper.h"
#include "rpe/core/TypeBridge.h"
#include "rpe/core/TypeRenderer.h"

// gui
#include "rpe/gui/EditorWidgets.h"
#include "rpe/gui/PropertyDelegate.h"
#include "rpe/gui/PropertyEditor.h"
#include "rpe/gui/PropertyModel.h"
#include "rpe/gui/VariantEditor.h"

// ecs (optional)
#if defined(RPE_WITH_FLECS)
#  include "rpe/ecs/ComponentListWidget.h"
#  include "rpe/ecs/EntityComponentBrowser.h"
#  include "rpe/ecs/EntityListWidget.h"
#endif
