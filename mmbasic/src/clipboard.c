/*
 * Host OS clipboard bridge (#525).
 *
 * Thin, platform-agnostic accessors over mmb_platform.clipboard_get/set. The
 * native desktop backend (SDL) supplies the host clipboard; the bare-metal Pi
 * leaves both callbacks NULL, so every call here is a no-op there and a copy
 * simply stays in MMBasic's own buffer.
 */
#include "mmb_priv.h"

int mmb_clipboard_available(void)
{
	return G.plat && G.plat->clipboard_set ? 1 : 0;
}

char *mmb_clipboard_get(void)
{
	if (!G.plat || !G.plat->clipboard_get)
		return 0;
	return G.plat->clipboard_get();
}

int mmb_clipboard_setn(const char *s, unsigned n)
{
	char *buf;

	if (!G.plat || !G.plat->clipboard_set)
		return -1;
	if (!s)
	{
		s = "";
		n = 0;
	}
	buf = G.plat->alloc(n + 1);
	if (!buf)
		return -1;
	if (n)
		memcpy(buf, s, n);
	buf[n] = 0;
	G.plat->clipboard_set(buf);
	G.plat->free(buf);
	return 0;
}

int mmb_clipboard_set(const char *utf8)
{
	return mmb_clipboard_setn(utf8, utf8 ? (unsigned)strlen(utf8) : 0);
}
