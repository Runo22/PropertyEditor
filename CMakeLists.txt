cmake_minimum_required(VERSION 3.25)
project(rttr_property_editor CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

# Dependencies via FetchContent
include(FetchContent)

FetchContent_Declare(
    rttr
    GIT_REPOSITORY https://github.com/rttrorg/rttr.git
    GIT_TAG        v0.9.6
    GIT_SHALLOW    TRUE
)
set(BUILD_RTTR_DYNAMIC    OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS      OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_STATIC_RUNTIME_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION   OFF CACHE BOOL "" FORCE)
set(BUILD_INSTALLER       OFF CACHE BOOL "" FORCE)
set(BUILD_PACKAGE         OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rttr)

FetchContent_Declare(
    flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG        v4.0.3
    GIT_SHALLOW    TRUE
)
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flecs)

# ── Static library ─────────────────────────────────────────────────────────────
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS
    "src/core/*.cpp"
    "src/widgets/*.cpp"
    "src/ecs/*.cpp"
)

add_library(rttr_property_editor STATIC ${LIB_SOURCES})

target_include_directories(rttr_property_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rttr_property_editor
    PUBLIC
        Qt5::Core
        Qt5::Widgets
        RTTR::Core_Lib
)

# ECS layer links flecs only if sources exist in src/ecs
target_link_libraries(rttr_property_editor
    PUBLIC
        flecs::flecs_static
)

if(MSVC)
    target_compile_options(rttr_property_editor PRIVATE /W4 /MP)
    target_compile_definitions(rttr_property_editor PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# ── Test executable ────────────────────────────────────────────────────────────
add_executable(rttr_property_editor_test test/main.cpp)

target_link_libraries(rttr_property_editor_test
    PRIVATE
        rttr_property_editor
)

if(MSVC)
    target_compile_options(rttr_property_editor_test PRIVATE /W4 /MP)
    target_compile_definitions(rttr_property_editor_test PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    set_target_properties(rttr_property_editor_test PROPERTIES
        WIN32_EXECUTABLE FALSE   # keep console for test output
    )
endif()
