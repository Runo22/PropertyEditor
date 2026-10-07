# reflect_generate(<target> [DEFAULT_FIELDS explicit|all] [HEADERS h1 h2 ...])
#
# Runs reflectgen on every header of <target> that contains REFLECT( and adds the
# generated <header>.gen.cpp files to <target>. Call it on the module that OWNS the
# types — the exe or the plugin DLL itself, never a static library shared by
# several DLLs (each module keeps its own registration list).
#
#   add_subdirectory(tools/reflectgen)            # builds reflectgen + reflect::runtime
#   add_library(game_plugin SHARED ...)
#   target_link_libraries(game_plugin PRIVATE reflect::runtime rpe::core flecs::flecs)
#   reflect_generate(game_plugin)
#
# Headers are picked at CONFIGURE time (cheap text scan), so re-run CMake after
# adding the first REFLECT to a header that had none. Edits to a header that is
# already picked are tracked by Ninja (header + everything it includes, via the
# depfile); an unchanged result is not rewritten, so nothing downstream rebuilds.

function(reflect_generate target)
    cmake_parse_arguments(RG "" "DEFAULT_FIELDS" "HEADERS" ${ARGN})

    if(TARGET reflectgen)
        set(_tool $<TARGET_FILE:reflectgen>)
        set(_tool_dep reflectgen)
    else()
        find_program(REFLECTGEN_EXECUTABLE reflectgen REQUIRED)
        set(_tool ${REFLECTGEN_EXECUTABLE})
        set(_tool_dep ${REFLECTGEN_EXECUTABLE})
    endif()

    if(NOT RG_HEADERS)
        get_target_property(_srcs ${target} SOURCES)
        foreach(_s IN LISTS _srcs)
            if(_s MATCHES "\\.(h|hh|hpp|hxx)$")
                list(APPEND RG_HEADERS ${_s})
            endif()
        endforeach()
    endif()

    set(_extra)
    if(RG_DEFAULT_FIELDS)
        list(APPEND _extra --default-fields ${RG_DEFAULT_FIELDS})
    endif()

    # Parse the header with the same include paths / defines the target compiles with
    # (transitive usage requirements included).
    set(_inc "$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>")
    set(_def "$<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>")
    if(MSVC)
        set(_mode --driver-mode=cl /std:c++latest /EHsc)
        set(_I /I)
        set(_D /D)
    else()
        set(_std ${CMAKE_CXX_STANDARD})
        if(NOT _std)
            set(_std 20)
        endif()
        set(_mode -std=c++${_std})
        set(_I -I)
        set(_D -D)
    endif()

    set(_outdir ${CMAKE_CURRENT_BINARY_DIR}/reflect_gen/${target})
    foreach(_h IN LISTS RG_HEADERS)
        get_filename_component(_abs "${_h}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        file(STRINGS "${_abs}" _hit REGEX "REFLECT[ \t]*\\(" LIMIT_COUNT 1)
        if(NOT _hit)
            continue()
        endif()
        file(RELATIVE_PATH _rel ${CMAKE_CURRENT_SOURCE_DIR} ${_abs})
        string(REPLACE ".." "__" _rel "${_rel}")
        set(_out ${_outdir}/${_rel}.gen.cpp)

        add_custom_command(
            OUTPUT ${_out}
            COMMAND ${_tool} ${_abs} -o ${_out} --depfile ${_out}.d --include "\"${_abs}\"" ${_extra}
                    -- ${_mode}
                    "$<$<BOOL:${_inc}>:${_I}$<JOIN:${_inc},;${_I}>>"
                    "$<$<BOOL:${_def}>:${_D}$<JOIN:${_def},;${_D}>>"
            DEPENDS ${_abs} ${_tool_dep}
            DEPFILE ${_out}.d
            COMMENT "reflectgen ${_rel}"
            COMMAND_EXPAND_LISTS
            VERBATIM)
        target_sources(${target} PRIVATE ${_out})
    endforeach()
endfunction()
