#include "mmb_priv.h"
#include "frontend.h"
#include "tui.h"
#include "pkg_info.h"
#include "app_category.h"
#include "app_list.h"

#include <stdio.h>

/*
 * App PATH and home launcher (#515, #520, #1524).
 *
 * - OPTION PATH lists directories to search for ``.APP`` packages. A bare
 *   token typed at the REPL prompt resolves to the first ``NAME.APP`` found
 *   and runs it with the same semantics as ``RUN "NAME.APP"``. Builtins and
 *   user SUBs/functions always win (the caller only falls through here).
 * - Ctrl+Space opens a quick picker of every ``.APP`` on the PATH.
 * - OPTION BOOT REPL|LAUNCHER|"name" decides what happens at power-on. The
 *   launcher is the same widget: Enter runs, Esc/back reaches the REPL.
 *
 * The APPS launcher groups packages by their PACKAGE.INF category (#1524);
 * the Ctrl+Space picker stays a flat label-sorted list with a category
 * filter. Row building and navigation live in app_list.c so they are
 * host-testable without the TUI.
 */

#define APP_MAX_ENT  160
#define APP_LABEL    48
#define APP_NAME     80
#define APP_PATH     160
#define APP_LISTING  4096
#define APP_ESC_IDLE_MS 60

#define APPTUI_PICK     0
#define APPTUI_LAUNCH   1

#ifdef MMB_HAVE_HUB
#define AT_HAS_HUB 1
#else
#define AT_HAS_HUB 0
#endif

/* Built-in full-screen apps offered beside user .APP packages, in the order
 * shown in the launcher (labels are the human-readable names, #624). #793
 * dropped Package/Connect from the offer and put Terminal after Jukebox; the
 * PACKAGE and CONNECT commands still work when typed directly. They form
 * their own trailing BUILT-IN group (#1524) so the fixed category order and
 * case-insensitive package sort leave their deliberate order untouched. */
typedef struct builtin_app {
	const char *cmd;
	const char *label;
} builtin_app;

static const builtin_app at_builtins[] = {
	{ "EDIT", "Editor" },
	{ "FILES", "File manager" },
	{ "WORDPAD", "Word processor" },
	{ "PAINT", "Paint" },
	{ "JUKE", "Jukebox" },
	{ "TERM", "Terminal" },
	{ "HELP", "Help" },
	{ "SETTINGS", "Settings" },
};
#define AT_BUILTIN_COUNT ((int)(sizeof(at_builtins) / sizeof(at_builtins[0])))

#define AT_KIND_PKG 0
#define AT_KIND_CMD 1

typedef struct app_entry {
	int kind;              /* AT_KIND_PKG or AT_KIND_CMD */
	char label[APP_LABEL]; /* human-readable name */
	char path[APP_PATH];   /* resolved .APP path (kind PKG) */
	char cmd[APP_LABEL];   /* builtin command to run (kind CMD) */
	int cat;               /* category index, or APP_LIST_CAT_BUILTIN */
	char *summary;         /* dynamic INF summary (may be NULL) */
} app_entry;

static const mmb_ed_theme *atth(void)
{
	return mmb_editor_theme();
}

#define AT_FG       ((int)atth()->dlg_fg)
#define AT_BG       ((int)atth()->dlg_bg)
#define AT_TITLE_FG ((int)atth()->menu_fg)
#define AT_TITLE_BG ((int)atth()->menu_bg)
#define AT_SEL_FG   ((int)atth()->sel_fg)
#define AT_SEL_BG   ((int)atth()->sel_bg)
#define AT_HOT      ((int)atth()->hot)
#define AT_CMT      ((int)atth()->cmt_fg)
#define AT_BRD_FG   ((int)atth()->brd_fg)
#define AT_BRD_BG   ((int)atth()->brd_bg)
#define AT_ERR_FG   ((int)atth()->err_fg)
#define AT_ERR_BG   ((int)atth()->err_bg)

typedef struct app_tui_state {
	int active;
	int mode;   /* APPTUI_PICK or APPTUI_LAUNCH */
	int nent;
	app_entry ent[APP_MAX_ENT];
	struct app_list list;
	int sel;                /* selected row in list */
	int top;                /* first visible row */
	int page;               /* rows per page from the last layout */
	int catf;               /* picker category filter, -1 = ALL */
	int present[MMB_APP_CATEGORY_COUNT]; /* non-empty package categories */
	int builtin_present;    /* picker BUILT-IN group has entries */
	char filter[APP_LABEL]; /* typed substring, empty = no filter */
	int esc;
	unsigned esc_at;
	char csi[8];            /* ESC [ ... parameter bytes */
	int csi_len;
	int confirm;            /* Del removal prompt is showing */
	char confirm_label[APP_LABEL];
	char confirm_path[APP_PATH];
	char status[96];
} app_tui_state;

static app_tui_state AT_s[MMB_MAX_CONSOLES];
#define AT (AT_s[g_console])

static void at_draw(void);

static void at_append(char *dst, int sz, const char *s)
{
	int n = (int)strlen(dst);
	if (n >= sz - 1 || !s)
		return;
	strncat(dst, s, (unsigned)(sz - n - 1));
}

static void at_trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		memmove(s, s + 1, strlen(s));
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = 0;
}

static int at_join(char *out, int outsz, const char *dir, const char *name)
{
	int n;
	out[0] = 0;
	if (!dir || !dir[0])
		return -1;
	strncpy(out, dir, (unsigned)outsz - 1);
	out[outsz - 1] = 0;
	n = (int)strlen(out);
	if (n && out[n - 1] != '/' && out[n - 1] != '\\' && n + 1 < outsz)
	{
		out[n++] = '/';
		out[n] = 0;
	}
	strncat(out, name, (unsigned)outsz - strlen(out) - 1);
	return 0;
}

/* True when name already ends in .APP (case-insensitive). */
static int at_has_ext(const char *name)
{
	int n = (int)strlen(name);
	return n >= 4 && mmb_keyword_eq(name + n - 4, ".APP");
}

static int at_is_explicit(const char *name)
{
	return strchr(name, '/') || strchr(name, '\\') || strchr(name, ':');
}

int mmb_app_resolve(const char *name, char *out, int outsz)
{
	char want[APP_PATH];
	const char *p;
	int n;

	if (!name || !name[0] || outsz <= 4)
		return 0;
	n = 0;
	while (name[n] && n < (int)sizeof(want) - 6)
	{
		want[n] = name[n];
		n++;
	}
	want[n] = 0;
	if (!at_has_ext(want))
		strncat(want, ".APP", sizeof(want) - strlen(want) - 1);

	if (at_is_explicit(want))
	{
		if (mmb_vfs_exists(want) && !mmb_vfs_isdir(want))
		{
			strncpy(out, want, (unsigned)outsz - 1);
			out[outsz - 1] = 0;
			return 1;
		}
		return 0;
	}

	p = mmb_app_path();
	while (p && *p)
	{
		const char *e = p;
		char dir[APP_PATH], full[APP_PATH + APP_NAME];
		int dn;

		while (*e && *e != ';')
			e++;
		dn = (int)(e - p);
		if (dn > (int)sizeof(dir) - 1)
			dn = (int)sizeof(dir) - 1;
		memcpy(dir, p, (unsigned)dn);
		dir[dn] = 0;
		at_trim(dir);
		if (dir[0] && at_join(full, sizeof(full), dir, want) == 0 &&
		    mmb_vfs_exists(full) && !mmb_vfs_isdir(full))
		{
			strncpy(out, full, (unsigned)outsz - 1);
			out[outsz - 1] = 0;
			return 1;
		}
		p = *e ? e + 1 : e;
	}
	return 0;
}

/* Directory names compare case-insensitively with an optional trailing
 * slash, so "c:/apps" matches "C:/APPS/". */
static int at_dir_eq(const char *a, const char *b)
{
	int na = (int)strlen(a), nb = (int)strlen(b);

	while (na && (a[na - 1] == '/' || a[na - 1] == '\\'))
		na--;
	while (nb && (b[nb - 1] == '/' || b[nb - 1] == '\\'))
		nb--;
	if (na != nb)
		return 0;
	while (na--)
	{
		char x = a[na], y = b[na];

		if (x >= 'a' && x <= 'z')
			x = (char)(x - 32);
		if (y >= 'a' && y <= 'z')
			y = (char)(y - 32);
		if (x == '\\')
			x = '/';
		if (y == '\\')
			y = '/';
		if (x != y)
			return 0;
	}
	return 1;
}

/* Is `dir` one of the semicolon-separated entries of `path`? */
int mmb_app_path_has(const char *path, const char *dir)
{
	while (path && *path)
	{
		const char *e = path;
		char one[APP_PATH];
		int n;

		while (*e && *e != ';')
			e++;
		n = (int)(e - path);
		if (n > (int)sizeof(one) - 1)
			n = (int)sizeof(one) - 1;
		memcpy(one, path, (unsigned)n);
		one[n] = 0;
		at_trim(one);
		if (one[0] && at_dir_eq(one, dir))
			return 1;
		path = *e ? e + 1 : e;
	}
	return 0;
}

/* The PATH every app lookup uses. An explicit OPTION PATH is used as is; the
 * default (A:/APPS/, the ramdisk) gains C:/APPS/ in front of it when that
 * folder exists, so apps installed to the SD card are found first and a
 * newer copy there shadows a bundled one of the same name. */
const char *mmb_app_path(void)
{
	static char eff[sizeof(G.opt.app_path) + sizeof(MMB_APP_PATH_SD) + 2];
	const char *p = G.opt.app_path;

	if (!at_dir_eq(p, MMB_APP_PATH_DEFAULT))
		return p;
	if (!mmb_vfs_isdir(MMB_APP_PATH_SD))
		return p;
	snprintf(eff, sizeof(eff), "%s;%s", MMB_APP_PATH_SD, p);
	return eff;
}

/* A package file name already listed from an earlier PATH folder: the later
 * copy is shadowed (a bare name runs the first one), so it is not listed. */
/* Circle's util.h has strchr but not strrchr; find the last occurrence. */
static const char *at_last(const char *s, int c)
{
	const char *last = 0;

	for (; s && *s; s++)
	{
		if ((unsigned char)*s == (unsigned char)c)
			last = s;
	}
	return last;
}

static int at_seen_file(const char *fname)
{
	int i;

	for (i = 0; i < AT.nent; i++)
	{
		const char *path = AT.ent[i].path, *base;

		if (AT.ent[i].kind != AT_KIND_PKG)
			continue;
		base = at_last(path, '/');
		base = base ? base + 1 : path;
		if (mmb_keyword_eq(base, fname))
			return 1;
	}
	return 0;
}

static int at_seen_label(const char *label)
{
	int i;
	for (i = 0; i < AT.nent; i++)
		if (mmb_keyword_eq(AT.ent[i].label, label))
			return 1;
	return 0;
}

static char *at_strdup(const char *s)
{
	unsigned n;
	char *p;

	if (!s || !s[0])
		return 0;
	n = (unsigned)strlen(s) + 1;
	p = G.plat->alloc(n);
	if (p)
		memcpy(p, s, n);
	return p;
}

static int at_is_builtin_cmd(const char *name)
{
	int i;
	for (i = 0; i < AT_BUILTIN_COUNT; i++)
		if (mmb_keyword_eq(at_builtins[i].cmd, name))
			return 1;
	return 0;
}

static void at_free_entries(void)
{
	int i;

	for (i = 0; i < AT.nent; i++)
		if (AT.ent[i].summary)
		{
			G.plat->free(AT.ent[i].summary);
			AT.ent[i].summary = 0;
		}
	AT.nent = 0;
}

/* One PACKAGE.INF read per package (#1524): title, category and summary come
 * from the shared reader. Unreadable archives leave everything empty, so the
 * entry falls back to the file stem under OTHER, as before. */
static void at_add(const char *dir, const char *fname)
{
	char stem[APP_LABEL], full[APP_PATH + APP_NAME], title[APP_LABEL];
	char label[APP_LABEL];
	struct mmb_pkg_info pi;
	int n, cat;

	if (AT.nent >= APP_MAX_ENT)
		return;
	n = (int)strlen(fname);
	if (n < 5 || !mmb_keyword_eq(fname + n - 4, ".APP"))
		return;
	strncpy(stem, fname, sizeof(stem) - 1);
	stem[sizeof(stem) - 1] = 0;
	if (n - 4 < (int)sizeof(stem))
		stem[n - 4] = 0;
	if (!stem[0] || at_is_builtin_cmd(stem) || at_seen_file(fname))
		return;
	if (at_join(full, sizeof(full), dir, fname) != 0)
		return;
	/* Prefer the package's stored Title (#1305). Duplicate labels fall back
	 * to the stem, then to the full file name, so two distinct packages are
	 * never silently collapsed into one entry. */
	mmb_pkg_info_read(full, &pi);
	title[0] = 0;
	if (pi.title && pi.title[0])
	{
		strncpy(title, pi.title, sizeof(title) - 1);
		title[sizeof(title) - 1] = 0;
	}
	cat = pi.cat_index;
	if (cat < 0 || cat >= mmb_app_category_count())
		cat = MMB_APP_CATEGORY_OTHER;
	label[0] = 0;
	if (title[0] && !at_seen_label(title))
	{
		strncpy(label, title, sizeof(label) - 1);
		label[sizeof(label) - 1] = 0;
	}
	else if (!at_seen_label(stem))
	{
		strncpy(label, stem, sizeof(label) - 1);
		label[sizeof(label) - 1] = 0;
	}
	else if (!at_seen_label(fname))
	{
		strncpy(label, fname, sizeof(label) - 1);
		label[sizeof(label) - 1] = 0;
	}
	else
	{
		mmb_pkg_info_free(&pi);
		return;
	}
	AT.ent[AT.nent].kind = AT_KIND_PKG;
	strncpy(AT.ent[AT.nent].label, label, APP_LABEL - 1);
	AT.ent[AT.nent].label[APP_LABEL - 1] = 0;
	strncpy(AT.ent[AT.nent].path, full, APP_PATH - 1);
	AT.ent[AT.nent].path[APP_PATH - 1] = 0;
	AT.ent[AT.nent].cat = cat;
	AT.ent[AT.nent].summary = at_strdup(pi.summary);
	AT.nent++;
	mmb_pkg_info_free(&pi);
}

static void at_compute_present(void)
{
	int i;

	memset(AT.present, 0, sizeof(AT.present));
	AT.builtin_present = 0;
	for (i = 0; i < AT.nent; i++)
	{
		if (AT.ent[i].cat == APP_LIST_CAT_BUILTIN)
			AT.builtin_present = 1;
		else if (AT.ent[i].cat >= 0 &&
			 AT.ent[i].cat < mmb_app_category_count())
			AT.present[AT.ent[i].cat] = 1;
	}
}

/* Rebuild the visible row list from the entries (and the HUB pinned entry
 * when that feature is compiled in). Selection is clamped onto a real app
 * row, so header rows are never selected. */
static void at_rebuild(void)
{
	struct app_list_src *src = 0;
	int i, flags = 0, catf = -1;

	if (AT.nent > 0)
	{
		src = G.plat->alloc((unsigned)AT.nent * (unsigned)sizeof(*src));
		if (!src)
		{
			app_list_free(&AT.list);
			AT.sel = -1;
			return;
		}
		for (i = 0; i < AT.nent; i++)
		{
			src[i].label = AT.ent[i].label;
			src[i].summary = AT.ent[i].summary;
			src[i].category = AT.ent[i].cat;
			src[i].payload = i;
		}
	}
	if (AT.mode == APPTUI_LAUNCH)
	{
		flags = APP_LIST_GROUPED;
		if (AT_HAS_HUB)
			flags |= APP_LIST_HUB;
	}
	else
		catf = AT.catf;
	app_list_build(&AT.list, src, AT.nent, AT.filter, catf, flags);
	if (src)
		G.plat->free(src);
	AT.sel = app_list_clamp(&AT.list, AT.sel);
}

static void at_scan(void)
{
	char listing[APP_LISTING];
	const char *p = mmb_app_path();
	int i;

	at_free_entries();
	AT.top = 0;
	/* User packages first (so a boot launcher defaults to them), then the
	 * built-in apps under their human labels (#624). */
	while (p && *p)
	{
		const char *e = p;
		char dir[APP_PATH];
		int dn;
		char *line, *next;

		while (*e && *e != ';')
			e++;
		dn = (int)(e - p);
		if (dn > (int)sizeof(dir) - 1)
			dn = (int)sizeof(dir) - 1;
		memcpy(dir, p, (unsigned)dn);
		dir[dn] = 0;
		at_trim(dir);
		p = *e ? e + 1 : e;
		if (!dir[0])
			continue;
		{
			int truncated = 0;

			if (mmb_vfs_list(dir, listing, sizeof(listing), &truncated) != 0)
				continue;
			if (truncated)
			{
				strncpy(AT.status, "... more: app folder was cut",
					sizeof(AT.status) - 1);
				AT.status[sizeof(AT.status) - 1] = 0;
			}
		}
		for (line = listing; line && *line && AT.nent < APP_MAX_ENT; line = next)
		{
			int len;
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			at_trim(line);
			len = (int)strlen(line);
			if (len < 5 || line[len - 1] == '/')
				continue;
			if (mmb_vfs_hidden_name(line))
				continue;
			at_add(dir, line);
		}
	}
	for (i = 0; i < AT_BUILTIN_COUNT && AT.nent < APP_MAX_ENT; i++)
	{
		AT.ent[AT.nent].kind = AT_KIND_CMD;
		strncpy(AT.ent[AT.nent].label, at_builtins[i].label,
			APP_LABEL - 1);
		AT.ent[AT.nent].label[APP_LABEL - 1] = 0;
		strncpy(AT.ent[AT.nent].cmd, at_builtins[i].cmd, APP_LABEL - 1);
		AT.ent[AT.nent].cmd[APP_LABEL - 1] = 0;
		AT.ent[AT.nent].cat = APP_LIST_CAT_BUILTIN;
		AT.ent[AT.nent].summary = 0;
		AT.nent++;
	}
	at_compute_present();
	at_rebuild();
}

static void at_close(void)
{
	AT.active = 0;
	AT.esc = 0;
	AT.confirm = 0;
	at_free_entries();
	app_list_free(&AT.list);
	tui_end();
	/* Overlay (#624): paint the REPL screen back instead of a blank one. */
	if (!tui_overlay_end())
		mmb_console_write("\r\n");
}

static void at_run_selected(void)
{
	char cmd[APP_PATH + 16];
	const struct app_list_row *r;
	int idx;

	if (!app_list_is_app(&AT.list, AT.sel))
		return;
	r = &AT.list.rows[AT.sel];
	if (r->payload == APP_LIST_PAYLOAD_HUB)
	{
		at_close();
		mmb_exec_line("HUB");
		return;
	}
	idx = r->payload;
	if (idx < 0 || idx >= AT.nent)
		return;
	if (AT.ent[idx].kind == AT_KIND_CMD)
	{
		strncpy(cmd, AT.ent[idx].cmd, sizeof(cmd) - 1);
		cmd[sizeof(cmd) - 1] = 0;
		at_close();
		if (cmd[0])
			mmb_exec_line(cmd);
		return;
	}
	strcpy(cmd, "RUN \"");
	strncat(cmd, AT.ent[idx].path, sizeof(cmd) - 8);
	strcat(cmd, "\"");
	at_close();
	mmb_exec_line(cmd);
}

static void at_move(int dir)
{
	int n = app_list_next(&AT.list, AT.sel, dir);

	if (n >= 0)
		AT.sel = n;
}

static void at_jump_category(int dir)
{
	int n = app_list_jump_category(&AT.list, AT.sel, dir);

	if (n >= 0)
		AT.sel = n;
}

static void at_page(int dir, int listh)
{
	AT.sel = app_list_skip(&AT.list, AT.sel, dir, listh);
}

static void at_home(void)
{
	int n = app_list_first(&AT.list);

	if (n >= 0)
		AT.sel = n;
}

static void at_end(void)
{
	int n = app_list_last(&AT.list);

	if (n >= 0)
		AT.sel = n;
}

/* Tab / Shift+Tab in the picker: ALL -> BUILT-IN -> first non-empty category
 * -> ... -> OTHER -> ALL, skipping groups with no entries (#1546). */
static void at_cycle_cat(int dir)
{
	int ring[MMB_APP_CATEGORY_COUNT + 2];
	int i, cur = 0, nring = 0, p;

	ring[nring++] = -1;
	if (AT.builtin_present)
		ring[nring++] = APP_LIST_CAT_BUILTIN;
	for (i = 0; i < mmb_app_category_count(); i++)
		if (AT.present[i])
			ring[nring++] = i;
	for (i = 0; i < nring; i++)
		if (ring[i] == AT.catf)
			cur = i;
	p = cur + dir;
	p %= nring;
	if (p < 0)
		p += nring;
	AT.catf = ring[p];
	at_rebuild();
	AT.sel = app_list_first(&AT.list);
}

static void at_escape(void)
{
	if (AT.confirm)
	{
		AT.confirm = 0;
		AT.status[0] = 0;
		if (AT.active)
			at_draw();
		return;
	}
	/* Esc clears an active filter first (#1320); only a second Esc (or one
	 * with nothing to clear) leaves the launcher. */
	if (AT.filter[0])
	{
		AT.filter[0] = 0;
		at_rebuild();
		if (AT.active)
			at_draw();
		return;
	}
	at_close();
}

/* Del in the APPS launcher: prompt on the status line, then remove the .APP
 * file on Y. Built-ins and the HUB entry are not files on the PATH. */
static void at_confirm_remove(void)
{
	const struct app_list_row *r;
	int idx;

	if (AT.mode != APPTUI_LAUNCH)
		return;
	if (!app_list_is_app(&AT.list, AT.sel))
		return;
	r = &AT.list.rows[AT.sel];
	if (r->payload < 0 || r->payload >= AT.nent)
		return;
	idx = r->payload;
	if (AT.ent[idx].kind != AT_KIND_PKG)
		return;
	AT.confirm = 1;
	strncpy(AT.confirm_label, AT.ent[idx].label, APP_LABEL - 1);
	AT.confirm_label[APP_LABEL - 1] = 0;
	strncpy(AT.confirm_path, AT.ent[idx].path, APP_PATH - 1);
	AT.confirm_path[APP_PATH - 1] = 0;
	snprintf(AT.status, sizeof(AT.status), "Remove %s? [y/N]",
		 AT.confirm_label);
}

static void at_remove_confirmed(void)
{
	char label[APP_LABEL];
	int old = AT.sel;

	strncpy(label, AT.confirm_label, sizeof(label) - 1);
	label[sizeof(label) - 1] = 0;
	AT.confirm = 0;
	if (mmb_vfs_readonly_path(AT.confirm_path))
	{
		snprintf(AT.status, sizeof(AT.status),
			 "Cannot remove %s: read-only drive", label);
		return;
	}
	if (mmb_vfs_kill(AT.confirm_path) != 0)
	{
		snprintf(AT.status, sizeof(AT.status), "Cannot remove %s", label);
		return;
	}
	snprintf(AT.status, sizeof(AT.status), "Removed %s", label);
	AT.sel = old;
	at_scan();
}

static void at_confirm_key(char c)
{
	if (c == 'y' || c == 'Y')
	{
		at_remove_confirmed();
		if (AT.active)
			at_draw();
		return;
	}
	if (c == 'n' || c == 'N' || c == 27)
	{
		AT.confirm = 0;
		AT.status[0] = 0;
		if (AT.active)
			at_draw();
	}
}

static int at_csi_int(const char *s)
{
	int v = 0;

	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

static void at_csi_final(char c)
{
	int v;

	switch (c)
	{
	case 'A':
		at_move(-1);
		break;
	case 'B':
		at_move(1);
		break;
	case 'C':
		at_jump_category(1);
		break;
	case 'D':
		at_jump_category(-1);
		break;
	case 'H':
		at_home();
		break;
	case 'F':
		at_end();
		break;
	case 'Z':
		if (AT.mode == APPTUI_PICK)
			at_cycle_cat(-1);
		break;
	case '~':
		v = at_csi_int(AT.csi);
		if (v == 1 || v == 7)
			at_home();
		else if (v == 4 || v == 8)
			at_end();
		else if (v == 5)
			at_page(-1, AT.page > 0 ? AT.page : 1);
		else if (v == 6)
			at_page(1, AT.page > 0 ? AT.page : 1);
		else if (v == 3)
			at_confirm_remove();
		break;
	default:
		break;
	}
}

/* Draw s clipped to `width` cells, ending in "..." when it does not fit. */
static void at_put_trunc(int x, int y, const char *s, int width, int fg, int bg)
{
	int n, i;

	if (!s || width <= 0)
		return;
	n = (int)strlen(s);
	if (n <= width)
	{
		tui_puts(x, y, s, fg, bg);
		return;
	}
	if (width <= 3)
	{
		for (i = 0; i < width; i++)
			tui_put(x + i, y, (unsigned char)s[i], fg, bg);
		return;
	}
	for (i = 0; i < width - 3; i++)
		tui_put(x + i, y, (unsigned char)s[i], fg, bg);
	tui_puts(x + width - 3, y, "...", fg, bg);
}

static void at_draw_header(int x, int w, int ry,
			   const struct app_list_row *r)
{
	char hdr[64];
	int n;

	tui_fill(x + 1, ry, w - 2, 1, ' ', AT_FG, AT_BG);
	if (!r->label)
		return;
	if (r->category == APP_LIST_CAT_HUB)
		snprintf(hdr, sizeof(hdr), "%s", r->label);
	else
		snprintf(hdr, sizeof(hdr), "%s (%d)", r->label, r->count);
	tui_puts(x + 2, ry, hdr, AT_HOT, AT_BG);
	n = (int)strlen(hdr);
	if (x + 3 + n < x + w - 1)
		tui_fill(x + 3 + n, ry, (x + w - 2) - (x + 3 + n), 1, TUI_H,
			 AT_HOT, AT_BG);
}

static void at_draw_app(int x, int w, int ry, const struct app_list_row *r,
			int sel)
{
	int fg = sel ? AT_SEL_FG : AT_FG;
	int bg = sel ? AT_SEL_BG : AT_BG;
	int dim = sel ? AT_SEL_FG : AT_CMT;
	int cx = x + 4;
	int avail = (x + w - 2) - cx;

	if (avail < 1)
		return;
	tui_fill(x + 1, ry, w - 2, 1, ' ', fg, bg);
	tui_puts(x + 2, ry, sel ? "> " : "  ", fg, bg);
	if (AT.mode == APPTUI_PICK)
	{
		const char *cat = r->category == APP_LIST_CAT_BUILTIN
					  ? "BUILT-IN"
					  : mmb_app_category_name(r->category);
		int catw = cat ? (int)strlen(cat) : 0;
		int labw = avail - catw - 2;

		if (!cat)
			cat = "OTHER";
		if (labw < 1)
		{
			catw = avail;
			labw = 0;
		}
		if (labw > 0)
			at_put_trunc(cx, ry, r->label, labw, fg, bg);
		if (catw > 0)
			tui_puts(cx + avail - catw, ry, cat, dim, bg);
		return;
	}
	if (r->summary && r->summary[0] && avail > 8)
	{
		int labw = avail * 3 / 5;
		int sumw = avail - labw - 2;
		int sx;

		if (sumw < 4)
		{
			labw = avail;
			sumw = 0;
		}
		at_put_trunc(cx, ry, r->label, labw, fg, bg);
		if (sumw > 0)
		{
			sx = cx + avail - sumw;
			at_put_trunc(sx, ry, r->summary, sumw, dim, bg);
		}
		return;
	}
	at_put_trunc(cx, ry, r->label, avail, fg, bg);
}

static void at_draw(void)
{
	int x, y, w, h;
	int i, listy, listh, cols, rows;
	char title[64];

	if (!AT.active)
		return;
	tui_begin();
	mmb_editor_apply_tui_palette();
	/* Same backdrop + panel chrome as SETTINGS (#793): fill the overlay with
	 * the menu surface, then draw a brd-framed dialog body. The panel grows
	 * with the screen (#1524) so category headers, labels and summaries fit
	 * at every MODE, narrow (4:3 / 16:10 / 1366x768) and wide (1920x1080). */
	tui_clear(AT_TITLE_FG, AT_TITLE_BG);
	/* Row-0 backdrop header (#1217), same as SETTINGS: date/time + battery. */
	mmb_tui_status_header();
	cols = tui_cols();
	rows = tui_rows();
	tui_dialog_geom(cols - 4, rows - 2, &x, &y, &w, &h);

	if (AT.mode == APPTUI_LAUNCH)
		strcpy(title, " HOME ");
	else if (AT.catf == APP_LIST_CAT_BUILTIN)
		strcpy(title, " APPS: BUILT-IN ");
	else if (AT.catf >= 0)
		snprintf(title, sizeof(title), " APPS: %s ",
			 mmb_app_category_name(AT.catf));
	else
		strcpy(title, " APPS: ALL ");
	tui_dialog_panel(x, y, w, h, title, AT_FG, AT_BG,
			 AT_BRD_FG, AT_BRD_BG, AT_TITLE_FG, AT_TITLE_BG);

	if (AT.filter[0])
	{
		char intro[APP_LABEL + 16];

		intro[0] = 0;
		at_append(intro, sizeof(intro), "Filter: ");
		at_append(intro, sizeof(intro), AT.filter);
		tui_puts(x + 2, y + 2, intro, AT_HOT, AT_BG);
	}
	else if (AT.mode == APPTUI_LAUNCH)
		tui_puts(x + 2, y + 2, "Choose an app. Esc returns to the REPL.",
			 AT_HOT, AT_BG);
	else
		tui_puts(x + 2, y + 2, "Choose an app to run.", AT_HOT, AT_BG);

	listy = y + 3;
	listh = (y + h - 2) - listy;
	if (listh < 1)
		listh = 1;
	AT.page = listh;
	if (AT.sel < AT.top)
		AT.top = AT.sel;
	if (AT.sel >= AT.top + listh)
		AT.top = AT.sel - listh + 1;
	if (AT.top < 0)
		AT.top = 0;
	if (AT.top > 0 && AT.list.nrow > 0 && AT.top >= AT.list.nrow)
		AT.top = AT.list.nrow - 1;

	for (i = 0; i < listh; i++)
	{
		int ridx = AT.top + i;
		int ry = listy + i;
		const struct app_list_row *r;

		if (ridx < 0 || ridx >= AT.list.nrow)
		{
			tui_fill(x + 1, ry, w - 2, 1, ' ', AT_FG, AT_BG);
			if (AT.list.nrow == 0 && i == 0)
				tui_pad(x + 2, ry, "  No matches", w - 4,
					AT_ERR_FG, AT_ERR_BG);
			continue;
		}
		r = &AT.list.rows[ridx];
		if (r->kind == APP_LIST_HEADER)
			at_draw_header(x, w, ry, r);
		else
			at_draw_app(x, w, ry, r, ridx == AT.sel);
	}

	if (AT.status[0])
		tui_puts(x + 2, y + h - 3, AT.status, AT_ERR_FG, AT_ERR_BG);
	if (AT.mode == APPTUI_LAUNCH)
		tui_status_hint_at(x + 1, y + h - 2, w - 2,
			"<Up/Down> Move  <Left/Right> Category  <Enter> Run  <Del> Remove  <Esc> REPL",
			AT_HOT, AT_FG, AT_BG);
	else
		tui_status_hint_at(x + 1, y + h - 2, w - 2,
			"<Up/Down> Move  <Tab> Category  <Enter> Run  <Esc> Cancel",
			AT_HOT, AT_FG, AT_BG);
	tui_cursor(0, 0, 0);
	tui_flush();
}

void mmb_apptui_open(int launcher)
{
	if (AT.active)
		return;
	tui_overlay_begin();
	at_free_entries();
	app_list_free(&AT.list);
	memset(&AT, 0, sizeof(AT));
	app_list_init(&AT.list);
	AT.active = 1;
	AT.mode = launcher ? APPTUI_LAUNCH : APPTUI_PICK;
	AT.catf = -1;
	AT.sel = 0;
	at_scan();
	tui_begin();
	mmb_editor_apply_tui_palette();
	tui_invalidate();
	at_draw();
}

int mmb_apptui_active(void)
{
	return AT.active;
}

int mmb_apptui_launcher(void)
{
	return AT.active && AT.mode == APPTUI_LAUNCH;
}

const char *mmb_apptui_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!AT.active)
		return G.out;
	if (AT.confirm)
	{
		at_confirm_key(c);
		return G.out;
	}
	if (AT.esc == 1)
	{
		if (c == '[' || c == 'O')
		{
			AT.esc = 2;
			AT.csi_len = 0;
			AT.csi[0] = 0;
			return G.out;
		}
		AT.esc = 0;
		at_escape();
		return G.out;
	}
	if (AT.esc == 2)
	{
		if (c >= 0x20 && c <= 0x3f)
		{
			if (AT.csi_len < (int)sizeof(AT.csi) - 1)
			{
				AT.csi[AT.csi_len++] = c;
				AT.csi[AT.csi_len] = 0;
			}
			return G.out;
		}
		AT.esc = 0;
		at_csi_final(c);
		if (AT.active)
			at_draw();
		return G.out;
	}
	if (c == 27)
	{
		AT.esc = 1;
		AT.esc_at = mmb_now_ms();
		return G.out;
	}
	if (c == 9)
	{
		if (AT.mode == APPTUI_PICK)
		{
			at_cycle_cat(1);
			if (AT.active)
				at_draw();
		}
		return G.out;
	}
	if (c == 8 || c == 127)
	{
		int n = (int)strlen(AT.filter);

		if (n > 0)
		{
			AT.filter[n - 1] = 0;
			at_rebuild();
			if (AT.active)
				at_draw();
		}
		return G.out;
	}
	/* Printable characters start or extend the name filter (#1320). */
	if (c >= 32 && c <= 126)
	{
		int n = (int)strlen(AT.filter);

		if (n < (int)sizeof(AT.filter) - 1)
		{
			AT.filter[n] = c;
			AT.filter[n + 1] = 0;
		}
		at_rebuild();
		if (AT.active)
			at_draw();
		return G.out;
	}
	if (c == '\r' || c == '\n')
		at_run_selected();
	if (AT.active)
		at_draw();
	return G.out;
}

void mmb_apptui_poll(void)
{
	if (!AT.active || AT.esc != 1)
		return;
	if (mmb_now_ms() - AT.esc_at < APP_ESC_IDLE_MS)
		return;
	AT.esc = 0;
	at_escape();
	if (!AT.active)
		mmb_front_prompt();
}

void mmb_cmd_apps(void)
{
	mmb_apptui_open(1);
}

void mmb_boot_start(void)
{
	if (G.opt.boot_mode == 1)
	{
		mmb_apptui_open(1);
		return;
	}
	if (G.opt.boot_mode == 2 && G.opt.boot_app[0])
	{
		char path[APP_PATH], cmd[APP_PATH + 16];
		if (!mmb_app_resolve(G.opt.boot_app, path, sizeof(path)))
		{
			/* Not on the PATH: treat it as an explicit RUN target so a
			 * path or a plain .BAS still boots. */
			strncpy(path, G.opt.boot_app, sizeof(path) - 1);
			path[sizeof(path) - 1] = 0;
		}
		strcpy(cmd, "RUN \"");
		strncat(cmd, path, sizeof(cmd) - 8);
		strcat(cmd, "\"");
		mmb_console_write("\r\n");
		mmb_exec_line(cmd);
	}
	mmb_front_prompt();
}
