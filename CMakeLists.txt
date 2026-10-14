cmake_minimum_required(VERSION 3.25)
project(shelltools LANGUAGES CXX)

if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Configure with cmake/toolchain-m68k-amigaos.cmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
        ${_cpu_flag} -mcrt=nix20 -Os -flto -fno-rtti -fno-exceptions
        -fno-threadsafe-statics -ffunction-sections -fdata-sections
        -Wall -Wextra -Werror)
    target_link_options(${name} PRIVATE
        ${_cpu_flag} -mcrt=nix20 -Os -flto -fno-rtti -fno-exceptions
        -Wl,--gc-sections)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/C")
    install(TARGETS ${name} RUNTIME DESTINATION C)
endfunction()

shelltool(ShowTasks src/show_tasks.cpp src/task_snapshot.cpp src/task_output.cpp)
shelltool(WatchTasks src/watch_tasks.cpp src/task_snapshot.cpp src/task_output.cpp)
shelltool(BreakTask src/break_task.cpp)
shelltool(FirstLines src/first_lines.cpp)
shelltool(LastLines src/last_lines.cpp)
shelltool(FindText src/find_text.cpp)
shelltool(DiskFree src/disk_free.cpp)
shelltool(SystemInfo src/system_info.cpp)
shelltool(RebootSync src/reboot_sync.cpp)

set(SHELLTOOLS_VAMOS "" CACHE FILEPATH "Path to Vamos for target smoke tests")
if(SHELLTOOLS_VAMOS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME amiga-smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/vamos_smoke.py"
            --vamos "${SHELLTOOLS_VAMOS}" --bin-dir "${CMAKE_BINARY_DIR}/C"
            --cpu "$<IF:$<STREQUAL:${SHELLTOOLS_CPU},68000>,68000,68020>")
endif()
