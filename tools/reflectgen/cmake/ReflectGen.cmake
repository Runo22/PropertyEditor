# reflect_generate(<target>
#     [DEFAULT_FIELDS explicit|all]   # field selection for REFLECT types without all/explicit
#     [HEADERS h1 h2 ...]             # default: the target's .h/.hh/.hpp/.hxx sources
#     [PREINCLUDE h1 h2 ...])         # headers the target's sources always see first
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
# Headers are parsed with the target's own include dirs, defines, compile options,
# C++ standard, CMAKE_CXX_FLAGS and PRECOMPILE_HEADERS (as forced includes), so a
# header that is only complete inside the target's build still parses; reflectgen
# keeps just the options that affect parsing. They reach it through a response
# file written with file(GENERATE ... TARGET), which evaluates $<COMPILE_LANGUAGE>
# conditions as CXX (a custom command would silently drop them) and keeps long
# include lists clear of command-line length limits.
#
# Headers are picked at CONFIGURE time (cheap text scan), so re-run CMake after
# adding the first REFLECT to a header that had none. Edits to a header that is
# already picked are tracked by Ninja (header + everything it includes, via the
# depfile); an unchanged result is not rewritten, so nothing downstream rebuilds.

function(reflect_generate target)
    cmake_parse_arguments(RG "" "DEFAULT_FIELDS" "HEADERS;PREINCLUDE" ${ARGN})

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

    set(_outdir ${CMAKE_CURRENT_BINARY_DIR}/reflect_gen/${target})
    _reflect_write_args(${target} "${_outdir}" "${RG_PREINCLUDE}" _rsp)

    foreach(_h IN LISTS RG_HEADERS)
        get_filename_component(_abs "${_h}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        file(STRINGS "${_abs}" _hit REGEX "REFLECT[ \t]*\\(" LIMIT_COUNT 1)
        if(NOT _hit)
            continue()
        endif()
        _reflect_out_name("${_abs}" "${_outdir}" _out _rel)
        add_custom_command(
            OUTPUT ${_out}
            COMMAND ${_tool} ${_abs} -o ${_out} --depfile ${_out}.d --include "\"${_abs}\"" ${_extra} -- @${_rsp}
            DEPENDS ${_abs} ${_tool_dep} ${_rsp}
            DEPFILE ${_out}.d
            COMMENT "reflectgen ${_rel}"
            VERBATIM)
        target_sources(${target} PRIVATE ${_out})
    endforeach()
endfunction()

# <outdir>/<header path relative to the source dir>.gen.cpp, safe for headers
# outside the source tree and on another drive.
function(_reflect_out_name abs outdir out_var rel_var)
    file(RELATIVE_PATH _rel ${CMAKE_CURRENT_SOURCE_DIR} ${abs})
    string(REPLACE ".." "__" _rel "${_rel}")
    string(REPLACE ":" "_" _rel "${_rel}")
    set(${out_var} ${outdir}/${_rel}.gen.cpp PARENT_SCOPE)
    set(${rel_var} ${_rel} PARENT_SCOPE)
endfunction()

# Writes the target's parse arguments (one per line) and returns the file's path
# for the current configuration.
function(_reflect_write_args target outdir preincludes out_var)
    set(_t ${target})
    set(_std "$<TARGET_PROPERTY:${_t},CXX_STANDARD>")
    set(_inc "$<TARGET_PROPERTY:${_t},INCLUDE_DIRECTORIES>")
    set(_def "$<TARGET_PROPERTY:${_t},COMPILE_DEFINITIONS>")
    set(_opt "$<TARGET_PROPERTY:${_t},COMPILE_OPTIONS>")
    set(_pch "$<TARGET_PROPERTY:${_t},PRECOMPILE_HEADERS>")

    if(MSVC)
        set(_I /I)
        set(_D /D)
        set(_FI /FI)
        # clang-cl knows /std:c++14 /std:c++17 /std:c++20 /std:c++latest
        set(_lines "--driver-mode=cl" "/EHsc"
            "/std:c++$<IF:$<OR:$<STREQUAL:${_std},>,$<VERSION_GREATER_EQUAL:${_std},23>>,latest,${_std}>")
    else()
        set(_I -I)
        set(_D -D)
        set(_FI -include)
        set(_lines "-std=c++$<IF:$<STREQUAL:${_std},>,20,${_std}>")
    endif()

    # CMAKE_CXX_FLAGS (+ the build type's) are not target properties.
    set(_flags "${CMAKE_CXX_FLAGS}")
    if(CMAKE_BUILD_TYPE)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
        string(APPEND _flags " ${CMAKE_CXX_FLAGS_${_bt}}")
    endif()
    separate_arguments(_flags NATIVE_COMMAND "${_flags}")
    list(APPEND _lines ${_flags})

    set(_pre)
    foreach(_p IN LISTS preincludes)
        get_filename_component(_p "${_p}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND _pre ${_FI} ${_p})
    endforeach()

    string(JOIN "\n" _content ${_lines} ${_pre})
    string(APPEND _content "\n"
        "$<$<BOOL:${_inc}>:${_I}$<JOIN:${_inc},\n${_I}>>\n"
        "$<$<BOOL:${_def}>:${_D}$<JOIN:${_def},\n${_D}>>\n"
        "$<JOIN:${_opt},\n>\n"
        "$<$<BOOL:${_pch}>:${_FI}\n$<JOIN:${_pch},\n${_FI}\n>>\n")

    # Per configuration AND language: evaluated with COMPILE_LANGUAGE=CXX in the
    # _CXX file (the _C file, if the project enables C, is simply unused).
    set(_base ${outdir}/reflectgen_args_$<CONFIG>)
    file(GENERATE OUTPUT ${_base}_$<COMPILE_LANGUAGE>.rsp CONTENT "${_content}" TARGET ${_t})
    set(${out_var} ${_base}_CXX.rsp PARENT_SCOPE)
endfunction()
