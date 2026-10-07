cmake_minimum_required(VERSION 3.20)
project(reflectgen LANGUAGES CXX)

# reflectgen only needs the libclang C API. On Windows the official LLVM installer
# ships it: <LLVM>/include/clang-c, <LLVM>/lib/libclang.lib, <LLVM>/bin/libclang.dll.
# Point LLVM_ROOT at the install if it is not in the default location.
set(LLVM_ROOT "" CACHE PATH "LLVM install prefix (contains include/clang-c and lib/libclang)")

file(GLOB _llvm_linux_roots /usr/lib/llvm-*)
set(_llvm_hints ${LLVM_ROOT} "$ENV{LLVM_ROOT}" "C:/Program Files/LLVM" ${_llvm_linux_roots} /usr/local/opt/llvm)

find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS ${_llvm_hints} PATH_SUFFIXES include REQUIRED)
find_library(LIBCLANG_LIBRARY NAMES libclang clang HINTS ${_llvm_hints} PATH_SUFFIXES lib REQUIRED)

add_executable(reflectgen src/reflectgen.cpp)
target_compile_features(reflectgen PRIVATE cxx_std_20)
target_include_directories(reflectgen PRIVATE ${LIBCLANG_INCLUDE_DIR})
target_link_libraries(reflectgen PRIVATE ${LIBCLANG_LIBRARY})

if(WIN32)
    # libclang.dll must sit next to reflectgen.exe.
    get_filename_component(_llvm_prefix "${LIBCLANG_INCLUDE_DIR}" DIRECTORY)
    find_file(LIBCLANG_DLL libclang.dll HINTS "${_llvm_prefix}/bin" REQUIRED)
    add_custom_command(TARGET reflectgen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_DLL}" "$<TARGET_FILE_DIR:reflectgen>")
else()
    get_filename_component(_libclang_dir "${LIBCLANG_LIBRARY}" DIRECTORY)
    set_target_properties(reflectgen PROPERTIES BUILD_RPATH "${_libclang_dir}")
endif()

# Header-only runtime for the generated code (reflect/Reflect.h, reflect/Runtime.h).
add_library(reflect_runtime INTERFACE)
add_library(reflect::runtime ALIAS reflect_runtime)
target_include_directories(reflect_runtime INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReflectGen.cmake)
