# Vendored libzip 1.11.4 — minimal static build for cozip.
#
# Replaces upstream libzip's ~600-line lib/CMakeLists.txt. We strip
# every codec other than deflate (BZIP2, LZMA, Zstd) and every
# crypto backend (OpenSSL, mbedTLS, GnuTLS, CommonCrypto, Windows
# CNG, WinZip AES). Only the bare ZIP-with-deflate writer remains.
#
# When bumping libzip version: replace .c and .h files in this
# directory, refresh zipconf.h and zip_err_str.c from the new
# build/, diff config.h against cmake-config.h.in for changed
# probes.

# ---- common sources (compile on every platform) ----

set(LIBZIP_SOURCES_COMMON
    zip_add.c
    zip_add_dir.c
    zip_add_entry.c
    zip_algorithm_deflate.c
    zip_buffer.c
    zip_close.c
    zip_delete.c
    zip_dir_add.c
    zip_dirent.c
    zip_discard.c
    zip_entry.c
    zip_err_str.c
    zip_error.c
    zip_error_clear.c
    zip_error_get.c
    zip_error_get_sys_type.c
    zip_error_strerror.c
    zip_error_to_str.c
    zip_extra_field.c
    zip_extra_field_api.c
    zip_fclose.c
    zip_fdopen.c
    zip_file_add.c
    zip_file_error_clear.c
    zip_file_error_get.c
    zip_file_get_comment.c
    zip_file_get_external_attributes.c
    zip_file_get_offset.c
    zip_file_rename.c
    zip_file_replace.c
    zip_file_set_comment.c
    zip_file_set_encryption.c
    zip_file_set_external_attributes.c
    zip_file_set_mtime.c
    zip_file_strerror.c
    zip_fopen.c
    zip_fopen_encrypted.c
    zip_fopen_index.c
    zip_fopen_index_encrypted.c
    zip_fread.c
    zip_fseek.c
    zip_ftell.c
    zip_get_archive_comment.c
    zip_get_archive_flag.c
    zip_get_encryption_implementation.c
    zip_get_file_comment.c
    zip_get_name.c
    zip_get_num_entries.c
    zip_get_num_files.c
    zip_hash.c
    zip_io_util.c
    zip_libzip_version.c
    zip_memdup.c
    zip_name_locate.c
    zip_new.c
    zip_open.c
    zip_pkware.c
    zip_progress.c
    zip_realloc.c
    zip_rename.c
    zip_replace.c
    zip_set_archive_comment.c
    zip_set_archive_flag.c
    zip_set_default_password.c
    zip_set_file_comment.c
    zip_set_file_compression.c
    zip_set_name.c
    zip_source_accept_empty.c
    zip_source_begin_write.c
    zip_source_begin_write_cloning.c
    zip_source_buffer.c
    zip_source_call.c
    zip_source_close.c
    zip_source_commit_write.c
    zip_source_compress.c
    zip_source_crc.c
    zip_source_error.c
    zip_source_file_common.c
    zip_source_file_stdio.c
    zip_source_free.c
    zip_source_function.c
    zip_source_get_dostime.c
    zip_source_get_file_attributes.c
    zip_source_is_deleted.c
    zip_source_layered.c
    zip_source_open.c
    zip_source_pass_to_lower_layer.c
    zip_source_pkware_decode.c
    zip_source_pkware_encode.c
    zip_source_read.c
    zip_source_remove.c
    zip_source_rollback_write.c
    zip_source_seek.c
    zip_source_seek_write.c
    zip_source_stat.c
    zip_source_supports.c
    zip_source_tell.c
    zip_source_tell_write.c
    zip_source_window.c
    zip_source_write.c
    zip_source_zip.c
    zip_source_zip_new.c
    zip_stat.c
    zip_stat_index.c
    zip_stat_init.c
    zip_strerror.c
    zip_string.c
    zip_unchange.c
    zip_unchange_all.c
    zip_unchange_archive.c
    zip_unchange_data.c
    zip_utf-8.c
)

# ---- platform-specific sources ----

if(WIN32)
    set(LIBZIP_SOURCES_PLATFORM
        zip_random_win32.c
        zip_source_file_win32.c
        zip_source_file_win32_named.c
        zip_source_file_win32_ansi.c
        zip_source_file_win32_utf8.c
        zip_source_file_win32_utf16.c
    )
else()
    # Linux, macOS, FreeBSD, WASM/Emscripten — all use the POSIX
    # stdio backend.
    set(LIBZIP_SOURCES_PLATFORM
        zip_random_unix.c
        zip_source_file_stdio_named.c
    )
endif()

# ---- target ----

add_library(zip STATIC
    ${LIBZIP_SOURCES_COMMON}
    ${LIBZIP_SOURCES_PLATFORM})

# Public include path: this directory carries zip.h, zipconf.h and
# config.h. Consumers (cozip itself) need zip.h.
#
# SYSTEM marks these as external/vendored headers. CMake passes
# them with -isystem instead of -I, which suppresses warnings from
# inside zip.h when consumers compile with -Wpedantic. Notably this
# silences ~245 -Wnullability-extension warnings on macOS Clang
# (libzip annotates its API with Apple's _Nonnull macros).
target_include_directories(zip SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# libzip's source files use HAVE_CONFIG_H to gate inclusion of
# config.h. Always set it — we always supply config.h here.
target_compile_definitions(zip PRIVATE HAVE_CONFIG_H)

# Link zlib for deflate. zlib is added via add_subdirectory()
# in the parent CMakeLists, exposing the ZLIB::ZLIB alias.
target_link_libraries(zip PUBLIC ZLIB::ZLIB)

# Windows extras: link advapi32 for the random number generator
# and define WIN32_LEAN_AND_MEAN to keep <windows.h> minimal.
if(WIN32)
    target_link_libraries(zip PRIVATE advapi32)
    target_compile_definitions(zip PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# Suppress upstream libzip warnings — its source has style choices
# (-Wsign-compare, deprecated POSIX funcs on Windows) that we
# don't control. We want libzip to build silently so cozip's own
# warnings stand out.
if(NOT MSVC)
    target_compile_options(zip PRIVATE
        -w
        -Wno-deprecated-declarations
        -Wno-sign-compare
        -Wno-unused-parameter)
else()
    target_compile_options(zip PRIVATE /w)
endif()

# Position-independent code so cozip (a SHARED lib) can link us in.
#
# C_VISIBILITY_PRESET=hidden hides every libzip symbol when this
# static archive is absorbed into a shared library (cozip.dylib /
# libcozip.so). Without it, every libzip function is re-exported by
# the final shared library, polluting its ABI and risking symbol
# clashes if the consumer also links a different libzip.
set_target_properties(zip PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET       hidden
    VISIBILITY_INLINES_HIDDEN ON)