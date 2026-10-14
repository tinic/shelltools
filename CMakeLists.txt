cmake_minimum_required(VERSION 3.25)
project(shelltools LANGUAGES CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Configure with cmake/toolchain-m68k-amigaos.cmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHELLTOOLS_ANALYZE "Build with GCC static analyzer instead of LTO" OFF)

set(_warning_flags
    -Wall -Wextra -Werror -Wpedantic -Weffc++ -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
    -Wconversion -Wsign-conversion -Wmisleading-indentation
    -Wduplicated-cond -Wduplicated-branches -Wlogical-op
    -Wnull-dereference -Wuseless-cast -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough)

if(SHELLTOOLS_ANALYZE)
    set(_analysis_flags -fanalyzer -fno-lto)
    set(_lto_flags -fno-lto)
else()
    set(_analysis_flags)
    set(_lto_flags -flto)
endif()

set(SHELLTOOLS_CPU "68000" CACHE STRING "Target CPU: 68000 or 68020-60")
set_property(CACHE SHELLTOOLS_CPU PROPERTY STRINGS 68000 68020-60)
if(SHELLTOOLS_CPU STREQUAL "68000")
    set(_cpu_flag -m68000)
elseif(SHELLTOOLS_CPU STREQUAL "68020-60")
    set(_cpu_flag -m68020-60)
else()
    message(FATAL_ERROR "SHELLTOOLS_CPU must be 68000 or 68020-60")
endif()

function(shelltool name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE
        ${_cpu_flag} -mcrt=nix20 -Os ${_lto_flags} ${_analysis_flags}
        -fno-rtti -fno-exceptions
        -fno-threadsafe-statics -ffunction-sections -fdata-sections
        ${_warning_flags})
    target_link_options(${name} PRIVATE
        ${_cpu_flag} -mcrt=nix20 -Os ${_lto_flags}
        -fno-rtti -fno-exceptions
        -Wl,--gc-sections)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/C")
    install(TARGETS ${name} RUNTIME DESTINATION C)
endfunction()

shelltool(ShowTasks src/show_tasks.cpp src/task_snapshot.cpp src/task_output.cpp)
shelltool(WatchTasks src/watch_tasks.cpp src/task_snapshot.cpp src/task_output.cpp)

if(SHELLTOOLS_ANALYZE)
    find_program(CPPCHECK_EXECUTABLE cppcheck REQUIRED)
    add_custom_target(cppcheck ALL
        COMMAND "${CPPCHECK_EXECUTABLE}"
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
            --platform=${CMAKE_SOURCE_DIR}/cmake/cppcheck-m68k.xml
            --enable=all --inconclusive --check-level=exhaustive
            --inline-suppr
            --suppress=missingIncludeSystem --suppress=checkersReport
            --error-exitcode=1
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM)
endif()

set(SHELLTOOLS_VAMOS "" CACHE FILEPATH "Path to Vamos for target smoke tests")
if(SHELLTOOLS_VAMOS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME amiga-smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/vamos_smoke.py"
            --vamos "${SHELLTOOLS_VAMOS}" --bin-dir "${CMAKE_BINARY_DIR}/C"
            --cpu "$<IF:$<STREQUAL:${SHELLTOOLS_CPU},68000>,68000,68020>")
endif()
