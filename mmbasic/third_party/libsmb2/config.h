/*
 * Build shim for mmcore's native (desktop/Linux) builds of the vendored
 * libsmb2 v6.0.0.
 *
 * Upstream generates config.h with CMake/autoconf per platform. mmcore builds
 * libsmb2 directly from source with a plain Makefile, so this file supplies the
 * HAVE_* feature macros. It covers the POSIX desktop targets (macOS and Linux);
 * a future Circle port should provide its own config (see README.mmcore.md).
 *
 * Keep in sync with upstream cmake/config.h.cmake.
 */
#ifndef LIBSMB2_MMCORE_CONFIG_H
#define LIBSMB2_MMCORE_CONFIG_H

#define HAVE_ARPA_INET_H 1
#define HAVE_DLFCN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LINGER 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_ERRNO_H 1
#define HAVE_SYS_FCNTL_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_UNISTD_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

/* BSD-derived platforms (macOS) put a length byte on sockaddr. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
	defined(__NetBSD__)
#define HAVE_SOCKADDR_LEN 1
#endif

/* Every POSIX target mmcore builds has sockaddr_storage. */
#define HAVE_SOCKADDR_STORAGE 1

/* Kerberos/GSSAPI is intentionally left out: mmcore uses NTLMv2 only. */
/* #undef HAVE_LIBKRB5 */
/* #undef HAVE_GSSAPI_GSSAPI_H */

#endif /* LIBSMB2_MMCORE_CONFIG_H */
