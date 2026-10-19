/*
 * Build shim for mmcore's MinGW/Winsock build of the vendored libsmb2 v6.0.0.
 *
 * Upstream generates config.h with CMake/autoconf per platform; its own
 * Windows path (CMakeLists.txt, CMAKE_SYSTEM_NAME Windows) does not use
 * config.h at all and instead relies on <compat.h> for the Winsock shims.
 * mmcore builds libsmb2 from a plain Makefile and defines HAVE_CONFIG_H for
 * every target, so this header supplies the HAVE_* feature macros for MinGW.
 *
 * It mirrors the header set the CMake build assumes for Windows: only the
 * standard C headers MinGW-w64 actually ships are enabled; everything
 * socket/POSIX-shaped (netdb.h, arpa/inet.h, netinet/*, poll.h, sys/socket.h,
 * sys/uio.h) is left undefined and comes from <winsock2.h> via <compat.h>.
 *
 * Keep in sync with the POSIX config at mmbasic/third_party/libsmb2/config.h
 * and with upstream cmake/config.h.cmake.
 */
#ifndef LIBSMB2_MMCORE_WIN_CONFIG_H
#define LIBSMB2_MMCORE_WIN_CONFIG_H

/* MinGW-w64 has the C99 standard headers. */
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* Winsock2 provides struct linger (and SO_LINGER), which lib/socket.c wants
 * instead of its own fallback definition. */
#define HAVE_LINGER 1

/* <winsock2.h> defines struct sockaddr_storage. */
#define HAVE_SOCKADDR_STORAGE 1

/* Kerberos/GSSAPI is intentionally left out: mmcore uses NTLMv2 only. */
/* #undef HAVE_LIBKRB5 */
/* #undef HAVE_GSSAPI_GSSAPI_H */

#endif /* LIBSMB2_MMCORE_WIN_CONFIG_H */
