cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" KARU_VERSION_FULL)
string(STRIP "${KARU_VERSION_FULL}" KARU_VERSION_FULL)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" KARU_VERSION_NUMERIC
    "${KARU_VERSION_FULL}")
if(NOT KARU_VERSION_NUMERIC)
    message(FATAL_ERROR "VERSION must start with MAJOR.MINOR.PATCH")
endif()

project(karu
    VERSION     ${KARU_VERSION_NUMERIC}
    DESCRIPTION "Stateless byte-range transport for local and cloud objects"
    LANGUAGES   C CXX
)

option(KARU_BUILD_TESTS      "Build the Karu component tests" OFF)
option(KARU_BUILD_BENCHMARKS "Build the Karu development benchmarks" OFF)
option(KARU_WERROR            "Treat warnings in Karu as errors" OFF)
option(KARU_COVERAGE          "Instrument Karu for GCC coverage reporting" OFF)
set(KARU_SANITIZE "" CACHE STRING
    "Sanitizers used to compile and link Karu component tests")

set(KARU_CORE_SOURCES
    src/platform.cpp
    src/process.cpp
    src/config.cpp
    src/uri.cpp
    src/error.cpp
    src/credential_cache.cpp
    src/request_builder.cpp
    src/runtime/batch.cpp
    src/runtime/planner.cpp
    src/runtime/transport.cpp
    src/runtime/http_response.cpp
    src/runtime/engine.cpp
    src/karu.cpp
)

set(KARU_BACKEND_SOURCES
    src/backends/url.cpp
    src/backends/crypto.cpp
    src/backends/credentials.cpp
    src/backends/aws_profile.cpp
    src/backends/registry.cpp
    src/backends/s3.cpp
    src/backends/s3_credentials.cpp
    src/backends/s3_signing.cpp
    src/backends/gcs.cpp
    src/backends/gcs_credentials.cpp
    src/backends/azure.cpp
    src/backends/azure_credentials.cpp
    src/backends/http.cpp
    src/backends/hugging_face.cpp
    src/backends/source.cpp
    src/backends/source_credentials.cpp
)

add_library(karu STATIC ${KARU_CORE_SOURCES} ${KARU_BACKEND_SOURCES})
add_library(karu::karu ALIAS karu)
# Rumi consumes Karu with add_subdirectory() and keeps this private alias.
add_library(karu::internal ALIAS karu)

target_compile_features(karu PUBLIC cxx_std_23)
target_include_directories(karu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(karu
    PUBLIC  KARU_STATIC
    PRIVATE
        KARU_VERSION_STRING="${KARU_VERSION_FULL}"
        $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS;NOMINMAX;WIN32_LEAN_AND_MEAN>
        $<$<NOT:$<PLATFORM_ID:Windows>>:_FILE_OFFSET_BITS=64>
)
target_compile_options(karu PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive-;/utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow>
    $<$<AND:$<BOOL:${KARU_WERROR}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
    $<$<AND:$<BOOL:${KARU_WERROR}>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Werror>
)
set_target_properties(karu PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# 7.83.0 stopped forwarding Authorization across redirects to another origin
# (CVE-2022-27776). Older libcurl versions could leak an HF token to a CDN.
find_package(CURL 7.83.0 REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(karu
    PUBLIC CURL::libcurl OpenSSL::Crypto Threads::Threads)

if(KARU_SANITIZE AND NOT MSVC)
    target_compile_options(karu PRIVATE
        -fsanitize=${KARU_SANITIZE} -g -fno-omit-frame-pointer)
    target_link_options(karu PUBLIC -fsanitize=${KARU_SANITIZE})
endif()

if(KARU_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "KARU_COVERAGE requires GCC")
    endif()
    target_compile_options(karu PRIVATE --coverage -O0 -g)
    target_link_options(karu PUBLIC --coverage)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS karu EXPORT karuTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/karu)
install(FILES README.md CHANGELOG.md CONFIGURATION.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(EXPORT karuTargets
    FILE karuTargets.cmake
    NAMESPACE karu::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/karu)

configure_package_config_file(
    cmake/karuConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/karuConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/karu)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/karuConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/karuConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/karuConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/karu)

if(KARU_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KARU_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
