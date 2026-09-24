#include "mmb_priv.h"
#include "tui.h"

static void strip_slash(char *s)
{
	int n;
	if (!s)
		return;
	n = (int)strlen(s);
	while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\'))
	{
		if (n == 3 && s[1] == ':')
			break;
		s[--n] = 0;
	}
}

static void join_rel(char *out, int outsz, const char *dir, const char *name)
{
	int n;
	out[0] = 0;
	if (dir && dir[0])
	{
		strncpy(out, dir, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
	n = (int)strlen(out);
	if (n && out[n - 1] != '/' && out[n - 1] != '\\' && n + 1 < outsz)
	{
		out[n++] = '/';
		out[n] = 0;
	}
	strncat(out, name, (unsigned)outsz - strlen(out) - 1);
}

static int confirm_overwrite(void)
{
	char *line;
	char *p;
	mmb_console_write("File exists, overwrite? [Y/n] ");
	if (!G.plat || !G.plat->read_line)
		return 0;
	line = mmb_read_line(0);
	p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == 0 || *p == 'Y' || *p == 'y')
		return 1;
	return 0;
}

static int folder_has_main(const char *folder)
{
	char p[160];
	join_rel(p, sizeof(p), folder, "MAIN.BAS");
	return mmb_vfs_exists(p) && !mmb_vfs_isdir(p);
}

#define PKG_DIR_ENT_INIT 64
/* Safety cap on a single folder scan (entries, not bytes). The listing is
 * heap-allocated and grows on demand, so this only bounds a pathological
 * folder; reaching it fails the pack rather than omitting files. */
#define PKG_DIR_ENT_MAX 16384

/* Read one folder into a heap mmb_dirent array (#706). The structured listing
 * is a bounded smallest-N selection, so a truncated scan means "retry with
 * room for more", not "the folder is too big": grow until the whole folder
 * fits. Returns the entry count (>=0), storing the array in *out (0 when the
 * folder is empty), or -1 on failure. *out_trunc is set only when even
 * PKG_DIR_ENT_MAX could not hold the folder. */
static int pkg_list_dir(const char *absdir, mmb_dirent **out, int *out_trunc)
{
	int cap = PKG_DIR_ENT_INIT;
	*out = 0;
	*out_trunc = 0;
	for (;;)
	{
		mmb_dirent *ents = (mmb_dirent *)G.plat->alloc(
			(unsigned)cap * sizeof(mmb_dirent));
		int n, truncated = 0;
		if (!ents)
			return -1;
		n = mmb_vfs_list_entries(absdir, ents, cap, &truncated);
		if (n < 0)
		{
			G.plat->free(ents);
			return -1;
		}
		if (!truncated)
		{
			*out = ents;
			return n;
		}
		G.plat->free(ents);
		if (cap >= PKG_DIR_ENT_MAX)
		{
			*out_trunc = 1;
			return 0;
		}
		cap *= 2;
		if (cap > PKG_DIR_ENT_MAX)
			cap = PKG_DIR_ENT_MAX;
	}
}

static int pack_walk(mmb_zip_w *z, const char *absdir, const char *rel,
		     const char *dest_full, int depth)
{
	mmb_dirent *ents = 0;
	int rc = 0, n, i, truncated = 0;
	if (depth > 8)
		return -1;
	n = pkg_list_dir(absdir, &ents, &truncated);
	if (n < 0)
		return -1;
	/* A cut folder listing would silently omit files from the archive; fail
	 * the pack rather than write an incomplete package (#693). */
	if (truncated)
		return -1;
	if (n == 0)
	{
		if (rel[0])
		{
			char d[128];
			strncpy(d, rel, sizeof(d) - 2);
			d[sizeof(d) - 2] = 0;
			if (d[strlen(d) - 1] != '/')
				strcat(d, "/");
			rc = mmb_zip_add(z, d, 0, 0);
		}
		G.plat->free(ents);
		return rc;
	}
	for (i = 0; i < n && rc == 0; i++)
	{
		char name[MMB_DIRENT_NAME], child_abs[160], child_rel[128], resolved[128];
		int is_dir = ents[i].is_dir;
		if (!ents[i].name[0] || mmb_vfs_hidden_name(ents[i].name))
			continue;
		strncpy(name, ents[i].name, sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		join_rel(child_abs, sizeof(child_abs), absdir, name);
		if (rel[0])
			join_rel(child_rel, sizeof(child_rel), rel, name);
		else
		{
			strncpy(child_rel, name, sizeof(child_rel) - 1);
			child_rel[sizeof(child_rel) - 1] = 0;
		}
		if (mmb_vfs_resolve(child_abs, resolved, sizeof(resolved)) == 0 &&
		    dest_full[0] && mmb_keyword_eq(resolved, dest_full))
			continue;
		if (is_dir)
		{
			rc = pack_walk(z, child_abs, child_rel, dest_full, depth + 1);
			continue;
		}
		{
			int sz = ents[i].size;
			unsigned got = 0;
			unsigned char *buf;
			if (sz < 0)
			{
				rc = -1;
				break;
			}
			buf = G.plat->alloc((unsigned)sz + 1);
			if (!buf)
			{
				rc = -1;
				break;
			}
			if (mmb_vfs_read(child_abs, buf, (unsigned)sz, &got) != 0)
			{
				G.plat->free(buf);
				rc = -1;
				break;
			}
			if (mmb_zip_add(z, child_rel, buf, got) != 0)
			{
				G.plat->free(buf);
				rc = -1;
				break;
			}
			G.plat->free(buf);
		}
	}
	G.plat->free(ents);
	return rc;
}

typedef struct mmb_unpack_ctx {
	int err; /* 0 none, 1 generic, 2 file exists */
} mmb_unpack_ctx;

static int unpack_mkdirs(const char *rel)
{
	char buf[160];
	char *p;
	strncpy(buf, rel, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	p = buf;
	while (*p)
	{
		if (*p == '/')
		{
			*p = 0;
			if (buf[0] && !mmb_vfs_isdir(buf) && mmb_vfs_mkdir(buf) != 0)
			{
				*p = '/';
				return -1;
			}
			*p = '/';
		}
		p++;
	}
	return 0;
}

static int unpack_add_file(const char *path, const void *data, unsigned n, void *ctx)
{
	mmb_unpack_ctx *u = (mmb_unpack_ctx *)ctx;
	char rel[160];

	strncpy(rel, path, sizeof(rel) - 1);
	rel[sizeof(rel) - 1] = 0;
	if (!mmb_zip_path_ok(rel) || unpack_mkdirs(rel) != 0)
	{
		u->err = 1;
		return -1;
	}
	if (n == 0 && !data)
	{
		if (!mmb_vfs_isdir(rel) && mmb_vfs_mkdir(rel) != 0)
		{
			u->err = 1;
			return -1;
		}
		return 0;
	}
	if (mmb_vfs_exists(rel) && !mmb_vfs_isdir(rel))
	{
		if (G.running)
		{
			u->err = 2;
			return -1;
		}
		if (!confirm_overwrite())
			return 0;
	}
	if (mmb_vfs_write(rel, data, n, 0) != 0)
	{
		u->err = 1;
		return -1;
	}
	return 0;
}

void mmb_cmd_unpack(void)
{
	char arc[128];
	unsigned char *zip;
	unsigned got = 0;
	int sz;
	mmb_unpack_ctx ctx;
	mmb_val v;

	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(arc, v.s, sizeof(arc) - 1);
	arc[sizeof(arc) - 1] = 0;
	if (!arc[0])
		mmb_error("?FILE NOT FOUND");
	sz = mmb_vfs_size(arc);
	if (sz < 0)
		mmb_error("?FILE NOT FOUND");
	if ((unsigned)sz > MMB_ZIP_MAX_BYTES)
		mmb_error("?UNPACK");
	zip = G.plat->alloc((unsigned)sz + 1);
	if (!zip)
		mmb_error("?OUT OF MEMORY");
	if (mmb_vfs_read(arc, zip, (unsigned)sz, &got) != 0)
	{
		G.plat->free(zip);
		mmb_error("?FILE NOT FOUND");
	}
	ctx.err = 0;
	if (mmb_zip_foreach(zip, got, unpack_add_file, &ctx) != 0)
	{
		int e = ctx.err;
		G.plat->free(zip);
		mmb_error(e == 2 ? "?FILE EXISTS" : "?UNPACK");
	}
	G.plat->free(zip);
}

/* Zip folder$ into pkg$ (a ZIP store). When meta$ is non-empty it is added
 * as PACKAGE.INF at the archive root. Returns 0 on success, otherwise:
 * 1 ?NO MAIN.BAS, 2 ?DIRECTORY, 3 ?PACKAGE, 4 ?FILE. */
static int package_build(const char *pkg, const char *folder_in, const char *meta)
{
	char folder[128], dest_full[128];
	mmb_zip_w z;
	unsigned char *out = 0;
	unsigned n = 0;

	strncpy(folder, folder_in, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = 0;
	strip_slash(folder);
	if (!folder[0] || !mmb_vfs_isdir(folder))
		return 2;
	if (!folder_has_main(folder))
		return 1;
	dest_full[0] = 0;
	mmb_vfs_resolve(pkg, dest_full, sizeof(dest_full));
	if (mmb_zip_begin(&z) != 0)
		return 3;
	if (pack_walk(&z, folder, "", dest_full, 0) != 0)
	{
		mmb_zip_abort(&z);
		return 3;
	}
	if (meta && meta[0] &&
	    mmb_zip_add(&z, "PACKAGE.INF", meta, (unsigned)strlen(meta)) != 0)
	{
		mmb_zip_abort(&z);
		return 3;
	}
	if (mmb_zip_finish(&z, &out, &n) != 0)
	{
		mmb_zip_abort(&z);
		return 3;
	}
	if (mmb_vfs_write(pkg, out, n, 0) != 0)
	{
		G.plat->free(out);
		return 4;
	}
	G.plat->free(out);
	return 0;
}

static void package_error(int rc)
{
	if (rc == 1)
		mmb_error("?NO MAIN.BAS");
	if (rc == 2)
		mmb_error("?DIRECTORY");
	if (rc == 4)
		mmb_error("?FILE");
	mmb_error("?PACKAGE");
}

void mmb_cmd_package(void)
{
	char pkg[128], folder[128];
	mmb_val v;

	/* Bare PACKAGE at the prompt is the authoring wizard; scripts keep the
	 * two-argument form (and a bare PACKAGE there stays a syntax error). */
	mmb_skip_sp();
	if (!G.running && (*G.p == 0 || *G.p == ':' || *G.p == '\''))
	{
		mmb_package_wiz_open();
		return;
	}
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(pkg, v.s, sizeof(pkg) - 1);
	pkg[sizeof(pkg) - 1] = 0;
	mmb_skip_sp();
	if (*G.p == ',')
		G.p++;
	mmb_skip_sp();
	v = mmb_expr();
	if (v.type != T_STR)
		mmb_syntax();
	strncpy(folder, v.s, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = 0;
	strip_slash(folder);
	if (!strchr(pkg, '.'))
		strncat(pkg, ".APP", sizeof(pkg) - strlen(pkg) - 1);
	if (!folder[0] || !mmb_vfs_isdir(folder))
		mmb_error("?DIRECTORY");
	if (!folder_has_main(folder))
		mmb_error("?NO MAIN.BAS");
	if (mmb_vfs_exists(pkg) && !mmb_vfs_isdir(pkg))
	{
		if (G.running)
			mmb_error("?FILE EXISTS");
		if (!confirm_overwrite())
			return;
	}
	{
		int rc = package_build(pkg, folder, 0);
		if (rc)
			package_error(rc);
	}
}

/* ------------------------------------------------------------------ */
/* PACKAGE authoring wizard: folder -> .APP                            */
/* ------------------------------------------------------------------ */

#define PW_MAX_ENT 128
#define PW_NAME    80
#define PW_PATH    160
#define PW_LIST    4096

#define PW_FOLDER 0
#define PW_FORM   1
#define PW_DONE   2

#define PW_ESC_IDLE_MS 60

static const mmb_ed_theme *pwth(void)
{
	return mmb_editor_theme();
}

#define PW_FG       ((int)pwth()->edit_fg)
#define PW_BG       ((int)pwth()->edit_bg)
#define PW_TITLE_FG ((int)pwth()->menu_fg)
#define PW_TITLE_BG ((int)pwth()->menu_bg)
#define PW_SEL_FG   ((int)pwth()->sel_fg)
#define PW_SEL_BG   ((int)pwth()->sel_bg)
#define PW_HOT      ((int)pwth()->hot)
#define PW_DIM      ((int)pwth()->cmt_fg)
#define PW_STR      ((int)pwth()->str_fg)
#define PW_BRD      ((int)pwth()->brd_fg)
#define PW_BRD_BG   ((int)pwth()->brd_bg)
#define PW_ERR_FG   ((int)pwth()->err_fg)
#define PW_ERR_BG   ((int)pwth()->err_bg)
#define PW_FIELD_FG ((int)pwth()->field_fg)
#define PW_FIELD_BG ((int)pwth()->field_bg)

typedef struct {
	int active;
	int phase;
	int nent, sel, top;
	char cwd[128];
	char name[PW_MAX_ENT][PW_NAME];
	char path[PW_MAX_ENT][PW_PATH];
	char folder[PW_PATH];
	char pkg[PW_NAME];
	char title[PW_NAME];
	char author[PW_NAME];
	int field;
	int overwrite;
	int esc;
	unsigned esc_at;
	char status[96];
} pw_state;

static pw_state PW_s[MMB_MAX_CONSOLES];
#define PW (PW_s[g_console])

static void pw_draw(void);
static void pw_scan(void);

static void pw_set_status(const char *s)
{
	strncpy(PW.status, s ? s : "", sizeof(PW.status) - 1);
	PW.status[sizeof(PW.status) - 1] = 0;
}

static void pw_append(char *dst, int sz, const char *s)
{
	int n = (int)strlen(dst);
	if (n >= sz - 1 || !s)
		return;
	strncat(dst, s, (unsigned)(sz - n - 1));
}

static int pw_has_main(int i)
{
	return folder_has_main(PW.path[i]);
}

static void pw_scan(void)
{
	char listing[PW_LIST];
	char *line, *next;
	int n = 1, truncated = 0;

	PW.sel = 0;
	PW.top = 0;
	PW.nent = 0;
	strncpy(PW.cwd, mmb_vfs_cwd(), sizeof(PW.cwd) - 1);
	PW.cwd[sizeof(PW.cwd) - 1] = 0;
	strcpy(PW.name[0], ".");
	strncpy(PW.path[0], PW.cwd, sizeof(PW.path[0]) - 1);
	PW.path[0][sizeof(PW.path[0]) - 1] = 0;
	if (mmb_vfs_list(PW.cwd, listing, sizeof(listing), &truncated) != 0)
		listing[0] = 0;
	if (truncated)
		pw_set_status("... more: folder listing was cut");
	for (line = listing; line && *line && n < PW_MAX_ENT; line = next)
	{
		int len;
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		len = (int)strlen(line);
		if (len < 2 || line[len - 1] != '/')
			continue; /* files are not package roots */
		line[len - 1] = 0;
		if (!line[0] || mmb_vfs_hidden_name(line))
			continue;
		strncpy(PW.name[n], line, PW_NAME - 1);
		PW.name[n][PW_NAME - 1] = 0;
		join_rel(PW.path[n], (int)sizeof(PW.path[n]), PW.cwd, line);
		n++;
	}
	PW.nent = n;
}

static void pw_default_name(void)
{
	const char *base = PW.name[PW.sel];
	PW.pkg[0] = 0;
	if (!base[0] || !strcmp(base, "."))
		base = "PACKAGE";
	strncpy(PW.pkg, base, sizeof(PW.pkg) - 6);
	PW.pkg[sizeof(PW.pkg) - 6] = 0;
	strcat(PW.pkg, ".APP");
}

static void pw_close(void)
{
	PW.active = 0;
	PW.esc = 0;
	tui_end();
	mmb_console_write("\r\n");
	mmb_console_write(mmb_prompt());
}

static void pw_escape(void)
{
	if (PW.phase == PW_FORM)
	{
		PW.phase = PW_FOLDER;
		PW.overwrite = 0;
		pw_set_status("");
		return;
	}
	pw_close();
}

static void pw_choose_folder(void)
{
	int i = PW.sel;
	if (i < 0 || i >= PW.nent)
		return;
	if (!pw_has_main(i))
	{
		char msg[96];
		strncpy(msg, "No MAIN.BAS in ", sizeof(msg) - 1);
		msg[sizeof(msg) - 1] = 0;
		pw_append(msg, sizeof(msg), PW.name[i]);
		pw_set_status(msg);
		return;
	}
	strncpy(PW.folder, PW.path[i], sizeof(PW.folder) - 1);
	PW.folder[sizeof(PW.folder) - 1] = 0;
	pw_default_name();
	PW.title[0] = 0;
	PW.author[0] = 0;
	PW.field = 0;
	PW.overwrite = 0;
	PW.phase = PW_FORM;
	pw_set_status("");
}

static void pw_build_meta(char *dst, int sz)
{
	dst[0] = 0;
	pw_append(dst, sz, "name=");
	pw_append(dst, sz, PW.pkg);
	pw_append(dst, sz, "\nmain=MAIN.BAS\n");
	if (PW.title[0])
	{
		pw_append(dst, sz, "title=");
		pw_append(dst, sz, PW.title);
		pw_append(dst, sz, "\n");
	}
	if (PW.author[0])
	{
		pw_append(dst, sz, "author=");
		pw_append(dst, sz, PW.author);
		pw_append(dst, sz, "\n");
	}
}

static void pw_create(void)
{
	char meta[512];
	int rc;

	if (!PW.pkg[0])
	{
		pw_set_status("Package name required");
		return;
	}
	if (!strchr(PW.pkg, '.'))
		strncat(PW.pkg, ".APP", sizeof(PW.pkg) - strlen(PW.pkg) - 1);
	if (mmb_vfs_exists(PW.pkg) && !mmb_vfs_isdir(PW.pkg) && !PW.overwrite)
	{
		PW.overwrite = 1;
		pw_set_status("File exists: Enter overwrites, Esc goes back");
		return;
	}
	pw_build_meta(meta, sizeof(meta));
	rc = package_build(PW.pkg, PW.folder, meta);
	if (rc != 0)
	{
		if (rc == 1)
			pw_set_status("?NO MAIN.BAS");
		else if (rc == 2)
			pw_set_status("?DIRECTORY");
		else if (rc == 4)
			pw_set_status("?FILE");
		else
			pw_set_status("?PACKAGE");
		return;
	}
	PW.phase = PW_DONE;
	pw_set_status("");
}

static void pw_move(int dir)
{
	if (PW.phase == PW_FOLDER)
	{
		if (dir < 0)
		{
			if (PW.sel > 0)
				PW.sel--;
		}
		else if (PW.sel + 1 < PW.nent)
			PW.sel++;
	}
	else if (PW.phase == PW_FORM)
	{
		if (dir < 0)
		{
			if (PW.field > 0)
				PW.field--;
		}
		else if (PW.field < 2)
			PW.field++;
	}
}

static void pw_key_folder(char c)
{
	if (c == '\r' || c == '\n')
	{
		pw_choose_folder();
		return;
	}
}

static void pw_field_edit(char c)
{
	char *buf;
	int cap;
	if (PW.field == 0)
	{
		buf = PW.pkg;
		cap = (int)sizeof(PW.pkg);
	}
	else if (PW.field == 1)
	{
		buf = PW.title;
		cap = (int)sizeof(PW.title);
	}
	else
	{
		buf = PW.author;
		cap = (int)sizeof(PW.author);
	}
	if (c == 8 || c == 127)
	{
		int n = (int)strlen(buf);
		if (n > 0)
			buf[n - 1] = 0;
		return;
	}
	if (c >= 32 && c < 127)
	{
		int n = (int)strlen(buf);
		if (n + 1 < cap)
		{
			buf[n] = c;
			buf[n + 1] = 0;
		}
	}
}

static void pw_key_form(char c)
{
	if (c == '\t')
	{
		if (PW.field < 2)
			PW.field++;
		else
			PW.field = 0;
		return;
	}
	if (c == '\r' || c == '\n')
	{
		if (PW.field < 2)
			PW.field++;
		else
			pw_create();
		return;
	}
	pw_field_edit(c);
}

static void pw_key_done(void)
{
	if (!PW.active)
		return;
	pw_close();
}

const char *mmb_package_key(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!PW.active)
		return G.out;
	if (PW.esc)
	{
		if (PW.esc == 1)
		{
			if (c == '[' || c == 'O')
			{
				PW.esc = 2;
				return G.out;
			}
			PW.esc = 0;
			pw_escape();
			if (PW.active)
				pw_draw();
			return G.out;
		}
		PW.esc = 0;
		if (c == 'A')
			pw_move(-1);
		else if (c == 'B')
			pw_move(1);
		if (PW.active)
			pw_draw();
		return G.out;
	}
	if (c == 27)
	{
		PW.esc = 1;
		PW.esc_at = mmb_now_ms();
		return G.out;
	}
	if (PW.phase == PW_FOLDER)
		pw_key_folder(c);
	else if (PW.phase == PW_FORM)
		pw_key_form(c);
	else
		pw_key_done();
	if (PW.active)
		pw_draw();
	return G.out;
}

void mmb_package_poll(void)
{
	if (!PW.active || PW.esc != 1)
		return;
	if (mmb_now_ms() - PW.esc_at < PW_ESC_IDLE_MS)
		return;
	PW.esc = 0;
	pw_escape();
	if (PW.active)
		pw_draw();
}

int mmb_in_package(void)
{
	return PW.active;
}

void mmb_package_wiz_open(void)
{
	memset(&PW, 0, sizeof(PW));
	PW.active = 1;
	PW.phase = PW_FOLDER;
	tui_begin();
	mmb_editor_apply_tui_palette();
	tui_invalidate();
	pw_scan();
	pw_draw();
}

/* Modal dialog chrome (#590): backdrop + centred framed panel. Returns the
 * panel rect; content goes at (x+2, y+2), the status hint sits on y+h-2. */
static void pw_panel(const char *title, int want_w, int want_h,
		     int *x, int *y, int *w, int *h)
{
	tui_clear(PW_TITLE_FG, PW_TITLE_BG);
	tui_dialog_geom(want_w, want_h, x, y, w, h);
	tui_dialog_panel(*x, *y, *w, *h, title, PW_FG, PW_BG,
			 PW_BRD, PW_BRD_BG, PW_TITLE_FG, PW_TITLE_BG);
}

static void pw_status_bar(int x, int row, int width, const char *hint)
{
	tui_status_hint_at(x, row, width, hint, PW_HOT, PW_DIM, PW_BG);
}

static void pw_draw_folder(void)
{
	int x, y, w, h;
	int listy, listh, i;
	char line[PW_PATH + 32];

	pw_panel("PACKAGE WIZARD", 72, 17, &x, &y, &w, &h);
	tui_puts(x + 2, y + 2, "Step 1/3  Choose the folder to package", PW_STR, PW_BG);
	line[0] = 0;
	pw_append(line, sizeof(line), "Folder: ");
	pw_append(line, sizeof(line), PW.cwd);
	tui_puts(x + 2, y + 3, line, PW_DIM, PW_BG);

	listy = y + 5;
	listh = (y + h - 3) - listy;
	if (listh < 3)
		listh = 3;
	if (PW.sel < PW.top)
		PW.top = PW.sel;
	if (PW.sel >= PW.top + listh)
		PW.top = PW.sel - listh + 1;
	if (PW.top < 0)
		PW.top = 0;

	for (i = 0; i < listh; i++)
	{
		int idx = PW.top + i;
		int ry = listy + i;
		char row[PW_NAME + 24];
		int fg = PW_FG, bg = PW_BG;
		if (idx >= PW.nent)
		{
			tui_fill(x + 1, ry, w - 2, 1, ' ', PW_FG, PW_BG);
			continue;
		}
		row[0] = 0;
		pw_append(row, sizeof(row), "[");
		pw_append(row, sizeof(row), PW.name[idx]);
		pw_append(row, sizeof(row), "]");
		if (!pw_has_main(idx))
			pw_append(row, sizeof(row), "  (no MAIN.BAS)");
		else
			pw_append(row, sizeof(row), "  MAIN.BAS ok");
		if (idx == PW.sel)
		{
			fg = PW_SEL_FG;
			bg = PW_SEL_BG;
		}
		else if (!pw_has_main(idx))
			fg = PW_ERR_FG;
		tui_fill(x + 1, ry, w - 2, 1, ' ', fg, bg);
		tui_puts(x + 2, ry, row, fg, bg);
	}

	if (PW.status[0])
		tui_fill(x + 1, y + h - 3, w - 2, 1, ' ', PW_ERR_FG, PW_ERR_BG);
	else
		tui_fill(x + 1, y + h - 3, w - 2, 1, ' ', PW_FG, PW_BG);
	tui_puts(x + 2, y + h - 3, PW.status[0] ? PW.status :
		 "Pick a folder that contains MAIN.BAS.", PW_ERR_FG, PW_ERR_BG);
	pw_status_bar(x + 1, y + h - 2, w - 2,
		      "<Up/Down> Move  <Enter> Choose  <Esc> Cancel");
}

static void pw_draw_field(int x, int y, int w, const char *label,
			  const char *value, int selected)
{
	int fg = selected ? PW_SEL_FG : PW_FIELD_FG;
	int bg = selected ? PW_SEL_BG : PW_FIELD_BG;
	char row[PW_NAME + 32];
	row[0] = 0;
	pw_append(row, sizeof(row), label);
	pw_append(row, sizeof(row), value);
	tui_fill(x + 1, y, w - 2, 1, ' ', fg, bg);
	tui_puts(x + 2, y, row, fg, bg);
	if (selected)
		tui_put(x + 2 + (int)strlen(row), y, ' ', PW_SEL_FG, PW_SEL_BG);
}

static void pw_draw_form(void)
{
	int x, y, w, h;
	char line[PW_PATH + 32];

	pw_panel("PACKAGE WIZARD", 72, 14, &x, &y, &w, &h);
	tui_puts(x + 2, y + 2, "Step 2/3  Name and optional metadata", PW_STR, PW_BG);
	line[0] = 0;
	pw_append(line, sizeof(line), "Folder: ");
	pw_append(line, sizeof(line), PW.folder);
	pw_append(line, sizeof(line), "  (MAIN.BAS ok)");
	tui_puts(x + 2, y + 3, line, PW_DIM, PW_BG);

	pw_draw_field(x, y + 5, w, "Package : ", PW.pkg, PW.field == 0);
	pw_draw_field(x, y + 6, w, "Title   : ", PW.title, PW.field == 1);
	pw_draw_field(x, y + 7, w, "Author  : ", PW.author, PW.field == 2);

	tui_fill(x + 1, y + h - 3, w - 2, 1, ' ', PW_FG, PW_BG);
	if (PW.status[0])
		tui_puts(x + 2, y + h - 3, PW.status, PW_ERR_FG, PW_ERR_BG);
	else
		tui_puts(x + 2, y + h - 3,
			 "Enter advances; on the last field it creates the .APP.",
			 PW_DIM, PW_BG);
	pw_status_bar(x + 1, y + h - 2, w - 2,
		      "<Tab/Up/Down> Field  <Enter> Next/Create  <Esc> Back");
}

static void pw_draw_done(void)
{
	int x, y, w, h;
	char line[PW_PATH + 64];

	pw_panel("PACKAGE WIZARD", 72, 13, &x, &y, &w, &h);
	tui_puts(x + 2, y + 2, "Step 3/3  Package created", PW_STR, PW_BG);
	line[0] = 0;
	pw_append(line, sizeof(line), "Wrote ");
	pw_append(line, sizeof(line), PW.pkg);
	pw_append(line, sizeof(line), " from ");
	pw_append(line, sizeof(line), PW.folder);
	tui_puts(x + 2, y + 4, line, PW_FG, PW_BG);
	tui_puts(x + 2, y + 6, "RUN \"<name>.APP\" mounts it read-only as B:.",
		 PW_DIM, PW_BG);
	tui_fill(x + 1, y + h - 3, w - 2, 1, ' ', PW_FG, PW_BG);
	pw_status_bar(x + 1, y + h - 2, w - 2, "<Any key> Close");
}

static void pw_draw(void)
{
	if (!PW.active)
		return;
	tui_begin();
	mmb_editor_apply_tui_palette();
	if (PW.phase == PW_FOLDER)
		pw_draw_folder();
	else if (PW.phase == PW_FORM)
		pw_draw_form();
	else
		pw_draw_done();
	tui_cursor(0, 0, 0);
	tui_flush();
}
