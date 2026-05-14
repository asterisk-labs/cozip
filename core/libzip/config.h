/*
 * core/libzip/config.h
 *
 * Hand-written, cross-platform replacement for the config.h that
 * libzip's CMake build normally generates from cmake-config.h.in.
 *
 * Vendored from libzip 1.11.4. When bumping libzip, diff this file
 * against the new cmake-config.h.in and update the macro list to
 * match.
 *
 * Layout:
 *   1. Common (every platform, no #ifdef guard).
 *   2. POSIX (Linux, macOS, MinGW; explicitly NOT MSVC).
 *   3. Unix only (Linux, macOS, BSD; NOT MinGW, NOT MSVC).
 *   4. Windows MSVC cluster — _underscore variants, Annex K.
 *   5. Windows-common — declarations true for both MSVC and MinGW.
 *   6. macOS / BSD extras — arc4random, clonefile, getprogname, fts.
 *   7. Tunables — package metadata, sizeof checks, FDOPEN toggle.
 *
 * libzip itself does not depend on the actual values of PACKAGE or
 * VERSION, so we hardcode them to identify the vendored copy.
 */

#ifndef HAD_CONFIG_H
#define HAD_CONFIG_H

#ifndef _HAD_ZIPCONF_H
#include "zipconf.h"
#endif


/* ---- 1. Common (every platform) ---- */

#define HAVE_FILENO
#define HAVE_SETMODE
#define HAVE_SNPRINTF
#define HAVE_STRDUP
#define HAVE_STRTOLL
#define HAVE_STRTOULL
#define HAVE_STDBOOL_H
#define ENABLE_FDOPEN

/* Sizes: 64-bit on every platform we target (no 32-bit Windows,
 * no 32-bit ARM). If 32-bit support is ever required, gate these
 * with #if SIZEOF_VOID_P == 8. */
#define SIZEOF_OFF_T  8
#define SIZEOF_SIZE_T 8


/* ---- 2. POSIX (Linux, macOS, MinGW; explicitly NOT MSVC) ---- */

/* MinGW is treated as POSIX here because it ships <unistd.h>,
 * <strings.h>, and the corresponding APIs (fseeko, ftello, fchmod,
 * strcasecmp). MSVC has none of these. */
#if !defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)
#define HAVE_FCHMOD       /* POSIX file permission API */
#define HAVE_FSEEKO       /* 64-bit fseek; MSVC has _fseeki64 instead */
#define HAVE_FTELLO       /* 64-bit ftell; MSVC has _ftelli64 instead */
#define HAVE_STRCASECMP   /* MSVC has _stricmp instead */
#define HAVE_STRINGS_H    /* POSIX <strings.h>, distinct from <string.h> */
#define HAVE_UNISTD_H     /* POSIX <unistd.h>, leaks into zlib's zconf.h */
#endif


/* ---- 3. Unix-only (Linux, macOS, BSD; NOT Windows of any kind) ---- */

/* localtime_r is the thread-safe POSIX variant. MinGW does NOT
 * implement it — it's the one POSIX function that's missing on
 * Windows-with-GCC. Both MSVC and MinGW use localtime_s instead,
 * declared in the Windows-common block below. */
#if !defined(_WIN32)
#define HAVE_LOCALTIME_R
#endif


/* ---- 4. Windows MSVC ---- */

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__MINGW64__)

/* MSVC ships its POSIX-ish APIs with a leading underscore. */
#define HAVE__CLOSE
#define HAVE__DUP
#define HAVE__FDOPEN
#define HAVE__FILENO
#define HAVE__FSEEKI64
#define HAVE__FSTAT64
#define HAVE__SETMODE
#define HAVE__SNPRINTF
#define HAVE__SNPRINTF_S
#define HAVE__STAT64
#define HAVE__STRDUP
#define HAVE__STRICMP
#define HAVE__STRTOI64
#define HAVE__STRTOUI64
#define HAVE__UNLINK

#endif /* _WIN32 && !MINGW */


/* ---- 5. Windows-common (both MSVC and MinGW) ---- */

#if defined(_WIN32)

/* localtime_s is the Windows thread-safe localtime. Available on
 * both MSVC and MinGW-w64. compat.h's HAVE_LOCALTIME_S branch
 * handles the swapped argument order on Windows
 * (struct tm *result, const time_t *t) returning errno_t. */
#define HAVE_LOCALTIME_S

/* _snwprintf_s exists in MSVCRT and is exported by both MSVC and
 * MinGW-w64 via <wchar.h>. Advertising it here keeps libzip's
 * compat.h from generating its variadic-macro fallback at line 104:
 *
 *   #define _snwprintf_s(buf, bufsz, len, fmt, ...) (...)
 *
 * MSVC expands that fallback into <corecrt_wstdio.h> declarations
 * and breaks with C2059 syntax errors. GCC/MinGW under -std=gnu2x
 * rejects the same macro at parse time. The real CRT function
 * works correctly on both compilers. */
#define HAVE__SNWPRINTF_S

/* C11 Annex K bounds-checking string functions. MinGW-w64 exposes
 * them in <string.h> when __STDC_WANT_LIB_EXT1__=1 is defined,
 * which compat.h does at line 39. Advertising HAVE_STRNCPY_S here
 * avoids compat.h's macro fallback at line 220:
 *
 *   #define strncpy_s(dest, destsz, src, count) \
 *       (strncpy((dest), (src), (count)), 0)
 *
 * GCC 14 (Rtools45) flags that fallback with -Wstringop-truncation
 * when callers pass count == strlen(src). The real CRT function
 * handles the nul terminator correctly without warning. Advertising
 * HAVE_MEMCPY_S for the same reason. */
#define HAVE_MEMCPY_S
#define HAVE_STRNCPY_S
#define HAVE_STRERROR_S

#endif /* _WIN32 */


/* ---- 6. Apple/BSD extras ---- */

#if defined(__APPLE__)
#define HAVE_ARC4RANDOM     /* BSD-style RNG, present on Darwin */
#define HAVE_CLONEFILE      /* APFS copy-on-write copy */
#define HAVE_GETPROGNAME    /* BSD-style argv[0] accessor */
#define HAVE_FTS_H          /* file traversal; also on glibc but
                               libzip checks it on macOS */
#endif


/* ---- 7. Identification ---- */

#define PACKAGE "cozip-vendored-libzip"
#define VERSION "1.11.4"

#endif /* HAD_CONFIG_H */