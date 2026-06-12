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
    src/core/TypeBridge.cpp)

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
    src/gui/EditorWidgets.cpp
    src/gui/PropertyEditor.cpp
    src/gui/VariantEditor.cpp
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
        src/ecs/EcsMirror.cpp
        include/rpe/ecs/EntityListWidget.h
        include/rpe/ecs/ComponentListWidget.h
        include/rpe/ecs/EntityComponentBrowser.h)
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
        target_compile_options(${_t} PRIVATE /W4 /MP /permissive-)
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
    add_executable(rpe_mirror_test test/mirror_test.cpp)
    target_link_libraries(rpe_mirror_test PRIVATE rpe_gui)

    add_executable(rpe_mirror_runtime_attach_test test/mirror_runtime_attach_test.cpp)
    target_link_libraries(rpe_mirror_runtime_attach_test PRIVATE rpe_gui)
endif()
