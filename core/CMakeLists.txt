cmake_minimum_required(VERSION 3.21)

# Single source of truth for the version.
#
# cozip/VERSION lives one level up. ALL bindings (this C library, the
# Python wheel, R package, Julia package, and npm package) read the
# same file so they cannot drift apart.
#
# CalVer of the form YYYY.M.D parses fine into CMake's MAJOR.MINOR.PATCH.

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" COZIP_VERSION_RAW)
string(STRIP "${COZIP_VERSION_RAW}" COZIP_VERSION)

project(cozip
    VERSION      ${COZIP_VERSION}
    DESCRIPTION  "Cloud-Optimized ZIP core"
    LANGUAGES    C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# IMPORTANT: do NOT set CMAKE_C_STANDARD / CMAKE_C_EXTENSIONS / warning
# flags / visibility presets globally — those would propagate to the
# vendored zlib and libzip subdirectories and break their builds. All
# such options are applied to the cozip target only, further down.

option(COZIP_INSTALL     "Generate install / export targets" ON)
option(COZIP_BUILD_TESTS "Build the test suite"              OFF)
# The Flat reader fetches bytes through the vendored karu, which needs C++23,
# libcurl and OpenSSL. OFF keeps a writer-only C library with no system deps.
option(COZIP_READER      "Build the Flat reader"             ON)


# ----------------------------------------------------------------------
# Generate version.h from version.h.in
# ----------------------------------------------------------------------

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY)


# ----------------------------------------------------------------------
# Vendored deps — sources live in core/zlib/ and core/libzip/
#
# These are NOT downloaded at build time. They are committed to the
# repo so this project is standalone (no FetchContent, no network in
# CI, no race conditions with upstream tags).
#
# When upgrading either library:
#   1. Drop the new sources into core/{zlib,libzip}/.
#   2. Update zipconf.h, zip_err_str.c, zconf.h from the upstream build.
#   3. Diff core/libzip/config.h against the new cmake-config.h.in.
#   4. Run `make lib` and fix anything that breaks.
#   5. Commit. No script, no automation — once or twice a year.
#
# add_subdirectory() runs core/{zlib,libzip}/CMakeLists.txt, which
# define the `zlibstatic` and `zip` static-library targets and the
# `ZLIB::ZLIB` alias that libzip's internal find_package would
# otherwise look up.
# ----------------------------------------------------------------------

add_subdirectory(zlib)
add_subdirectory(libzip)

# core/karu/ is a copy of a tagged karu release, a static C++23 library with
# hidden visibility that links libcurl and OpenSSL.
if(EMSCRIPTEN)
    set(COZIP_READER OFF)
endif()
if(COZIP_READER)
    enable_language(CXX)
    set(KARU_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
    set(KARU_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    add_subdirectory(karu EXCLUDE_FROM_ALL)
endif()


# ----------------------------------------------------------------------
# cozip itself
# ----------------------------------------------------------------------

add_library(cozip SHARED cozip.c cozip_read.c)
add_library(cozip::cozip ALIAS cozip)

target_compile_definitions(cozip PRIVATE COZIP_BUILDING)

# Without the reader, cozip_read.c keeps the location mapping, and reading an
# archive reports that the build has no reader.
if(COZIP_READER)
    target_compile_definitions(cozip PRIVATE COZIP_READER)
    target_link_libraries(cozip PRIVATE karu::karu)
    set_target_properties(cozip PROPERTIES LINKER_LANGUAGE CXX)

    # Release builds link curl and OpenSSL statically. Only cozip_* symbols
    # leave the library, so a host's own copies never bind to ours.
    if(APPLE)
        target_link_options(cozip PRIVATE "LINKER:-exported_symbol,_cozip_*")
    elseif(UNIX)
        target_link_options(cozip PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
endif()

# C standard — applied only to cozip, NOT propagated to vendored deps.
target_compile_features(cozip PRIVATE c_std_11)

# Strict warnings for our own code only. libzip and zlib silence their
# own warnings inside their CMakeLists, so this list applies only here.
if(MSVC)
    target_compile_options(cozip PRIVATE
        /W4 /permissive- /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cozip PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Symbol visibility.
#
# C_VISIBILITY_PRESET=hidden hides every symbol by default; cozip.h's
# COZIP_API macro re-exports the public API surface explicitly via
# __attribute__((visibility("default"))) on Unix and __declspec(dllexport)
# on Windows.
#
# WINDOWS_EXPORT_ALL_SYMBOLS=OFF: with COZIP_API in place, exports are
# explicit. ON would also export libzip/zlib symbols (statically linked
# into us), polluting the ABI surface.
set_target_properties(cozip PROPERTIES
    C_VISIBILITY_PRESET        hidden
    VISIBILITY_INLINES_HIDDEN  ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)

target_include_directories(cozip
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>)

# Link the vendored libzip statically. libzip in turn pulls in zlib
# via its PUBLIC link to ZLIB::ZLIB — we don't need to list zlib here.
target_link_libraries(cozip PRIVATE zip)

set_target_properties(cozip PROPERTIES
    VERSION       ${PROJECT_VERSION}
    PUBLIC_HEADER cozip.h
    OUTPUT_NAME   cozip
    PREFIX        "")

# SOVERSION skipped on Apple (universal2 lipo issues; install_name
# already encodes VERSION). With CalVer, MAJOR is the year.
if(NOT APPLE)
    set_target_properties(cozip PROPERTIES
        SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

# rpath: artifact looks for sibling libs in @rpath / $ORIGIN.
if(APPLE)
    set_target_properties(cozip PROPERTIES
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_RPATH TRUE)
elseif(UNIX)
    set_target_properties(cozip PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# Disable install/export under Emscripten — WASM builds don't get
# "installed" as system libraries; the .wasm artifact is loaded
# directly by the host (browser, node, webR). The export set also
# fails Generate because the static deps (zip, zlibstatic) aren't
# in any install set.
if(EMSCRIPTEN)
    set(COZIP_INSTALL OFF)
endif()

if(COZIP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS cozip
        EXPORT        cozipTargets
        LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/version.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT cozipTargets
        FILE        cozipTargets.cmake
        NAMESPACE   cozip::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cozip)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/cozipConfigVersion.cmake"
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cozipConfig.cmake"
"include(\"\${CMAKE_CURRENT_LIST_DIR}/cozipTargets.cmake\")\n")

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/cozipConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/cozipConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cozip)
endif()


# ----------------------------------------------------------------------
# Tests (OFF by default)
# ----------------------------------------------------------------------

if(COZIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
