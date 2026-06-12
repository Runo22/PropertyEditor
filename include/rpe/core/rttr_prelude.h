#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  rttr_prelude — includes the core RTTR headers in a known-good order.
//
//  RTTR's detail headers (e.g. data_address_container.h) embed rttr::type by
//  value, so <rttr/type.h> MUST be seen before <rttr/variant.h> / instance.h /
//  property.h, otherwise the type is still incomplete. Funnelling the includes
//  through this single header keeps the order correct and prevents include
//  sorters from re-alphabetising them into a broken order.
// ─────────────────────────────────────────────────────────────────────────────
// clang-format off
#include <rttr/type.h>
#include <rttr/variant.h>
#include <rttr/instance.h>
#include <rttr/property.h>
// clang-format on
