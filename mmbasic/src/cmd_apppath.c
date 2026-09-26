#include "mmb_priv.h"
#include "frontend.h"
#include "tui.h"

/*
 * App PATH and home launcher (#515, #520).
 *
 * - OPTION PATH lists directories to search for ``.APP`` packages. A bare
 *   token typed at the REPL prompt resolves to the first ``NAME.APP`` found
 *   and runs it with the same semantics as ``RUN "NAME.APP"``. Builtins and
 *   user SUBs/functions always win (the caller only falls through here).
 * - Ctrl+Space opens a quick picker of every ``.APP`` on the PATH.
 * - OPTION BOOT REPL|LAUNCHER|"name" decides what happens at power-on. The
 *   launcher is the same widget: Enter runs, Esc/back reaches the REPL.
 */

#define APP_MAX_ENT  160
#define APP_LABEL    48
#define APP_NAME     80
#define APP_PATH     160
#define APP_LISTING  4096
#define APP_ESC_IDLE_MS 60

#define APPTUI_PICK     0
#define APPTUI_LAUNCH   1

/* Built-in full-screen apps offered beside user .APP packages, in the order
 * shown in the launcher (labels are the human-readable names, #624). #793
 * dropped Package/Connect from the offer and put Terminal after Jukebox; the
 * PACKAGE and CONNECT commands still work when typed directly. */
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
#define AT_BRD_FG   ((int)atth()->brd_fg)
#define AT_BRD_BG   ((int)atth()->brd_bg)
#define AT_ERR_FG   ((int)atth()->err_fg)
#define AT_ERR_BG   ((int)atth()->err_bg)

typedef struct app_tui_state {
	int active;
	int mode;   /* APPTUI_PICK or APPTUI_LAUNCH */
	int nent, sel, top;
	app_entry ent[APP_MAX_ENT];
	int esc;
	unsigned esc_at;
	char status[96];
} app_tui_state;

static app_tui_state AT_s[MMB_MAX_CONSOLES];
#define AT (AT_s[g_console])

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

	p = G.opt.app_path;
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

static int at_seen_label(const char *label)
{
	int i;
	for (i = 0; i < AT.nent; i++)
		if (mmb_keyword_eq(AT.ent[i].label, label))
			return 1;
	return 0;
}

static int at_is_builtin_cmd(const char *name)
{
	int i;
	for (i = 0; i < AT_BUILTIN_COUNT; i++)
		if (mmb_keyword_eq(at_builtins[i].cmd, name))
			return 1;
	return 0;
}

static void at_add(const char *dir, const char *fname)
{
	char stem[APP_LABEL], full[APP_PATH + APP_NAME];
	int n;

	if (AT.nent >= APP_MAX_ENT)
		return;
	n = (int)strlen(fname);
	if (n < 5 || !mmb_keyword_eq(fname + n - 4, ".APP"))
		return;
	strncpy(stem, fname, sizeof(stem) - 1);
	stem[sizeof(stem) - 1] = 0;
	if (n - 4 < (int)sizeof(stem))
		stem[n - 4] = 0;
	if (!stem[0] || at_seen_label(stem) || at_is_builtin_cmd(stem))
		return;
	if (at_join(full, sizeof(full), dir, fname) != 0)
		return;
	AT.ent[AT.nent].kind = AT_KIND_PKG;
	strncpy(AT.ent[AT.nent].label, stem, APP_LABEL - 1);
	AT.ent[AT.nent].label[APP_LABEL - 1] = 0;
	strncpy(AT.ent[AT.nent].path, full, APP_PATH - 1);
	AT.ent[AT.nent].path[APP_PATH - 1] = 0;
	AT.nent++;
}

static void at_scan(void)
{
	char listing[APP_LISTING];
	const char *p = G.opt.app_path;
	int i;

	AT.nent = 0;
	AT.sel = 0;
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
		AT.nent++;
	}
}

static void at_close(void)
{
	AT.active = 0;
	AT.esc = 0;
	tui_end();
	/* Overlay (#624): paint the REPL screen back instead of a blank one. */
	if (!tui_overlay_end())
		mmb_console_write("\r\n");
}

static void at_run_selected(void)
{
	char cmd[APP_PATH + 16];

	if (AT.sel < 0 || AT.sel >= AT.nent)
		return;
	if (AT.ent[AT.sel].kind == AT_KIND_CMD)
	{
		strncpy(cmd, AT.ent[AT.sel].cmd, sizeof(cmd) - 1);
		cmd[sizeof(cmd) - 1] = 0;
		at_close();
		if (cmd[0])
			mmb_exec_line(cmd);
		return;
	}
	strcpy(cmd, "RUN \"");
	strncat(cmd, AT.ent[AT.sel].path, sizeof(cmd) - 8);
	strcat(cmd, "\"");
	at_close();
	mmb_exec_line(cmd);
}

static void at_escape(void)
{
	at_close();
}

static void at_move(int dir)
{
	if (dir < 0)
	{
		if (AT.sel > 0)
			AT.sel--;
	}
	else if (AT.sel + 1 < AT.nent)
		AT.sel++;
}

static void at_draw(void)
{
	int x, y, w, h;
	int i, listy, listh;
	const char *title = AT.mode == APPTUI_LAUNCH ? " HOME " : " APPS ";

	if (!AT.active)
		return;
	tui_begin();
	mmb_editor_apply_tui_palette();
	/* Same backdrop + panel chrome as SETTINGS (#793): fill the overlay with
	 * the menu surface, then draw a brd-framed dialog body. */
	tui_clear(AT_TITLE_FG, AT_TITLE_BG);
	/* A small centred overlay dialog (#624), like the editor quick open. */
	tui_dialog_geom(52, 16, &x, &y, &w, &h);
	tui_dialog_panel(x, y, w, h, title, AT_FG, AT_BG,
			 AT_BRD_FG, AT_BRD_BG, AT_TITLE_FG, AT_TITLE_BG);

	if (AT.mode == APPTUI_LAUNCH)
		tui_puts(x + 2, y + 2, "Choose an app. Esc returns to the REPL.",
			 AT_HOT, AT_BG);
	else
		tui_puts(x + 2, y + 2, "Choose an app to run.", AT_HOT, AT_BG);

	listy = y + 3;
	listh = (y + h - 2) - listy;
	if (listh < 1)
		listh = 1;
	if (AT.sel < AT.top)
		AT.top = AT.sel;
	if (AT.sel >= AT.top + listh)
		AT.top = AT.sel - listh + 1;
	if (AT.top < 0)
		AT.top = 0;

	for (i = 0; i < listh; i++)
	{
		int idx = AT.top + i;
		int ry = listy + i;
		int fg = AT_FG, bg = AT_BG;
		char row[APP_LABEL + 4];
		if (idx >= AT.nent)
		{
			tui_fill(x + 1, ry, w - 2, 1, ' ', AT_FG, AT_BG);
			continue;
		}
		row[0] = 0;
		at_append(row, sizeof(row), idx == AT.sel ? "> " : "  ");
		at_append(row, sizeof(row), AT.ent[idx].label);
		if (idx == AT.sel)
		{
			fg = AT_SEL_FG;
			bg = AT_SEL_BG;
		}
		tui_fill(x + 1, ry, w - 2, 1, ' ', fg, bg);
		tui_pad(x + 2, ry, row, w - 4, fg, bg);
	}

	if (AT.status[0])
		tui_puts(x + 2, y + h - 3, AT.status, AT_ERR_FG, AT_ERR_BG);
	if (AT.mode == APPTUI_LAUNCH)
		tui_status_hint_at(x + 1, y + h - 2, w - 2,
			"<Up/Down> Move  <Enter> Run  <Esc> REPL",
			AT_HOT, AT_FG, AT_BG);
	else
		tui_status_hint_at(x + 1, y + h - 2, w - 2,
			"<Up/Down> Move  <Enter> Run  <Esc> Cancel",
			AT_HOT, AT_FG, AT_BG);
	tui_cursor(0, 0, 0);
	tui_flush();
}

void mmb_apptui_open(int launcher)
{
	if (AT.active)
		return;
	tui_overlay_begin();
	memset(&AT, 0, sizeof(AT));
	AT.active = 1;
	AT.mode = launcher ? APPTUI_LAUNCH : APPTUI_PICK;
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
	if (AT.esc)
	{
		if (AT.esc == 1)
		{
			if (c == '[' || c == 'O')
			{
				AT.esc = 2;
				return G.out;
			}
			AT.esc = 0;
			at_escape();
			return G.out;
		}
		AT.esc = 0;
		if (c == 'A')
			at_move(-1);
		else if (c == 'B')
			at_move(1);
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
