/*
 * PAINT - file Open / Save / Save As (#640).
 *
 * Strong definitions replace the weak stubs at the bottom of cmd_paint.c.
 * cmd_paint.c owns the event loop and calls the pt_file_* hooks declared in the
 * frozen paint.h; this module owns the canvas <-> PCX round trip and the state
 * of the modal file dialog.
 *
 * The dialog itself is the reusable picker implemented in cmd_files_ui.c
 * (#640). It browses the VFS (A:, C:, USB) and returns a chosen path without
 * touching the FILES browser state. Its prototypes are declared here rather
 * than in a frozen header so the PAINT and FILES modules stay file-disjoint.
 *
 * Behaviour:
 *   - New canvas resets to black (index 0) and marks the document clean.
 *   - Save reuses the last path; with none it falls back to Save As.
 *   - Save As / Open run the picker; Esc cancels and leaves state unchanged.
 *   - A chosen name without an extension gets .PCX
 *     (mmb_files_pick_result()). mmb_pcx_encode/decode_indexed do the codec
 *     work; the file always uses the fixed VGA palette.
 *   - A successful save clears the unsaved marker (the menus warn via
 *     PT.undo_depth, so pt_undo_clear() is the clean signal).
 */
#include "mmb_priv.h"
#include "paint.h"
#include "pcx.h"
#include "tui.h"

/* ---- reusable picker (cmd_files_ui.c, #640) ---------------------------- */

void mmb_files_pick_begin(int save, const char *start_dir, const char *seed);
int mmb_files_pick_active(void);
int mmb_files_pick_done(void);
int mmb_files_pick_cancelled(void);
const char *mmb_files_pick_result(void);
int mmb_files_pick_key(int key);
int mmb_files_pick_poll(void);
void mmb_files_pick_render(void);
void mmb_files_pick_compose(void);
int mmb_files_pick_geom(int *x, int *y, int *w, int *h);
void mmb_files_pick_end(void);

/* ---- module state ------------------------------------------------------ */

enum { PF_NONE = 0, PF_OPEN, PF_SAVEAS };

/* One dialog state per virtual console: the open picker purpose and its
 * overlay bookkeeping must not cross consoles (#766). */
typedef struct {
	int mode;		/* active picker purpose */
	int pick_drawn;		/* overlay painted on the previous frame (#722) */
	int pick_x, pick_y, pick_w, pick_h;
} pt_file_state;

static pt_file_state s_file_state[MMB_MAX_CONSOLES];
#define PICK (s_file_state[g_console])

static const char *pf_base(const char *p)
{
	const char *s = p;

	while (p && *p)
	{
		if (*p == '/' || *p == ':' || *p == '\\')
			s = p + 1;
		p++;
	}
	return s;
}

/* Directory part of a path, including the trailing '/', or "" when there is
 * none. Falls back to the current working directory when `p` is empty. */
static void pf_dir_of(const char *p, char *out, int outsz)
{
	const char *slash = 0, *q;

	if (!p || !p[0])
		p = mmb_vfs_cwd();
	for (q = p; *q; q++)
		if (*q == '/' || *q == '\\')
			slash = q;
	if (!slash)
	{
		strncpy(out, p, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
		return;
	}
	if (slash - p + 1 >= outsz)
		slash = p + outsz - 2;
	memcpy(out, p, (unsigned)(slash - p + 1));
	out[slash - p + 1] = 0;
}

/* Turn a picker result into a canonical VFS path. Adds the .PCX extension
 * when the basename has none and prefixes the current drive for a bare
 * relative name. */
static void pf_full_path(const char *in, char *out, int outsz)
{
	char tmp[160];
	const char *b;

	if (!in || !in[0])
	{
		out[0] = 0;
		return;
	}
	strncpy(tmp, in, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	b = pf_base(tmp);
	if (!strchr(b, '.'))
		strncat(tmp, ".PCX", sizeof(tmp) - strlen(tmp) - 1);
	if (!strchr(tmp, ':') && tmp[0] != '/')
	{
		char full[192];
		sprintf(full, "%c:/%s", G.drive ? G.drive : 'A', tmp);
		if (mmb_vfs_resolve(full, out, outsz) == 0)
			return;
	}
	if (mmb_vfs_resolve(tmp, out, outsz) != 0)
	{
		strncpy(out, tmp, (unsigned)outsz - 1);
		out[outsz - 1] = 0;
	}
}

static void pf_status(const char *fmt, const char *a)
{
	char tmp[192];

	if (a)
		sprintf(tmp, fmt, a);
	else
	{
		strncpy(tmp, fmt, sizeof(tmp) - 1);
		tmp[sizeof(tmp) - 1] = 0;
	}
	strncpy(PT.status, tmp, sizeof(PT.status) - 1);
	PT.status[sizeof(PT.status) - 1] = 0;
}

/* ---- PCX save / load --------------------------------------------------- */

static int pf_save(const char *path)
{
	char full[160];
	unsigned char *pcx = 0;
	unsigned n = 0;

	pf_full_path(path, full, sizeof(full));
	if (!full[0])
		return -1;
	if (mmb_pcx_encode_indexed(PT.canvas, PT.width, PT.height, 0, &pcx, &n) != 0)
	{
		pf_status("?Save failed", 0);
		return -1;
	}
	if (mmb_vfs_write(full, pcx, n, 0) != 0)
	{
		G.plat->free(pcx);
		pf_status("?Save failed", 0);
		return -1;
	}
	G.plat->free(pcx);
	strncpy(PT.path, full, sizeof(PT.path) - 1);
	PT.path[sizeof(PT.path) - 1] = 0;
	strncpy(PT.label, pf_base(PT.path), sizeof(PT.label) - 1);
	PT.label[sizeof(PT.label) - 1] = 0;
	pt_undo_clear();		/* the document is clean */
	pf_status("Saved %s", PT.label);
	return 0;
}

static int pf_resize_canvas(int w, int h)
{
	unsigned char *nc, *ns;
	unsigned bytes;

	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (w > PT_MAX_W)
		w = PT_MAX_W;
	if (h > PT_MAX_H)
		h = PT_MAX_H;
	bytes = (unsigned)w * (unsigned)h;
	if (w == PT.width && h == PT.height && PT.canvas && PT.scratch)
		return 0;
	nc = G.plat->alloc(bytes);
	ns = G.plat->alloc(bytes);
	if (!nc || !ns)
	{
		if (nc)
			G.plat->free(nc);
		if (ns)
			G.plat->free(ns);
		return -1;
	}
	if (PT.canvas)
		G.plat->free(PT.canvas);
	if (PT.scratch)
		G.plat->free(PT.scratch);
	PT.canvas = nc;
	PT.scratch = ns;
	PT.width = w;
	PT.height = h;
	PT.scratch_valid = 0;
	return 1;
}

static int pf_open(const char *path)
{
	char full[160];
	unsigned char *file, *idx = 0, pal[768];
	unsigned sz, got = 0, y, copyw;
	int w = 0, h = 0, ncol = 0, changed, size;

	pf_full_path(path, full, sizeof(full));
	if (!full[0])
		return -1;
	size = mmb_vfs_size(full);
	if (size <= 0)
	{
		pf_status("?Open failed", 0);
		return -1;
	}
	sz = (unsigned)size;
	file = G.plat->alloc(sz);
	if (!file)
	{
		pf_status("?Out of memory", 0);
		return -1;
	}
	if (mmb_vfs_read(full, file, sz, &got) != 0)
	{
		G.plat->free(file);
		pf_status("?Open failed", 0);
		return -1;
	}
	if (mmb_pcx_decode_indexed(file, got, &idx, &w, &h, pal, &ncol) != 0)
	{
		G.plat->free(file);
		pf_status("?Not a PCX", 0);
		return -1;
	}
	G.plat->free(file);

	(void)pal;
	(void)ncol;
	changed = pf_resize_canvas(w, h);
	if (changed < 0)
	{
		G.plat->free(idx);
		pf_status("?Out of memory", 0);
		return -1;
	}
	memset(PT.canvas, 0, (unsigned)PT.width * (unsigned)PT.height);
	copyw = (unsigned)w < (unsigned)PT.width ? (unsigned)w : (unsigned)PT.width;
	for (y = 0; y < (unsigned)PT.height && y < (unsigned)h; y++)
		memcpy(PT.canvas + y * (unsigned)PT.width, idx + y * (unsigned)w,
		       copyw);
	G.plat->free(idx);
	pt_undo_init();			/* history belongs to the new canvas */
	strncpy(PT.path, full, sizeof(PT.path) - 1);
	PT.path[sizeof(PT.path) - 1] = 0;
	strncpy(PT.label, pf_base(PT.path), sizeof(PT.label) - 1);
	PT.label[sizeof(PT.label) - 1] = 0;
	pt_undo_clear();
	pf_status("Opened %s", PT.label);
	return 0;
}

/* ---- picker plumbing --------------------------------------------------- */

static void pf_begin(int mode)
{
	char dir[128];

	PICK.mode = mode;
	if (mode == PF_OPEN)
	{
		pf_dir_of(PT.path, dir, sizeof(dir));
		mmb_files_pick_begin(0, dir, "");
		pf_status("Open PCX", 0);
	}
	else
	{
		pf_dir_of(PT.path, dir, sizeof(dir));
		mmb_files_pick_begin(1, dir, PT.label[0] ? PT.label : "");
		pf_status("Save as", 0);
	}
	pt_request_redraw();
}

static void pf_commit(const char *path)
{
	if (PICK.mode == PF_OPEN)
		pf_open(path);
	else if (PICK.mode == PF_SAVEAS)
		pf_save(path);
}

/* ---- frozen API -------------------------------------------------------- */

void pt_file_init(void)
{
	PICK.mode = PF_NONE;
	PICK.pick_drawn = 0;
	PICK.pick_x = PICK.pick_y = PICK.pick_w = PICK.pick_h = 0;
	PT.path[0] = 0;
	PT.label[0] = 0;
}

void pt_file_new(void)
{
	if (PT.canvas)
		memset(PT.canvas, 0, (unsigned)PT.width * (unsigned)PT.height);
	PT.path[0] = 0;
	PT.label[0] = 0;
	pt_undo_clear();
	pf_status("New canvas", 0);
	pt_request_redraw();
}

void pt_file_open(void)
{
	pf_begin(PF_OPEN);
}

void pt_file_save(void)
{
	if (PT.path[0])
		pf_save(PT.path);
	else
		pf_begin(PF_SAVEAS);
}

void pt_file_save_as(void)
{
	pf_begin(PF_SAVEAS);
}

int pt_file_dialog_active(void)
{
	return mmb_files_pick_active();
}

int pt_file_key(int key)
{
	if (!mmb_files_pick_active())
		return 0;

	mmb_files_pick_key(key);
	if (mmb_files_pick_done())
	{
		if (!mmb_files_pick_cancelled())
			pf_commit(mmb_files_pick_result());
		else
			pf_status("Cancelled", 0);
		mmb_files_pick_end();
		PICK.mode = PF_NONE;
		pt_request_redraw();
		return 1;
	}
	pt_request_redraw();
	return 1;
}

/* Resolve a lone Esc in the picker once its idle window elapses. The key
 * handler buffers Esc to tell it from an arrow key, so without this poll the
 * dialog would wait for another key (#760). */
int pt_file_poll(void)
{
	if (!mmb_files_pick_active())
		return 0;
	if (!mmb_files_pick_poll())
		return 0;
	if (mmb_files_pick_done() && mmb_files_pick_cancelled())
	{
		pf_status("Cancelled", 0);
		mmb_files_pick_end();
		PICK.mode = PF_NONE;
		pt_request_redraw();
	}
	return 1;
}

/* Compose the picker as part of PAINT's redraw. The dialog is drawn directly
 * by the picker module outside pt_redraw(), so without this the frame's canvas
 * pass paints over it and it vanishes after one frame (#722). Cell bookkeeping
 * mirrors paint_menus.c: invalidate the panel while it is open (a partial
 * canvas recomposite may have run underneath) and blank + accept the old
 * rectangle when it closes so the text cannot re-blit over the canvas. */
void pt_file_draw(void)
{
	int nx = 0, ny = 0, nw = 0, nh = 0;
	int active = mmb_files_pick_active();

	if (active)
		mmb_files_pick_geom(&nx, &ny, &nw, &nh);

	if (PICK.pick_drawn &&
	    (nx != PICK.pick_x || ny != PICK.pick_y || nw != PICK.pick_w ||
	     nh != PICK.pick_h))
	{
		tui_fill(PICK.pick_x, PICK.pick_y, PICK.pick_w, PICK.pick_h, ' ',
			 TUI_WHITE, TUI_BLACK);
		tui_accept_rect(PICK.pick_x, PICK.pick_y, PICK.pick_w, PICK.pick_h);
	}
	if (active)
	{
		tui_invalidate_rect(nx, ny, nw, nh);
		mmb_files_pick_compose();
	}

	PICK.pick_drawn = active ? 1 : 0;
	PICK.pick_x = nx;
	PICK.pick_y = ny;
	PICK.pick_w = nw;
	PICK.pick_h = nh;
}
