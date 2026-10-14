set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR m68k)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT SHELLTOOLS_TOOLCHAIN_ROOT AND DEFINED ENV{SHELLTOOLS_TOOLCHAIN_ROOT})
    set(SHELLTOOLS_TOOLCHAIN_ROOT "$ENV{SHELLTOOLS_TOOLCHAIN_ROOT}" CACHE PATH
        "AmigaPorts toolchain root")
endif()

if(SHELLTOOLS_TOOLCHAIN_ROOT)
    set(_bin "${SHELLTOOLS_TOOLCHAIN_ROOT}/bin")
    set(_cxx "${_bin}/m68k-amigaos-c++")
else()
    find_program(_cxx m68k-amigaos-c++ REQUIRED)
    get_filename_component(_bin "${_cxx}" DIRECTORY)
    get_filename_component(SHELLTOOLS_TOOLCHAIN_ROOT "${_bin}/.." ABSOLUTE)
    set(SHELLTOOLS_TOOLCHAIN_ROOT "${SHELLTOOLS_TOOLCHAIN_ROOT}" CACHE PATH
        "AmigaPorts toolchain root")
endif()

if(NOT EXISTS "${_cxx}")
    message(FATAL_ERROR "No m68k-amigaos-c++ at ${_cxx}")
endif()

execute_process(COMMAND "${_cxx}" --version
    OUTPUT_VARIABLE _compiler_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _compiler_rc)
if(NOT _compiler_rc EQUAL 0)
    message(FATAL_ERROR "The selected Amiga compiler does not run on this host")
endif()
string(REGEX MATCH "AmigaDev v([0-9]+)\\.([0-9]+)-rc([0-9]+)" _release "${_compiler_version}")
if(NOT _release OR CMAKE_MATCH_1 LESS 16 OR
   (CMAKE_MATCH_1 EQUAL 16 AND CMAKE_MATCH_2 LESS 2) OR
   (CMAKE_MATCH_1 EQUAL 16 AND CMAKE_MATCH_2 EQUAL 2 AND CMAKE_MATCH_3 LESS 16))
    message(FATAL_ERROR "Need AmigaPorts GCC 16.2-rc16 or later; got ${_compiler_version}")
endif()

set(CMAKE_CXX_COMPILER "${_cxx}")
set(CMAKE_AR "${_bin}/m68k-amigaos-gcc-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_bin}/m68k-amigaos-gcc-ranlib" CACHE FILEPATH "")
set(CMAKE_NM "${_bin}/m68k-amigaos-gcc-nm" CACHE FILEPATH "")
set(CMAKE_STRIP "${_bin}/m68k-amigaos-strip" CACHE FILEPATH "")
message(STATUS "AmigaPorts toolchain: ${SHELLTOOLS_TOOLCHAIN_ROOT} (${_release})")
