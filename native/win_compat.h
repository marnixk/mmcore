/*
 * Minimal POSIX-on-Windows compatibility shim for the native backend.
 *
 * The native sources target Linux/macOS and use a handful of POSIX spellings
 * that MinGW-w64 does not provide verbatim. This header is included first so
 * <winsock2.h> wins the include-order battle with <windows.h>, and supplies
 * the missing helpers. It is a no-op on POSIX systems.
 */
#ifndef MMB_WIN_COMPAT_H
#define MMB_WIN_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

/* MinGW's <sys/stat.h> already defines S_ISDIR/S_ISREG. */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

/* POSIX mkdir(path, mode) vs Windows _mkdir(path). */
#define mkdir(path, mode) _mkdir(path)

/* setenv() does not exist; _putenv_s() is close enough. */
static __inline int mmb_win_setenv(const char *name, const char *value,
				   int overwrite)
{
	(void)overwrite;
	return _putenv_s(name, value ? value : "");
}
#define setenv(name, value, overwrite) mmb_win_setenv(name, value, overwrite)

/* sched_yield() does not exist. */
#define sched_yield() SwitchToThread()

/*
 * Winsock select() only watches sockets, so piped/redirected stdin needs a
 * different readiness probe. Console input is left to the SDL keyboard path.
 */
static __inline int mmb_stdin_ready(void)
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	DWORD type;

	if (h == NULL || h == INVALID_HANDLE_VALUE)
		return 0;
	type = GetFileType(h);
	if (type == FILE_TYPE_PIPE)
	{
		DWORD avail = 0;

		if (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
			return avail > 0;
		/* A broken pipe is EOF: report ready so the reader sees it and
		 * can stop instead of spinning forever. */
		if (GetLastError() == ERROR_BROKEN_PIPE)
			return 1;
		return 0;
	}
	if (type == FILE_TYPE_DISK)
		return 1; /* redirected file: readable until EOF */
	return 0; /* console: SDL owns keyboard input */
}

/* Winsock needs a one-time WSAStartup() before any socket call. */
static __inline int mmb_win_wsa_start(void)
{
	static int started;

	if (!started)
	{
		WSADATA wsa;

		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return -1;
		started = 1;
	}
	return 0;
}

#endif /* _WIN32 */
#endif /* MMB_WIN_COMPAT_H */
