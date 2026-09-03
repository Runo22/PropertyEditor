cmake_minimum_required(VERSION 3.21)
project(rpe LANGUAGES CXX)

# ── Options ──────────────────────────────────────────────────────────────────
option(RPE_WITH_FLECS      "Build the flecs ECS integration layer" ON)
option(RPE_BUILD_DEMO      "Build the demo/test application"       ON)
option(RPE_USE_SYSTEM_DEPS "Resolve rttr/flecs via find_package instead of FetchContent" OFF)
# flecs MUST be one shared instance when the world crosses a DLL/plugin boundary
# (host owns the world, plugin inspects it). Default to shared; set OFF only for
# a single-binary build.
option(RPE_FLECS_SHARED    "Build/link flecs as a shared library (DLL)" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static RTTR/flecs are linked into the SHARED rpe_core, so everything must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── Qt ───────────────────────────────────────────────────────────────────────
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Widgets Gui)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ── Dependencies ─────────────────────────────────────────────────────────────
# Resolution order (integration-friendly):
#   1. Targets already defined by the parent project (add_subdirectory embed).
#   2. find_package, when RPE_USE_SYSTEM_DEPS is ON.
#   3. FetchContent fallback (standalone build).
include(FetchContent)

# RTTR (reflection). NOTE: the static target RTTR::Core_Lib only exists when
# BUILD_STATIC is ON — this was the bug that prevented the original build.
if(NOT TARGET RTTR::Core_Lib AND NOT TARGET RTTR::Core)
    if(RPE_USE_SYSTEM_DEPS)
        find_package(rttr CONFIG REQUIRED)
    else()
        FetchContent_Declare(rttr
            GIT_REPOSITORY https://github.com/rttrorg/rttr.git
            GIT_TAG        v0.9.6
            GIT_SHALLOW    TRUE)
        set(BUILD_RTTR_DYNAMIC OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC        ON CACHE BOOL "" FORCE)
        set(BUILD_UNIT_TESTS   OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
        set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
        set(BUILD_INSTALLER    OFF CACHE BOOL "" FORCE)
        set(BUILD_PACKAGE      OFF CACHE BOOL "" FORCE)
        set(BUILD_WITH_STATIC_RUNTIME_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(rttr)
    endif()
endif()
if(TARGET RTTR::Core_Lib)
    set(RPE_RTTR_TARGET RTTR::Core_Lib)
else()
    set(RPE_RTTR_TARGET RTTR::Core)
endif()

if(RPE_WITH_FLECS)
    if(NOT TARGET flecs::flecs AND NOT TARGET flecs::flecs_static)
        if(RPE_USE_SYSTEM_DEPS)
            find_package(flecs CONFIG REQUIRED)
        else()
            FetchContent_Declare(flecs
                GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
                GIT_TAG        v4.1.5
                GIT_SHALLOW    TRUE)
            if(RPE_FLECS_SHARED)
                set(FLECS_SHARED ON  CACHE BOOL "" FORCE)
                set(FLECS_STATIC OFF CACHE BOOL "" FORCE)
            else()
                set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
                set(FLECS_STATIC ON  CACHE BOOL "" FORCE)
            endif()
            FetchContent_MakeAvailable(flecs)
        endif()
    endif()
    # Prefer the shared target — a single flecs instance across DLL boundaries.
    if(TARGET flecs::flecs)
        set(RPE_FLECS_TARGET flecs::flecs)
    else()
        set(RPE_FLECS_TARGET flecs::flecs_static)
    endif()
endif()

# ── rpe_core ─────────────────────────────────────────────────────────────────
# SHARED so the host and every plugin link ONE TypeBridge (and one RTTR) — the
# single-registry guarantee a plugin architecture needs. Depends only on QtCore/
# QtGui + RTTR, so a plugin can link this alone (no Widgets/flecs) just to
# register its component types.
set(RPE_CORE_SOURCES
    src/core/PropertyNode.cpp
    src/core/TypeRenderer.cpp
    src/core/RttrBridge.cpp
    src/core/RttrVariantWrapper.cpp
    src/core/TypeBridge.cpp
    src/core/OptionalSupport.cpp
    src/core/FlagsSupport.cpp)

add_library(rpe_core SHARED ${RPE_CORE_SOURCES})
add_library(rpe::core ALIAS rpe_core)
target_include_directories(rpe_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(rpe_core PUBLIC Qt5::Core Qt5::Gui ${RPE_RTTR_TARGET})
set_target_properties(rpe_core PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON   # export everything on MSVC without annotations
    CXX_VISIBILITY_PRESET default)

# ── rpe_gui ──────────────────────────────────────────────────────────────────
# The widgets (Qt) + optional flecs ECS layer. Host links this.
set(RPE_GUI_SOURCES
    src/gui/PropertyModel.cpp
    src/gui/PropertyDelegate.cpp
    src/gui/VariantEditorFactory.cpp
    src/gui/EditorWidgets.cpp
    src/gui/PropertyEditor.cpp
    src/gui/VariantEditor.cpp
    src/gui/DarkStyle.cpp
    # Q_OBJECT headers listed so AUTOMOC processes them (separate include/ tree).
    include/rpe/gui/PropertyModel.h
    include/rpe/gui/PropertyDelegate.h
    include/rpe/gui/EditorWidgets.h
    include/rpe/gui/PropertyEditor.h
    include/rpe/gui/VariantEditor.h)

if(RPE_WITH_FLECS)
    list(APPEND RPE_GUI_SOURCES
        src/ecs/EntityListWidget.cpp
        src/ecs/ComponentListWidget.cpp
        src/ecs/EntityComponentBrowser.cpp
        src/ecs/PinnedPropertiesWidget.cpp
        src/ecs/EcsMirror.cpp
        src/ecs/MirrorChannel.cpp
        src/ecs/ComponentScan.cpp
        resources/rpe_resources.qrc
        include/rpe/ecs/EntityListWidget.h
        include/rpe/ecs/ComponentListWidget.h
        include/rpe/ecs/EntityComponentBrowser.h
        include/rpe/ecs/PinnedPropertiesWidget.h)
endif()

add_library(rpe_gui STATIC ${RPE_GUI_SOURCES})
add_library(rpe::gui ALIAS rpe_gui)
add_library(rpe::rpe ALIAS rpe_gui)   # backward-compatible umbrella name
target_link_libraries(rpe_gui PUBLIC rpe_core Qt5::Widgets)

if(RPE_WITH_FLECS)
    target_link_libraries(rpe_gui PUBLIC ${RPE_FLECS_TARGET})
    target_compile_definitions(rpe_gui PUBLIC RPE_WITH_FLECS)
endif()

foreach(_t rpe_core rpe_gui)
    if(MSVC)
        # /utf-8: source files are UTF-8; without this MSVC decodes them with the
        # local ANSI codepage and non-ASCII string literals ("…", "›") turn into
        # mojibake ("â€¦", "â€º") in the UI.
        target_compile_options(${_t} PRIVATE /W4 /MP /permissive- /utf-8)
        target_compile_definitions(${_t} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${_t} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# ── Demo / test app ──────────────────────────────────────────────────────────
if(RPE_BUILD_DEMO)
    add_executable(rpe_demo test/main.cpp)
    target_link_libraries(rpe_demo PRIVATE rpe_gui)
endif()

if(RPE_WITH_FLECS AND RPE_BUILD_DEMO)
    add_executable(rpe_fs_path_test test/fs_path_test.cpp)
    target_link_libraries(rpe_fs_path_test PRIVATE rpe_gui)

    add_executable(rpe_array_mirror_test test/array_mirror_test.cpp)
    target_link_libraries(rpe_array_mirror_test PRIVATE rpe_gui)

    add_executable(rpe_list_sort_test test/list_sort_test.cpp)
    target_link_libraries(rpe_list_sort_test PRIVATE rpe_gui)

    add_executable(rpe_filter_test test/filter_test.cpp)
    target_link_libraries(rpe_filter_test PRIVATE rpe_gui)

    add_executable(rpe_select_entity_test test/select_entity_test.cpp)
    target_link_libraries(rpe_select_entity_test PRIVATE rpe_gui)

    add_executable(rpe_writeback_test test/writeback_test.cpp)
    target_link_libraries(rpe_writeback_test PRIVATE rpe_gui)

    add_executable(rpe_variant_editor_test test/variant_editor_test.cpp)
    target_link_libraries(rpe_variant_editor_test PRIVATE rpe_gui)

    add_executable(rpe_scan_budget_test test/scan_budget_test.cpp)
    target_link_libraries(rpe_scan_budget_test PRIVATE rpe_gui)

    add_executable(rpe_tags_pairs_test test/tags_pairs_test.cpp)
    target_link_libraries(rpe_tags_pairs_test PRIVATE rpe_gui)

    add_executable(rpe_pinned_test test/pinned_test.cpp)
    target_link_libraries(rpe_pinned_test PRIVATE rpe_gui)

    add_executable(rpe_pin_pair_test test/pin_pair_test.cpp)
    target_link_libraries(rpe_pin_pair_test PRIVATE rpe_gui)

    add_executable(rpe_plugin_unload_test test/plugin_unload_test.cpp)
    target_link_libraries(rpe_plugin_unload_test PRIVATE rpe_gui)

    add_executable(rpe_remove_flow_test test/remove_flow_test.cpp)
    target_link_libraries(rpe_remove_flow_test PRIVATE rpe_gui)

    add_executable(rpe_entity_delete_test test/entity_delete_test.cpp)
    target_link_libraries(rpe_entity_delete_test PRIVATE rpe_gui)

    add_executable(rpe_context_actions_test test/context_actions_test.cpp)
    target_link_libraries(rpe_context_actions_test PRIVATE rpe_gui)

    add_executable(rpe_spawn_prefab_test test/spawn_prefab_test.cpp)
    target_link_libraries(rpe_spawn_prefab_test PRIVATE rpe_gui)

    add_executable(rpe_entity_add_gui_test test/entity_add_gui_test.cpp)
    target_link_libraries(rpe_entity_add_gui_test PRIVATE rpe_gui)

    add_executable(rpe_pair_live_gui_test test/pair_live_gui_test.cpp)
    target_link_libraries(rpe_pair_live_gui_test PRIVATE rpe_gui)

    add_executable(rpe_entity_selection_stability_test test/entity_selection_stability_test.cpp)
    target_link_libraries(rpe_entity_selection_stability_test PRIVATE rpe_gui)

    add_executable(rpe_pin_cleanup_test test/pin_cleanup_test.cpp)
    target_link_libraries(rpe_pin_cleanup_test PRIVATE rpe_gui)

    add_executable(rpe_entity_filter_clear_test test/entity_filter_clear_test.cpp)
    target_link_libraries(rpe_entity_filter_clear_test PRIVATE rpe_gui)

    add_executable(rpe_entity_selection_mirror_test test/entity_selection_mirror_test.cpp)
    target_link_libraries(rpe_entity_selection_mirror_test PRIVATE rpe_gui)

    add_executable(rpe_entity_delete_refresh_test test/entity_delete_refresh_test.cpp)
    target_link_libraries(rpe_entity_delete_refresh_test PRIVATE rpe_gui)

    add_executable(rpe_reset_live_test test/reset_live_test.cpp)
    target_link_libraries(rpe_reset_live_test PRIVATE rpe_gui)

    add_executable(rpe_dark_style_shot test/dark_style_shot.cpp)
    target_link_libraries(rpe_dark_style_shot PRIVATE rpe_gui)

    add_executable(rpe_optional_test test/optional_test.cpp)
    target_link_libraries(rpe_optional_test PRIVATE rpe_gui)

    add_executable(rpe_display_format_test test/display_format_test.cpp)
    target_link_libraries(rpe_display_format_test PRIVATE rpe_core)

    add_executable(rpe_nested_array_update_test test/nested_array_update_test.cpp)
    target_link_libraries(rpe_nested_array_update_test PRIVATE rpe_gui)

    add_executable(rpe_struct_summary_test test/struct_summary_test.cpp)
    target_link_libraries(rpe_struct_summary_test PRIVATE rpe_gui)

    add_executable(rpe_property_copy_test test/property_copy_test.cpp)
    target_link_libraries(rpe_property_copy_test PRIVATE rpe_gui)

    add_executable(rpe_property_copy_shot test/property_copy_shot.cpp)
    target_link_libraries(rpe_property_copy_shot PRIVATE rpe_gui)

    add_executable(rpe_type_support_test test/type_support_test.cpp)
    target_link_libraries(rpe_type_support_test PRIVATE rpe_gui)

    add_executable(rpe_flags_test test/flags_test.cpp)
    target_link_libraries(rpe_flags_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_test test/mirror_test.cpp)
    target_link_libraries(rpe_mirror_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_runtime_attach_test test/mirror_runtime_attach_test.cpp)
    target_link_libraries(rpe_mirror_runtime_attach_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_lifetime_test test/mirror_lifetime_test.cpp)
    target_link_libraries(rpe_mirror_lifetime_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_teardown_test test/mirror_teardown_test.cpp)
    target_link_libraries(rpe_mirror_teardown_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_threaded_test test/mirror_threaded_test.cpp)
    target_link_libraries(rpe_mirror_threaded_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_manual_test test/mirror_manual_test.cpp)
    target_link_libraries(rpe_mirror_manual_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_concurrent_destroy_test test/mirror_concurrent_destroy_test.cpp)
    target_link_libraries(rpe_mirror_concurrent_destroy_test PRIVATE rpe_gui)

    add_executable(rpe_browser_mirror_gui_test test/browser_mirror_gui_test.cpp)
    target_link_libraries(rpe_browser_mirror_gui_test PRIVATE rpe_gui)

    add_executable(rpe_typebridge_test test/typebridge_test.cpp)
    target_link_libraries(rpe_typebridge_test PRIVATE rpe_gui)

    add_executable(rpe_component_edit_test test/component_edit_test.cpp)
    target_link_libraries(rpe_component_edit_test PRIVATE rpe_gui)

    add_executable(rpe_screenshot test/screenshot_tool.cpp)
    target_link_libraries(rpe_screenshot PRIVATE rpe_gui)

    add_executable(rpe_feature_shot test/feature_shot.cpp)
    target_link_libraries(rpe_feature_shot PRIVATE rpe_gui)

    add_executable(rpe_context_menu_shot test/context_menu_shot.cpp)
    target_link_libraries(rpe_context_menu_shot PRIVATE rpe_gui)

    add_executable(rpe_add_button_shot test/add_button_shot.cpp)
    target_link_libraries(rpe_add_button_shot PRIVATE rpe_gui)

    # Dev-only: regenerates resources/icons/*.png (the committed icons the GUI uses).
    add_executable(rpe_makeicons tools/make_icons.cpp)
    target_link_libraries(rpe_makeicons PRIVATE Qt5::Gui)

    add_executable(rpe_rttr_order_test test/rttr_order_test.cpp)
    target_link_libraries(rpe_rttr_order_test PRIVATE rpe_core)

    add_executable(rpe_mirror_bench test/mirror_bench.cpp)
    target_link_libraries(rpe_mirror_bench PRIVATE rpe_gui)

    add_executable(rpe_flecs_naming_test test/flecs_naming_test.cpp)
    target_link_libraries(rpe_flecs_naming_test PRIVATE rpe_gui)

    add_executable(rpe_namespace_carryover_test test/namespace_carryover_test.cpp)
    target_link_libraries(rpe_namespace_carryover_test PRIVATE rpe_gui)

    add_executable(rpe_namespaced_components_gui_test test/namespaced_components_gui_test.cpp)
    target_link_libraries(rpe_namespaced_components_gui_test PRIVATE rpe_gui)
endif()

# ── IDE target organisation ──────────────────────────────────────────────────
# Group every target this project defines under an "rpe" folder in IDEs that
# show a target tree (Visual Studio, Xcode, some Qt Creator/CLion views), so the
# ~30 tests/tools/libs don't clutter the top level. Only targets declared in
# THIS directory are swept — FetchContent's rttr/flecs live in their own
# subdirectories and are left as-is.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
get_property(_rpe_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _rpe_targets)
    get_target_property(_ttype ${_t} TYPE)
    if(_ttype STREQUAL "EXECUTABLE" AND NOT _t STREQUAL "rpe_demo")
        # tests, shots, benches and dev tools
        set_target_properties(${_t} PROPERTIES FOLDER "rpe/tests")
    else()
        # rpe_core, rpe_gui, rpe_demo
        set_target_properties(${_t} PROPERTIES FOLDER "rpe")
    endif()
endforeach()
