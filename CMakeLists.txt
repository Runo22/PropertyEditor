cmake_minimum_required(VERSION 3.21)
project(rpe LANGUAGES CXX)

# ── Options ──────────────────────────────────────────────────────────────────
option(RPE_WITH_FLECS      "Build the flecs ECS integration layer" ON)
option(RPE_BUILD_DEMO      "Build the demo/test application"       ON)
option(RPE_USE_SYSTEM_DEPS "Resolve rttr/flecs via find_package instead of FetchContent" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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
    if(NOT TARGET flecs::flecs_static AND NOT TARGET flecs::flecs)
        if(RPE_USE_SYSTEM_DEPS)
            find_package(flecs CONFIG REQUIRED)
        else()
            FetchContent_Declare(flecs
                GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
                GIT_TAG        v4.0.3
                GIT_SHALLOW    TRUE)
            set(FLECS_STATIC ON  CACHE BOOL "" FORCE)
            set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(flecs)
        endif()
    endif()
    if(TARGET flecs::flecs_static)
        set(RPE_FLECS_TARGET flecs::flecs_static)
    else()
        set(RPE_FLECS_TARGET flecs::flecs)
    endif()
endif()

# ── Library ──────────────────────────────────────────────────────────────────
set(RPE_SOURCES
    src/core/PropertyNode.cpp
    src/core/TypeRenderer.cpp
    src/core/RttrBridge.cpp
    src/core/RttrVariantWrapper.cpp
    src/core/TypeBridge.cpp
    src/gui/PropertyModel.cpp
    src/gui/PropertyDelegate.cpp
    src/gui/EditorWidgets.cpp
    src/gui/PropertyEditor.cpp
    src/gui/VariantEditor.cpp)

# Headers carrying Q_OBJECT are listed so AUTOMOC processes them (they live in a
# separate include/ tree, so basename auto-detection alone is not enough).
list(APPEND RPE_SOURCES
    include/rpe/gui/PropertyModel.h
    include/rpe/gui/PropertyDelegate.h
    include/rpe/gui/EditorWidgets.h
    include/rpe/gui/PropertyEditor.h
    include/rpe/gui/VariantEditor.h)

if(RPE_WITH_FLECS)
    list(APPEND RPE_SOURCES
        src/ecs/EntityListWidget.cpp
        src/ecs/ComponentListWidget.cpp
        src/ecs/EntityComponentBrowser.cpp
        include/rpe/ecs/EntityListWidget.h
        include/rpe/ecs/ComponentListWidget.h
        include/rpe/ecs/EntityComponentBrowser.h)
endif()

add_library(rpe STATIC ${RPE_SOURCES})
add_library(rpe::rpe ALIAS rpe)

target_include_directories(rpe PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(rpe PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets ${RPE_RTTR_TARGET})

if(RPE_WITH_FLECS)
    target_link_libraries(rpe PUBLIC ${RPE_FLECS_TARGET})
    target_compile_definitions(rpe PUBLIC RPE_WITH_FLECS)
endif()

if(MSVC)
    target_compile_options(rpe PRIVATE /W4 /MP /permissive-)
    target_compile_definitions(rpe PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(rpe PRIVATE -Wall -Wextra)
endif()

# ── Demo / test app ──────────────────────────────────────────────────────────
if(RPE_BUILD_DEMO)
    add_executable(rpe_demo test/main.cpp)
    target_link_libraries(rpe_demo PRIVATE rpe)
endif()
