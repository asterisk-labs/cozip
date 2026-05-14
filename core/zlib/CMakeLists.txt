# Vendored zlib 1.3.1 — minimal static build for cozip.
#
# Replaces upstream zlib's ~400-line CMakeLists with a tiny static
# target. We don't ship zlib examples, gzip command-line tools, or
# the shared `libz.so` — cozip links zlib statically and never
# re-exports its symbols.
#
# When bumping zlib version: drop new sources into this directory
# and update ZLIB_SOURCES below if upstream added or removed files.
# Most version bumps don't change the file list.

set(ZLIB_SOURCES
    adler32.c
    compress.c
    crc32.c
    deflate.c
    gzclose.c
    gzlib.c
    gzread.c
    gzwrite.c
    infback.c
    inffast.c
    inflate.c
    inftrees.c
    trees.c
    uncompr.c
    zutil.c
)

add_library(zlibstatic STATIC ${ZLIB_SOURCES})

# Match the alias name libzip's find_package(ZLIB) and our root
# CMakeLists are wired to consume.
add_library(ZLIB::ZLIB ALIAS zlibstatic)

# Headers in this directory must be visible to consumers (libzip,
# cozip). SYSTEM marks them as external/vendored so consumers
# compiling with -Wpedantic don't get warnings from inside zlib.h.
target_include_directories(zlibstatic SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# zlib expects HAVE_UNISTD_H to be set on POSIX. zconf.h handles
# this correctly already (it #defines Z_HAVE_UNISTD_H under the
# right #ifdef) so we don't need anything else.

# Suppress upstream zlib warnings — its source has known issues
# (-Wimplicit-function-declaration on some old funcs, etc) that
# we don't control. Apply quietly so cozip's own warnings stand
# out in build logs.
if(NOT MSVC)
    target_compile_options(zlibstatic PRIVATE
        -w
        -Wno-deprecated-declarations
        -Wno-implicit-function-declaration)
else()
    target_compile_options(zlibstatic PRIVATE /w)
endif()

# Position-independent code so cozip (which is a SHARED lib) can
# link this static archive into itself. Without this, ld throws
# "relocation R_X86_64_PC32 against symbol" on Linux x86_64.
#
# C_VISIBILITY_PRESET=hidden hides every zlib symbol when this static
# archive is absorbed into a shared library (cozip.dylib /
# libcozip.so). Without it, every zlib function is re-exported by the
# final shared library, polluting its ABI and risking symbol clashes
# if the consumer also links a different zlib through another path.
set_target_properties(zlibstatic PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET       hidden
    VISIBILITY_INLINES_HIDDEN ON)