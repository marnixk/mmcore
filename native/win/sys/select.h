/*
 * <sys/select.h> shim for the Windows build: the native sources freely
 * include it, but MinGW only exposes fd_set/select through <winsock2.h>.
 */
#ifndef MMB_WIN_SYS_SELECT_H
#define MMB_WIN_SYS_SELECT_H

#include <winsock2.h>

#endif
