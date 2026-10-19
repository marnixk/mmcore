/*
 * Build shim for the Circle (bare-metal Raspberry Pi) port of the vendored
 * libsmb2 v6.0.0 (#1132).
 *
 * Upstream generates config.h with CMake/autoconf per platform. mmcore builds
 * libsmb2 directly from source with Circle's Rules.mk, so this file supplies
 * the HAVE_* feature macros for aarch64-none-elf. It is compiled with
 * -DHAVE_CONFIG_H and this directory is placed on the include path ahead of
 * the POSIX shim in ../config.h.
 *
 * Circle is built with STDLIB_SUPPORT=1: the newlib headers of the bare-metal
 * toolchain are visible but newlib itself is not linked (Circle supplies
 * malloc/string and console/smb2_sock.cpp supplies the rest). There are no
 * BSD socket headers at all, so the network surface comes from
 * include/circle/smb2_circle.h via lib/compat.h, and is implemented on top of
 * Circle CSocket in console/smb2_sock.cpp.
 */
#ifndef LIBSMB2_MMCORE_CIRCLE_CONFIG_H
#define LIBSMB2_MMCORE_CIRCLE_CONFIG_H

/* Newlib headers that do exist for aarch64-none-elf. */
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define STDC_HEADERS 1

/* No socket headers on Circle. compat.h + smb2_circle.h provide the BSD
 * socket types and calls the library uses. */
/* #undef HAVE_ARPA_INET_H */
/* #undef HAVE_NETDB_H */
/* #undef HAVE_NETINET_IN_H */
/* #undef HAVE_NETINET_TCP_H */
/* #undef HAVE_POLL_H */
/* #undef HAVE_SYS_POLL_H */
/* #undef HAVE_SYS_SOCKET_H */
/* #undef HAVE_SYS_UIO_H */
/* #undef HAVE_SYS_IOCTL_H */
/* #undef HAVE_SYS_FCNTL_H */
/* #undef HAVE_SYS_UNISTD_H */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_LINGER */

/* smb2_circle.h supplies its own sockaddr_storage, so do not let compat.h
 * skip it. */
/* #undef HAVE_SOCKADDR_STORAGE */

/* Kerberos/GSSAPI is intentionally left out: mmcore uses NTLMv2 only. */
/* #undef HAVE_LIBKRB5 */
/* #undef HAVE_GSSAPI_GSSAPI_H */

#endif /* LIBSMB2_MMCORE_CIRCLE_CONFIG_H */
