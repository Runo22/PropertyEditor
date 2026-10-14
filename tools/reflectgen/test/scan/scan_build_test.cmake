# ctest: with SCAN BUILD, adding REFLECT to a header that had none must be picked up
# by the next build alone — no CMake re-run. Then the header is restored.
#   cmake -DBUILD_DIR=... -DHEADER=... -DGEN=... -P scan_build_test.cmake
foreach(v BUILD_DIR HEADER GEN)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} not set")
    endif()
endforeach()

file(READ ${BUILD_DIR}/build.ninja _ninja_before)
set(_plain "#pragma once\n// starts without the macro; scan_build_test.cmake adds it\nnamespace scanprobe { struct Later { int b = 0; }; }\n")
set(_reflected "#pragma once\n#include <reflect/Reflect.h>\nnamespace scanprobe { struct REFLECT() Later { PROP(min = 1) int b = 1; }; }\n")

function(build)
    execute_process(COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target scan_probe
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "build failed:\n${out}")
    endif()
endfunction()

file(WRITE ${HEADER} "${_reflected}")
build()
file(READ ${GEN} _gen)
if(NOT _gen MATCHES "Access<::scanprobe::Later>")
    file(WRITE ${HEADER} "${_plain}")
    message(FATAL_ERROR "REFLECT added after configure was not generated:\n${_gen}")
endif()
file(READ ${BUILD_DIR}/build.ninja _ninja_after)
if(NOT _ninja_before STREQUAL _ninja_after)
    message(FATAL_ERROR "CMake re-ran; SCAN BUILD should not need it")
endif()
message(STATUS "PASS: new REFLECT picked up by the build alone")

file(WRITE ${HEADER} "${_plain}")
build()
file(READ ${GEN} _gen)
if(_gen MATCHES "Access<")
    message(FATAL_ERROR "REFLECT removed but registration still generated:\n${_gen}")
endif()
message(STATUS "PASS: REFLECT removed again, generated file back to empty")
