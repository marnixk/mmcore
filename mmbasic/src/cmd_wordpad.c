#include "mmb_priv.h"
#include "tui.h"

#define WP_BUF      65536
#define WP_CLIP     8192
#define WP_DLG      128
#define WP_WRAP_N   80
#define WP_WRAP_W   120
#define WP_MAX_VR   4096
#define WP_FD_MAX   64
#define WP_FD_NAME  40

#define WP_ESC_NONE 0
#define WP_ESC_GOT  1
#define WP_ESC_CSI  2
#define WP_ESC_SS3  3
#define WP_ESC_IDLE_MS 60

#define WP_DLG_NONE    0
#define WP_DLG_OPEN    1
#define WP_DLG_SAVEAS  2
#define WP_DLG_PICK    3
#define WP_DLG_RECOVER 4

#define WP_DOCS       8
/* Backing-store size for the recursive quick-open walk.  The ramdisk alone
 * carries well over 100 files, so a small cap would hide documents before the
 * sort even runs (issue #539). */
#define WP_PICK_MAX   160
#define WP_PICK_DEPTH 5

#define WP_BOX_V  0xB3
#define WP_BOX_H  0xC4
#define WP_BOX_TL 0xDA
#define WP_BOX_TR 0xBF
#define WP_BOX_BL 0xC0
#define WP_BOX_BR 0xD9

#define WP_MENU_FILE     0
#define WP_MENU_EDIT     1
#define WP_MENU_SETTINGS 2
#define WP_MENU_COUNT    3

#define WP_FD_FOCUS_NAME 0
#define WP_FD_FOCUS_FILE 1
#define WP_FD_FOCUS_DIR  2

#define WP_STYLE_NORM    0
#define WP_STYLE_H1      1
#define WP_STYLE_H2      2
#define WP_STYLE_H3      3
#define WP_STYLE_BULLET  4
#define WP_STYLE_QUOTE   5
#define WP_STYLE_CODE    6
#define WP_STYLE_ORDERED 7
#define WP_BULLET_CH     0x07u

/* Leading spaces per list nesting level, and the cap used when recognising a
 * list marker so a run of spaces cannot exhaust the buffer walk. */
#define WP_INDENT_UNIT   2
#define WP_INDENT_MAX    16

typedef struct {
	int off0;
	int off1;
	int len;
	int style;
	int line_start;
	int prefix_len;
	int indent_len;
	int hide_prefix;
	int scale;
} wp_vrow;

typedef struct {
	char buf[WP_BUF];
	int len;
	char path[128];
	int dirty;
	int cx;
	int sel;
	int sel_anchor;
	int scroll;
} wp_doc;

typedef struct {
	int active;
	char buf[WP_BUF];
	int len;
	char path[128];
	int dirty;
	int cx;
	int sel;
	int sel_anchor;
	char clip[WP_CLIP];
	int cliplen;
	int wide;
	int menu_open;
	int menu;
	int menu_item;
	int dialog;
	char dlg[WP_DLG];
	int dlglen;
	int scroll;
	int vid_cols;
	int vid_rows;
	int pane_left;
	int pane_width;
	int text_rows;
	int text_top;
	int esc_state;
	unsigned esc_at;
	int csi_n;
	int csi_arg;
	int csi_semi;
	int alt_pend;
	int cx_vrow;
	int cx_vcol;
	int total_vrows;
	int ndoc;
	int cur;
	int chrome_shown;
	int last_scroll;
	unsigned rec_at;
	unsigned rec_sig;
	/* Graphics mode captured on entry so a later WORDPAD session starts
	 * from a fully torn-down screen (issue #583). */
	int saved_mode;
	int saved_bits;
	wp_vrow vrows[WP_MAX_VR];
} wp_state;

static wp_state W_s[MMB_MAX_CONSOLES];
#define W (W_s[g_console])
static wp_doc docs[WP_DOCS];
/* The file-picker root and its transient list/selection state are per-console
 * session state (#670, #680): opening the picker on one console must not reuse
 * or rebuild the root/list another console's picker is using. */
static char pick_path_s[MMB_MAX_CONSOLES][WP_PICK_MAX][128];
static char pick_root_s[MMB_MAX_CONSOLES][128];
#define pick_root (pick_root_s[g_console])
static int pick_n_s[MMB_MAX_CONSOLES], pick_sel_s[MMB_MAX_CONSOLES],
	pick_row0_s[MMB_MAX_CONSOLES], pick_vn_s[MMB_MAX_CONSOLES];
static int pick_view_s[MMB_MAX_CONSOLES][WP_PICK_MAX];
#define pick_path (pick_path_s[g_console])
#define pick_n (pick_n_s[g_console])
#define pick_sel (pick_sel_s[g_console])
#define pick_row0 (pick_row0_s[g_console])
#define pick_vn (pick_vn_s[g_console])
#define pick_view (pick_view_s[g_console])

/* Crash-resume sidecar <path>.rec and its pending-prompt path. */
#define WP_REC_SUFFIX  ".rec"
#define WP_AUTOSAVE_MS 1500
static char wp_rec_sidecar[160];
static char wp_rec_tmp[WP_BUF];

static int wp_save(void);
static void wp_autosave(void);
static void wp_stash(void);

/* Ctrl+Z undo (#532). WORDPAD's buffer is a flat string, so undo keeps a
 * compact pool of pre-edit copies (buffer + length), truncated to the shared
 * MMB_UNDO_DEPTH steps. The pool is shared across documents: only one WORDPAD
 * is active at a time. */
#define WP_UNDO_POOL 196608
static unsigned char wp_undo_pool[WP_UNDO_POOL];
static int wp_undo_off[MMB_UNDO_DEPTH];
static int wp_undo_len[MMB_UNDO_DEPTH];
static int wp_undo_n, wp_undo_bytes;
static int wp_hist_active, wp_hist_taken;

static void wp_undo_reset(void)
{
	wp_undo_n = 0;
	wp_undo_bytes = 0;
}

static void wp_undo_push(void)
{
	int len = W.len;
	if (len < 0)
		len = 0;
	if (len > WP_BUF)
		len = WP_BUF;
	if (len > WP_UNDO_POOL)
		return;
	while (wp_undo_n >= MMB_UNDO_DEPTH || wp_undo_bytes + len > WP_UNDO_POOL)
	{
		int drop, i;
		if (wp_undo_n <= 0)
			return;
		drop = wp_undo_len[0];
		memmove(wp_undo_pool, wp_undo_pool + drop,
			(unsigned)(wp_undo_bytes - drop));
		for (i = 1; i < wp_undo_n; i++)
		{
			wp_undo_off[i - 1] = wp_undo_off[i] - drop;
			wp_undo_len[i - 1] = wp_undo_len[i];
		}
		wp_undo_bytes -= drop;
		wp_undo_n--;
	}
	memcpy(wp_undo_pool + wp_undo_bytes, W.buf, (unsigned)len);
	wp_undo_off[wp_undo_n] = wp_undo_bytes;
	wp_undo_len[wp_undo_n] = len;
	wp_undo_bytes += len;
	wp_undo_n++;
}

/* One snapshot per top-level keystroke, no matter how many primitives it
 * calls. Programmatic loads/recovery reset the history instead. */
static void wp_note_edit(void)
{
	if (!wp_hist_active || wp_hist_taken)
		return;
	wp_undo_push();
	wp_hist_taken = 1;
}

static void wp_undo(void)
{
	int off, len;
	if (wp_undo_n <= 0)
		return;
	wp_undo_n--;
	off = wp_undo_off[wp_undo_n];
	len = wp_undo_len[wp_undo_n];
	memcpy(W.buf, wp_undo_pool + off, (unsigned)len);
	W.len = len;
	wp_undo_bytes = off;
	if (W.cx > W.len)
		W.cx = W.len;
	if (W.sel_anchor > W.len)
		W.sel_anchor = W.len;
	W.sel = 0;
	W.dirty = 1;
}

static const char *menu_names[] = { "File", "Edit", "Settings" };
static const char menu_hots[] = { 'F', 'E', 'S' };
static int menu_x[WP_MENU_COUNT];

static const char *file_items[] = { "New", "Open...", "Save", "Save As...", "Quit" };
static const char *edit_items[] = { "Copy", "Cut", "Paste" };
static const char *settings_items[] = { "Wide view" };

static char fd_dir[128];
/* Not "fd_mask": glibc declares that as a typedef via <sys/select.h>. */
static char wp_fd_mask[32];
static char fd_files[WP_FD_MAX][WP_FD_NAME];
static char fd_dirs[WP_FD_MAX][WP_FD_NAME];
static int fd_nfile, fd_ndir;
static int fd_fsel, fd_dsel;
static int fd_ftop, fd_dtop;
static int fd_focus;

static const mmb_ed_theme *wpth(void)
{
	return mmb_editor_theme();
}

static unsigned wp_vga_rgb(int idx)
{
	static const unsigned pal[16] = {
		0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
		0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
		0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
		0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu
	};
	if (idx < 0)
		idx = 0;
	if (idx > 15)
		idx = 15;
	return pal[idx];
}

static unsigned wp_rgb(unsigned char idx)
{
	const unsigned *pal = mmb_editor_palette();
	int i = (int)idx & 15;

	if (!pal)
		return wp_vga_rgb(i);
	return pal[i] & 0xFFFFFFu;
}

#define WP_BG      wp_rgb(wpth()->edit_bg)
#define WP_FG      wp_rgb(wpth()->edit_fg)
#define WP_HEAD    wp_rgb(wpth()->num_fg)
#define WP_DIM     wp_rgb(wpth()->cmt_fg)
#define WP_SEL_FG  wp_rgb(wpth()->sel_fg)
#define WP_SEL_BG  wp_rgb(wpth()->sel_bg)
#define WP_MENU_FG wp_rgb(wpth()->menu_fg)
#define WP_HOT     wp_rgb(wpth()->hot)
#define WP_STR     wp_rgb(wpth()->str_fg)

static unsigned code_bg(void)
{
	unsigned r = (WP_BG >> 16) & 255;
	unsigned g = (WP_BG >> 8) & 255;
	unsigned b = WP_BG & 255;
	return mmb_rgb_pack((int)(r * 7 / 8), (int)(g * 7 / 8), (int)(b * 7 / 8));
}

static unsigned dlg_bg(void)
{
	int r = (int)((WP_BG >> 16) & 255);
	int g = (int)((WP_BG >> 8) & 255);
	int b = (int)(WP_BG & 255);
	int lum = (r * 3 + g * 6 + b) / 10;
	int d = 28;

	if (lum < 140)
	{
		r += d;
		g += d;
		b += d;
	}
	else
	{
		r -= d;
		g -= d;
		b -= d;
	}
	if (r < 0)
		r = 0;
	if (g < 0)
		g = 0;
	if (b < 0)
		b = 0;
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	return mmb_rgb_pack(r, g, b);
}

static unsigned intense_fg(void)
{
	int r = (int)((WP_FG >> 16) & 255);
	int g = (int)((WP_FG >> 8) & 255);
	int b = (int)(WP_FG & 255);
	int br = (int)((WP_BG >> 16) & 255);
	int bg = (int)((WP_BG >> 8) & 255);
	int bb = (int)(WP_BG & 255);
	int fg_lum = (r * 3 + g * 6 + b) / 10;
	int bg_lum = (br * 3 + bg * 6 + bb) / 10;

	if (fg_lum >= bg_lum)
	{
		r = r + (255 - r) * 2 / 5;
		g = g + (255 - g) * 2 / 5;
		b = b + (255 - b) * 2 / 5;
	}
	else
	{
		r = r * 3 / 5;
		g = g * 3 / 5;
		b = b * 3 / 5;
	}
	return mmb_rgb_pack(r, g, b);
}

static int wp_chrome(void)
{
	if (W.menu_open || W.dialog || W.alt_pend)
		return 1;
	if (G.plat && G.plat->alt_held && G.plat->alt_held())
		return 1;
	return 0;
}

static int heading_scale(int style)
{
	if (style == WP_STYLE_H1)
		return 4;
	if (style == WP_STYLE_H2)
		return 3;
	if (style == WP_STYLE_H3)
		return 2;
	return 1;
}

static int vrow_h(int vr)
{
	int s;
	if (vr < 0 || vr >= W.total_vrows)
		return 1;
	s = W.vrows[vr].scale;
	if (s < 1)
		return 1;
	if (s > 4)
		return 4;
	return s;
}

static int screen_row_of_vrow(int vr)
{
	int s = 0, i, n;

	n = vr;
	if (n > W.total_vrows)
		n = W.total_vrows;
	for (i = 0; i < n; i++)
		s += vrow_h(i);
	return s;
}

static const unsigned char *heading_tnr(int scale, unsigned ch, int *gw, int *gh, int *rowb)
{
	if (ch < 32 || ch > 126)
		ch = '?';
	ch -= 32;
	if (scale == 2)
	{
		*gw = 16;
		*gh = 32;
		*rowb = 2;
		return mmb_tnr_16x32 + ch * 32 * 2;
	}
	if (scale == 3)
	{
		*gw = 24;
		*gh = 48;
		*rowb = 3;
		return mmb_tnr_24x48 + ch * 48 * 3;
	}
	if (scale == 4)
	{
		*gw = 32;
		*gh = 64;
		*rowb = 4;
		return mmb_tnr_32x64 + ch * 64 * 4;
	}
	return 0;
}

static int heading_metrics(int scale, unsigned ch, int *left)
{
	const unsigned char *g;
	int gw, gh, rowb, x, y, L, R, ink, track;

	if (left)
		*left = 0;
	if (scale <= 1)
		return 8;
	if (ch == ' ' || ch == '\t')
		return (scale * 8) / 2;
	g = heading_tnr(scale, ch, &gw, &gh, &rowb);
	if (!g)
		return (scale * 8 * 4) / 5;
	L = gw;
	R = -1;
	for (y = 0; y < gh; y++)
	{
		for (x = 0; x < gw; x++)
		{
			unsigned char bits = g[y * rowb + x / 8];
			if (bits & (unsigned char)(0x80 >> (x % 8)))
			{
				if (x < L)
					L = x;
				if (x > R)
					R = x;
			}
		}
	}
	if (R < L)
		return (scale * 8) / 2;
	if (left)
		*left = L;
	ink = R - L + 1;
	track = 1 + scale / 2;
	return ink + track;
}

static void wp_fill_px(int x, int y, int w, int h, unsigned rgb)
{
	if (G.plat && G.plat->tui_fill_px)
		G.plat->tui_fill_px(x, y, w, h, rgb);
}

static void wp_glyph(int col, int row, unsigned ch, unsigned fg, unsigned bg, int scale)
{
	if (!G.plat)
		return;
	if (scale < 1)
		scale = 1;
	if (G.plat->tui_glyph_n)
	{
		G.plat->tui_glyph_n(col, row, ch, fg, bg, scale);
		return;
	}
	if (scale >= 2 && G.plat->tui_glyph2x)
		G.plat->tui_glyph2x(col, row, ch, fg, bg);
	else if (G.plat->tui_glyph)
		G.plat->tui_glyph(col, row, ch, fg, bg);
}

static void wp_glyph_px(int x_px, int y_px, unsigned ch, unsigned fg, unsigned bg,
			int scale)
{
	if (!G.plat)
		return;
	if (scale < 1)
		scale = 1;
	if (G.plat->tui_glyph_n_px)
	{
		G.plat->tui_glyph_n_px(x_px, y_px, ch, fg, bg, scale, scale > 1);
		return;
	}
	if (G.plat->tui_glyph_n)
		G.plat->tui_glyph_n(x_px / 8, y_px / 16, ch, fg, bg, scale);
	else if (scale >= 2 && G.plat->tui_glyph2x)
		G.plat->tui_glyph2x(x_px / 8, y_px / 16, ch, fg, bg);
	else if (G.plat->tui_glyph)
		G.plat->tui_glyph(x_px / 8, y_px / 16, ch, fg, bg);
}

static void wp_box(int c0, int r0, int w, int h, unsigned fg, unsigned bg)
{
	int c, r;

	if (w < 2 || h < 2 || !G.plat || !G.plat->tui_glyph)
		return;
	G.plat->tui_glyph(c0, r0, WP_BOX_TL, fg, bg);
	G.plat->tui_glyph(c0 + w - 1, r0, WP_BOX_TR, fg, bg);
	G.plat->tui_glyph(c0, r0 + h - 1, WP_BOX_BL, fg, bg);
	G.plat->tui_glyph(c0 + w - 1, r0 + h - 1, WP_BOX_BR, fg, bg);
	for (c = 1; c < w - 1; c++)
	{
		G.plat->tui_glyph(c0 + c, r0, WP_BOX_H, fg, bg);
		G.plat->tui_glyph(c0 + c, r0 + h - 1, WP_BOX_H, fg, bg);
	}
	for (r = 1; r < h - 1; r++)
	{
		G.plat->tui_glyph(c0, r0 + r, WP_BOX_V, fg, bg);
		G.plat->tui_glyph(c0 + w - 1, r0 + r, WP_BOX_V, fg, bg);
	}
}

static void wp_stash(void)
{
	wp_doc *d;

	if (W.cur < 0 || W.cur >= WP_DOCS)
		return;
	d = &docs[W.cur];
	if (W.len < 0)
		W.len = 0;
	if (W.len >= WP_BUF)
		W.len = WP_BUF - 1;
	memcpy(d->buf, W.buf, (unsigned)W.len);
	d->buf[W.len] = 0;
	d->len = W.len;
	strncpy(d->path, W.path, sizeof(d->path) - 1);
	d->path[sizeof(d->path) - 1] = 0;
	d->dirty = W.dirty;
	d->cx = W.cx;
	d->sel = W.sel;
	d->sel_anchor = W.sel_anchor;
	d->scroll = W.scroll;
}

static void wp_restore(void)
{
	wp_doc *d;

	if (W.cur < 0)
		W.cur = 0;
	if (W.cur >= W.ndoc)
		W.cur = W.ndoc > 0 ? W.ndoc - 1 : 0;
	d = &docs[W.cur];
	W.len = d->len;
	if (W.len < 0)
		W.len = 0;
	if (W.len >= WP_BUF)
		W.len = WP_BUF - 1;
	memcpy(W.buf, d->buf, (unsigned)W.len);
	W.buf[W.len] = 0;
	strncpy(W.path, d->path, sizeof(W.path) - 1);
	W.path[sizeof(W.path) - 1] = 0;
	W.dirty = d->dirty;
	W.cx = d->cx;
	W.sel = d->sel;
	W.sel_anchor = d->sel_anchor;
	W.scroll = d->scroll;
}

static int wp_path_eq(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";
	while (*a && *b)
	{
		char ca = *a++, cb = *b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (char)(cb - 32);
		if (ca != cb)
			return 0;
	}
	return *a == 0 && *b == 0;
}

static void ser(const char *s)
{
	unsigned n;
	if (!s || !G.plat || !G.plat->write_serial)
		return;
	n = (unsigned)strlen(s);
	if (n)
		G.plat->write_serial(s, n);
}

static const char *wp_basename(void)
{
	const char *s = W.path;
	const char *p = s;

	if (!s[0])
		return "untitled";
	while (*p)
	{
		if (*p == '/' || *p == ':')
			s = p + 1;
		p++;
	}
	return s[0] ? s : "untitled";
}

static void wp_puts(int col, int row, const char *s, unsigned fg, unsigned bg)
{
	if (!G.plat || !G.plat->tui_glyph || !s)
		return;
	while (*s)
		G.plat->tui_glyph(col++, row, (unsigned)*s++, fg, bg);
}

static void wp_fill_row(int row, unsigned fg, unsigned bg)
{
	int c;
	if (!G.plat || !G.plat->tui_glyph)
		return;
	for (c = 0; c < W.vid_cols; c++)
		G.plat->tui_glyph(c, row, ' ', fg, bg);
}

static void wp_layout_geom(void)
{
	int wrap;

	W.vid_cols = G.plat && G.plat->video_cols ? G.plat->video_cols() : 80;
	W.vid_rows = G.plat && G.plat->video_rows ? G.plat->video_rows() : 25;
	wrap = W.wide ? WP_WRAP_W : WP_WRAP_N;
	if (W.vid_cols < wrap)
	{
		W.pane_width = W.vid_cols;
		W.pane_left = 0;
	}
	else
	{
		W.pane_width = wrap;
		W.pane_left = (W.vid_cols - wrap) / 2;
		if (W.pane_left < 0)
			W.pane_left = 0;
	}
	W.text_top = wp_chrome() ? 1 : 0;
	W.text_rows = W.vid_rows - (wp_chrome() ? 2 : 0);
	if (W.text_rows < 1)
		W.text_rows = 1;
}

static void ensure_md(char *path, int sz)
{
	if (!path || !path[0])
		return;
	if (!strchr(path, '.'))
		strncat(path, ".MD", (unsigned)sz - strlen(path) - 1);
}

static void canon_path(const char *path, char *out, int outsz)
{
	char tmp[128];

	out[0] = 0;
	if (!path || !path[0] || outsz < 2)
		return;
	if (mmb_vfs_resolve(path, tmp, sizeof(tmp)) == 0)
		strncpy(out, tmp, (unsigned)outsz - 1);
	else
		strncpy(out, path, (unsigned)outsz - 1);
	out[outsz - 1] = 0;
	ensure_md(out, outsz);
}

static int line_is_fence(int ls, int le)
{
	return (le - ls) == 3 && W.buf[ls] == '`' && W.buf[ls + 1] == '`' &&
	       W.buf[ls + 2] == '`';
}

/* True when the line starting at target sits inside a ``` fence and is not a
 * fence line itself.  Scans from the top so the key handler does not depend on
 * the last-drawn layout. */
static int line_in_code(int target)
{
	int pos = 0, in_code = 0;

	while (pos <= W.len)
	{
		int ls = pos, le = pos;

		while (le < W.len && W.buf[le] != '\n')
			le++;
		if (ls == target)
			return in_code && !line_is_fence(ls, le);
		if (line_is_fence(ls, le))
			in_code = !in_code;
		pos = le;
		if (pos < W.len && W.buf[pos] == '\n')
			pos++;
		else
			break;
	}
	return 0;
}

/* Recognise a list marker after optional leading spaces.  Returns 0 (none),
 * 1 (bullet) or 2 (ordered), and fills the indent width, marker width and
 * ordered number.  A bullet keeps its GFM family; an ordered marker is
 * "<digits>. ". */
static int list_marker(int ls, int le, int *indent, int *mlen, int *num)
{
	int i = ls, ind = 0;
	int d, v;

	*indent = 0;
	*mlen = 0;
	if (num)
		*num = 0;
	while (i < le && W.buf[i] == ' ' && ind < WP_INDENT_MAX)
	{
		i++;
		ind++;
	}
	if (i + 1 < le && (W.buf[i] == '-' || W.buf[i] == '*' ||
			   W.buf[i] == '+') && W.buf[i + 1] == ' ')
	{
		*indent = ind;
		*mlen = 2;
		return 1;
	}
	d = i;
	v = 0;
	while (d < le && W.buf[d] >= '0' && W.buf[d] <= '9' && d - i < 4)
	{
		v = v * 10 + (W.buf[d] - '0');
		d++;
	}
	if (d > i && d + 1 < le && W.buf[d] == '.' && W.buf[d + 1] == ' ')
	{
		*indent = ind;
		*mlen = (d - i) + 2;
		if (num)
			*num = v;
		return 2;
	}
	return 0;
}

static void line_style(int ls, int le, int in_code, int cursor_on, int *style,
		       int *indent_len, int *prefix_len, int *hide_prefix)
{
	int ind, mlen, num, kind;

	*style = WP_STYLE_NORM;
	*indent_len = 0;
	*prefix_len = 0;
	*hide_prefix = 0;
	if (in_code)
	{
		*style = WP_STYLE_CODE;
		return;
	}
	kind = list_marker(ls, le, &ind, &mlen, &num);
	if (kind == 1)
	{
		*style = WP_STYLE_BULLET;
		*indent_len = ind;
		*prefix_len = ind + mlen;
	}
	else if (kind == 2)
	{
		*style = WP_STYLE_ORDERED;
		*indent_len = ind;
		*prefix_len = ind + mlen;
	}
	else if (le > ls + 3 && W.buf[ls] == '#' && W.buf[ls + 1] == '#' &&
		 W.buf[ls + 2] == '#' && W.buf[ls + 3] == ' ')
	{
		*style = WP_STYLE_H3;
		*prefix_len = 4;
	}
	else if (le > ls + 2 && W.buf[ls] == '#' && W.buf[ls + 1] == '#' &&
		 W.buf[ls + 2] == ' ')
	{
		*style = WP_STYLE_H2;
		*prefix_len = 3;
	}
	else if (le > ls + 1 && W.buf[ls] == '#' && W.buf[ls + 1] == ' ')
	{
		*style = WP_STYLE_H1;
		*prefix_len = 2;
	}
	else if (le > ls + 1 && W.buf[ls] == '>' && W.buf[ls + 1] == ' ')
	{
		*style = WP_STYLE_QUOTE;
		*prefix_len = 2;
	}
	/* Ordered markers stay visible (their number is the point); bullet and
	 * heading markers collapse to a bullet glyph / nothing when the caret is
	 * elsewhere.  Indentation always stays visible. */
	if (*prefix_len > 0 && !cursor_on && *style != WP_STYLE_ORDERED)
		*hide_prefix = 1;
}

static int vis_ch_at(int style, int hide_prefix, int i, int ls, int indent_len,
		     int prefix_len, unsigned *ch)
{
	unsigned c = (unsigned char)W.buf[i];

	if (hide_prefix && i >= ls + indent_len && i < ls + prefix_len)
	{
		if (style == WP_STYLE_BULLET)
		{
			if (i == ls + indent_len)
				c = WP_BULLET_CH;
			/* the marker's trailing space stays visible */
		}
		else
			return 0;
	}
	*ch = c;
	return 1;
}

static int emph_mark_len(int i, int lim)
{
	if (i < 0 || i + 1 >= lim)
		return 0;
	if ((W.buf[i] == '*' && W.buf[i + 1] == '*') ||
	    (W.buf[i] == '_' && W.buf[i + 1] == '_'))
		return 2;
	return 0;
}

static int emph_at(int pos, int ls, int le, int style)
{
	int i, on = 0;

	if (style == WP_STYLE_CODE)
		return 0;
	for (i = ls; i < pos && i < le; )
	{
		int n = emph_mark_len(i, le);
		if (n)
		{
			on = !on;
			i += n;
		}
		else
			i++;
	}
	return on;
}

static int skip_emph_mark(int i, int le, int style)
{
	int n;

	if (style == WP_STYLE_CODE)
		return 0;
	n = emph_mark_len(i, le);
	if (!n)
		return 0;
	if (W.cx >= i && W.cx < i + n)
		return 0;
	return n;
}

static int in_sel(int off)
{
	int lo, hi, a, b;

	if (!W.sel)
		return 0;
	a = W.sel_anchor;
	b = W.cx;
	if (a > b)
	{
		int x = a;
		a = b;
		b = x;
	}
	if (a < 0)
		a = 0;
	if (b > W.len)
		b = W.len;
	if (a >= b)
		return 0;
	lo = a;
	hi = b;
	return off >= lo && off < hi;
}

static void wp_build_layout(void)
{
	int pos = 0;
	int vr = 0;
	int in_code = 0;
	int found_cx = 0;

	W.total_vrows = 0;
	W.cx_vrow = 0;
	W.cx_vcol = 0;
	while (pos <= W.len && vr < WP_MAX_VR)
	{
		int scale, px, max_px, adv;
		int ls = pos;
		int le = pos;
		int style, prefix_len, indent_len, hide_prefix;
		int cursor_on;
		int i, col;
		int break_at, break_col;

		while (le < W.len && W.buf[le] != '\n')
			le++;
		if (line_is_fence(ls, le))
			in_code = !in_code;
		cursor_on = (W.cx >= ls && W.cx <= le);
		line_style(ls, le, in_code && !line_is_fence(ls, le), cursor_on,
			   &style, &indent_len, &prefix_len, &hide_prefix);
		scale = heading_scale(style);
		max_px = W.pane_width * 8;
		if (max_px < 8)
			max_px = 8;
		i = ls;
		col = 0;
		px = 0;
		break_at = -1;
		break_col = 0;
		W.vrows[vr].off0 = i;
		W.vrows[vr].style = style;
		W.vrows[vr].line_start = ls;
		W.vrows[vr].prefix_len = prefix_len;
		W.vrows[vr].indent_len = indent_len;
		W.vrows[vr].hide_prefix = hide_prefix;
		W.vrows[vr].scale = scale;
		while (i < le)
		{
			unsigned vis;
			int skip;

			if (!vis_ch_at(style, hide_prefix, i, ls, indent_len,
				       prefix_len, &vis))
			{
				if (W.cx == i)
				{
					W.cx_vrow = vr;
					W.cx_vcol = col;
					found_cx = 1;
				}
				i++;
				continue;
			}
			skip = skip_emph_mark(i, le, style);
			if (skip)
			{
				i += skip;
				continue;
			}
			if (col > 0)
			{
				adv = heading_metrics(scale, vis, 0);
				if (px + adv > max_px)
				{
				if (break_at >= ls)
				{
					W.vrows[vr].off1 = break_at + 1;
					W.vrows[vr].len = break_col + 1;
					vr++;
					i = break_at + 1;
					col = 0;
					px = 0;
					break_at = -1;
					break_col = 0;
					if (vr >= WP_MAX_VR)
						break;
					W.vrows[vr].off0 = i;
					W.vrows[vr].style = style;
					W.vrows[vr].line_start = ls;
					W.vrows[vr].prefix_len = prefix_len;
					W.vrows[vr].indent_len = indent_len;
					W.vrows[vr].hide_prefix = hide_prefix;
					W.vrows[vr].scale = scale;
					continue;
				}
				W.vrows[vr].off1 = i;
				W.vrows[vr].len = col;
				vr++;
				col = 0;
				px = 0;
				break_at = -1;
				break_col = 0;
				if (vr >= WP_MAX_VR)
					break;
				W.vrows[vr].off0 = i;
				W.vrows[vr].style = style;
				W.vrows[vr].line_start = ls;
				W.vrows[vr].prefix_len = prefix_len;
				W.vrows[vr].indent_len = indent_len;
				W.vrows[vr].hide_prefix = hide_prefix;
				W.vrows[vr].scale = scale;
				continue;
				}
			}
			if (vis == ' ')
			{
				break_at = i;
				break_col = col;
			}
			if (W.cx == i)
			{
				W.cx_vrow = vr;
				W.cx_vcol = col;
				found_cx = 1;
			}
			px += heading_metrics(scale, vis, 0);
			col++;
			i++;
		}
		if (vr >= WP_MAX_VR)
			break;
		W.vrows[vr].off1 = le;
		W.vrows[vr].len = col;
		vr++;
		pos = le;
		if (pos < W.len && W.buf[pos] == '\n')
		{
			if (W.cx == pos && vr > 0)
			{
				W.cx_vrow = vr - 1;
				W.cx_vcol = W.vrows[vr - 1].len;
				found_cx = 1;
			}
			pos++;
		}
		else
			break;
	}
	W.total_vrows = vr;
	if (!found_cx && W.cx == W.len && W.total_vrows > 0)
	{
		W.cx_vrow = W.total_vrows - 1;
		W.cx_vcol = W.vrows[W.total_vrows - 1].len;
	}
}

static int vrow_col_to_off(int vr, int vc)
{
	int ls, i, col, le;
	int prefix_len, indent_len, hide_prefix;

	if (vr < 0 || vr >= W.total_vrows)
		return W.len;
	ls = W.vrows[vr].line_start;
	prefix_len = W.vrows[vr].prefix_len;
	indent_len = W.vrows[vr].indent_len;
	hide_prefix = W.vrows[vr].hide_prefix;
	le = ls;
	while (le < W.len && W.buf[le] != '\n')
		le++;
	i = W.vrows[vr].off0;
	col = 0;
	while (i < W.vrows[vr].off1)
	{
		int skip;
		unsigned vis;

		if (!vis_ch_at(W.vrows[vr].style, hide_prefix, i, ls, indent_len,
			       prefix_len, &vis))
		{
			i++;
			continue;
		}
		(void)vis;
		skip = skip_emph_mark(i, le, W.vrows[vr].style);
		if (skip)
		{
			i += skip;
			continue;
		}
		if (col >= vc)
			return i;
		col++;
		i++;
	}
	return W.vrows[vr].off1;
}

static void ensure_scroll(void)
{
	int cy, ch, total;

	cy = screen_row_of_vrow(W.cx_vrow);
	ch = vrow_h(W.cx_vrow);
	if (W.cx_vrow >= W.total_vrows)
	{
		cy = screen_row_of_vrow(W.total_vrows);
		ch = 1;
	}
	if (cy < W.scroll)
		W.scroll = cy;
	if (cy + ch > W.scroll + W.text_rows)
		W.scroll = cy + ch - W.text_rows;
	if (W.scroll < 0)
		W.scroll = 0;
	total = screen_row_of_vrow(W.total_vrows);
	if (total > 0 && W.scroll > total - 1)
		W.scroll = total - 1;
}

static unsigned style_fg(int style)
{
	switch (style)
	{
	case WP_STYLE_H1:
	case WP_STYLE_H2:
	case WP_STYLE_H3:
	case WP_STYLE_BULLET:
	case WP_STYLE_ORDERED:
		return WP_HEAD;
	case WP_STYLE_QUOTE:
		return WP_DIM;
	default:
		return WP_FG;
	}
}

static unsigned style_bg(int style)
{
	if (style == WP_STYLE_CODE)
		return code_bg();
	return WP_BG;
}

static int count_words(void)
{
	int n = 0, in_word = 0, i;

	for (i = 0; i < W.len; i++)
	{
		char c = W.buf[i];
		if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
			in_word = 0;
		else if (!in_word)
		{
			in_word = 1;
			n++;
		}
	}
	return n;
}

static char *put_uint(char *p, int n)
{
	char tmp[12];
	int i = 0;

	if (n <= 0)
	{
		*p++ = '0';
		return p;
	}
	while (n > 0 && i < 11)
	{
		tmp[i++] = (char)('0' + (n % 10));
		n /= 10;
	}
	while (i--)
		*p++ = tmp[i];
	return p;
}

static void serial_row(const char *s)
{
	if (s)
		ser(s);
	ser("\r\n");
}

static void wp_serial_dump(void)
{
}

/* Drop every module buffer that outlives a single WORDPAD session.  Called on
 * entry and exit so leaving and reopening with a file argument starts from the
 * same clean state as a first launch (issue #583). */
static void wp_reset_globals(void)
{
	memset(docs, 0, sizeof(docs));
	memset(pick_path, 0, sizeof(pick_path));
	pick_root[0] = 0;
	memset(pick_view, 0, sizeof(pick_view));
	pick_n = pick_sel = pick_row0 = pick_vn = 0;
	fd_dir[0] = 0;
	memset(wp_fd_mask, 0, sizeof(wp_fd_mask));
	memset(fd_files, 0, sizeof(fd_files));
	memset(fd_dirs, 0, sizeof(fd_dirs));
	fd_nfile = fd_ndir = fd_fsel = fd_dsel = 0;
	fd_ftop = fd_dtop = fd_focus = 0;
	memset(menu_x, 0, sizeof(menu_x));
	wp_rec_sidecar[0] = 0;
	wp_undo_reset();
}

/* Restore the graphics mode the caller was in before WORDPAD took over and
 * clear page-1 overlay / sprite state an earlier program may have left armed,
 * then repaint the console (mirrors the AFK/EDITOR teardown, issue #583). */
static void wp_restore_gfx(void)
{
	if (W.saved_mode != G.gfx.mode || W.saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(W.saved_mode, W.saved_bits);
	mmb_gfx_reset_console(0);
	mmb_gfx_cls(G.gfx.bg);
}

static void wp_leave(void)
{
	wp_autosave();
	tui_end();
	wp_restore_gfx();
	G.home_prompt = 1;
	memset(&W, 0, sizeof(W));
	wp_reset_globals();
}

static void sel_clear(void)
{
	W.sel = 0;
}

static int sel_bounds(int *lo, int *hi)
{
	int a, b;

	if (!W.sel)
		return 0;
	a = W.sel_anchor;
	b = W.cx;
	if (a > b)
	{
		int x = a;
		a = b;
		b = x;
	}
	if (a < 0)
		a = 0;
	if (b > W.len)
		b = W.len;
	if (a >= b)
		return 0;
	if (lo)
		*lo = a;
	if (hi)
		*hi = b;
	return 1;
}

static void sel_prepare(int shift)
{
	if (!shift)
	{
		W.sel = 0;
		return;
	}
	if (!W.sel)
	{
		W.sel_anchor = W.cx;
		W.sel = 1;
	}
}

static int line_start(int off);
static int line_end(int off);
static void insert_char(char c);

static void clip_store(const char *s, int n)
{
	if (n >= WP_CLIP)
		n = WP_CLIP - 1;
	if (n < 0)
		n = 0;
	memcpy(W.clip, s, (unsigned)n);
	W.cliplen = n;
	W.clip[W.cliplen] = 0;
	/* Mirror the copy to the host clipboard when one exists (#525). */
	if (W.cliplen > 0)
		mmb_clipboard_setn(W.clip, (unsigned)W.cliplen);
}

static int delete_range(int lo, int hi, int to_clip)
{
	int n;

	if (hi <= lo)
		return 0;
	wp_note_edit();
	n = hi - lo;
	if (to_clip)
		clip_store(W.buf + lo, n);
	memmove(W.buf + lo, W.buf + hi, (unsigned)(W.len - hi + 1));
	W.len -= n;
	W.cx = lo;
	W.sel = 0;
	W.dirty = 1;
	return 1;
}

static int delete_selection(int to_clip)
{
	int lo, hi;

	if (!sel_bounds(&lo, &hi))
	{
		sel_clear();
		return 0;
	}
	return delete_range(lo, hi, to_clip);
}

static void copy_selection(void)
{
	int lo, hi;

	if (!sel_bounds(&lo, &hi))
		return;
	clip_store(W.buf + lo, hi - lo);
}

static void cut_line(void)
{
	int a, b;

	a = line_start(W.cx);
	b = line_end(W.cx);
	if (b < W.len && W.buf[b] == '\n')
		b++;
	if (b > a)
		delete_range(a, b, 1);
}

static void cut_selection(void)
{
	if (!delete_selection(1))
		cut_line();
}

static void paste_clip(void)
{
	int n;

	if (W.cliplen <= 0)
		return;
	wp_note_edit();
	delete_selection(0);
	n = W.cliplen;
	if (W.len + n >= WP_BUF - 1)
		n = WP_BUF - 1 - W.len;
	if (n <= 0)
		return;
	if (W.cx < W.len)
		memmove(W.buf + W.cx + n, W.buf + W.cx, (unsigned)(W.len - W.cx + 1));
	memcpy(W.buf + W.cx, W.clip, (unsigned)n);
	W.cx += n;
	W.len += n;
	W.buf[W.len] = 0;
	W.len = mmb_normalize_newlines(W.buf, W.len);
	if (W.cx > W.len)
		W.cx = W.len;
	W.dirty = 1;
}

static int line_start(int off)
{
	while (off > 0 && W.buf[off - 1] != '\n')
		off--;
	return off;
}

static int line_end(int off)
{
	while (off < W.len && W.buf[off] != '\n')
		off++;
	return off;
}

/* Shift the caret's line one nesting level right (dir > 0) or left.  On a
 * list line this changes its depth; on a plain line it is a plain indent. */
static void indent_line(int dir)
{
	int ls = line_start(W.cx);
	int le = line_end(W.cx);
	int n, i;

	if (dir > 0)
	{
		if (W.len + WP_INDENT_UNIT >= WP_BUF - 1)
			return;
		wp_note_edit();
		memmove(W.buf + ls + WP_INDENT_UNIT, W.buf + ls,
			(unsigned)(W.len - ls + 1));
		for (i = 0; i < WP_INDENT_UNIT; i++)
			W.buf[ls + i] = ' ';
		W.len += WP_INDENT_UNIT;
		if (W.cx >= ls)
			W.cx += WP_INDENT_UNIT;
		W.dirty = 1;
		return;
	}
	n = 0;
	while (n < WP_INDENT_UNIT && ls + n < le && W.buf[ls + n] == ' ')
		n++;
	if (n == 0)
		return;
	wp_note_edit();
	memmove(W.buf + ls, W.buf + ls + n, (unsigned)(W.len - (ls + n) + 1));
	W.len -= n;
	if (W.cx > ls + n)
		W.cx -= n;
	else if (W.cx > ls)
		W.cx = ls;
	W.dirty = 1;
}

/* Enter inside a fenced code block: carry the current line's leading
 * whitespace onto the new line (typical editor auto-indent, issue #582). */
static void insert_newline_code(void)
{
	int ls = line_start(W.cx);
	int le = line_end(W.cx);
	char ind[WP_INDENT_MAX + 1];
	int n = 0, i;

	for (i = ls; i < le && n < WP_INDENT_MAX; i++)
	{
		if (W.buf[i] != ' ' && W.buf[i] != '\t')
			break;
		ind[n++] = W.buf[i];
	}
	insert_char('\n');
	for (i = 0; i < n; i++)
		insert_char(ind[i]);
}

/* Enter inside a list: continue the marker on the new line, or terminate /
 * outdent when the current item is empty. */
static void insert_newline_list(void)
{
	int ls = line_start(W.cx);
	int le = line_end(W.cx);
	int ind, mlen, num, kind;
	int content, i, empty;

	/* Code content is literal: never treat it as a list, and keep its
	 * indentation on the next line. */
	if (line_in_code(ls))
	{
		insert_newline_code();
		return;
	}
	kind = list_marker(ls, le, &ind, &mlen, &num);
	if (!kind)
	{
		insert_char('\n');
		return;
	}
	content = ls + ind + mlen;
	if (W.cx < content)
	{
		insert_char('\n');
		return;
	}
	empty = 1;
	for (i = content; i < le; i++)
	{
		if (W.buf[i] != ' ' && W.buf[i] != '\t')
		{
			empty = 0;
			break;
		}
	}
	if (empty)
	{
		if (ind > 0)
			indent_line(-1);
		else
			delete_range(ls, content, 0);
		return;
	}
	insert_char('\n');
	for (i = 0; i < ind; i++)
		insert_char(' ');
	if (kind == 1)
	{
		insert_char(W.buf[ls + ind]);
		insert_char(' ');
		return;
	}
	{
		char dig[12];
		int n = num + 1, d = 0;

		if (n < 1)
			n = 1;
		while (n > 0 && d < 11)
		{
			dig[d++] = (char)('0' + (n % 10));
			n /= 10;
		}
		while (d--)
			insert_char(dig[d]);
		insert_char('.');
		insert_char(' ');
	}
}

static void insert_char(char c)
{
	if (c == '\r')
		return;
	if (W.len >= WP_BUF - 1)
		return;
	wp_note_edit();
	delete_selection(0);
	if (W.len >= WP_BUF - 1)
		return;
	if (W.cx < W.len)
		memmove(W.buf + W.cx + 1, W.buf + W.cx, (unsigned)(W.len - W.cx));
	W.buf[W.cx++] = c;
	W.len++;
	W.buf[W.len] = 0;
	W.dirty = 1;
}

static void backspace(void)
{
	if (delete_selection(0))
		return;
	if (W.cx <= 0)
		return;
	wp_note_edit();
	memmove(W.buf + W.cx - 1, W.buf + W.cx, (unsigned)(W.len - W.cx + 1));
	W.cx--;
	W.len--;
	W.dirty = 1;
}

static void delete_char(void)
{
	if (delete_selection(0))
		return;
	if (W.cx >= W.len)
		return;
	wp_note_edit();
	memmove(W.buf + W.cx, W.buf + W.cx + 1, (unsigned)(W.len - W.cx));
	W.len--;
	W.dirty = 1;
}

static void move_left(void)
{
	if (W.cx > 0)
		W.cx--;
}

static void move_right(void)
{
	if (W.cx < W.len)
		W.cx++;
}

static void move_up(void)
{
	int vr, vc;

	wp_build_layout();
	vr = W.cx_vrow - 1;
	vc = W.cx_vcol;
	if (vr < 0)
		return;
	W.cx = vrow_col_to_off(vr, vc);
}

static void move_down(void)
{
	int vr, vc;

	wp_build_layout();
	vr = W.cx_vrow + 1;
	vc = W.cx_vcol;
	if (vr >= W.total_vrows && W.cx < W.len)
	{
		W.cx = W.len;
		return;
	}
	if (vr >= W.total_vrows)
		return;
	W.cx = vrow_col_to_off(vr, vc);
}

static void move_home(void)
{
	wp_build_layout();
	W.cx = W.vrows[W.cx_vrow].off0;
}

static void move_end(void)
{
	wp_build_layout();
	W.cx = W.vrows[W.cx_vrow].off1;
}

static void page_up(void)
{
	int vr, vc;

	wp_build_layout();
	vr = W.cx_vrow - W.text_rows;
	vc = W.cx_vcol;
	if (vr < 0)
		vr = 0;
	W.cx = vrow_col_to_off(vr, vc);
}

static void page_down(void)
{
	int vr, vc;

	wp_build_layout();
	vr = W.cx_vrow + W.text_rows;
	vc = W.cx_vcol;
	if (vr >= W.total_vrows)
		vr = W.total_vrows > 0 ? W.total_vrows - 1 : 0;
	W.cx = vrow_col_to_off(vr, vc);
}

static void wp_load_file(const char *path)
{
	char canon[128];
	unsigned got;

	canon_path(path, canon, sizeof(canon));
	wp_undo_reset();
	W.buf[0] = 0;
	W.len = 0;
	if (mmb_vfs_read(canon, W.buf, sizeof(W.buf) - 1, &got) == 0)
	{
		W.len = mmb_normalize_newlines(W.buf, (int)got);
		W.buf[W.len] = 0;
	}
	strncpy(W.path, canon, sizeof(W.path) - 1);
	W.path[sizeof(W.path) - 1] = 0;
	W.dirty = 0;
	W.cx = 0;
	W.scroll = 0;
	sel_clear();
}

static void wp_rec_path(const char *path, char *out, int outsz)
{
	int n;

	out[0] = 0;
	if (!path || !path[0] || outsz < 2)
		return;
	strncpy(out, path, (unsigned)outsz - 1);
	out[outsz - 1] = 0;
	n = (int)strlen(out);
	strncat(out, WP_REC_SUFFIX, (unsigned)outsz - (unsigned)n - 1);
}

static void wp_rec_kill(const char *path)
{
	char rec[160];

	wp_rec_path(path, rec, sizeof(rec));
	if (rec[0])
		mmb_vfs_kill(rec);
}

static unsigned wp_sig(void)
{
	unsigned h = 2166136261u;
	int i;

	for (i = 0; i < W.len; i++)
	{
		h ^= (unsigned char)W.buf[i];
		h *= 16777619u;
	}
	h ^= (unsigned)W.len;
	return h;
}

static int wp_write_file(void)
{
	if (!W.path[0])
		return 0;
	if (mmb_vfs_write(W.path, W.buf, (unsigned)W.len, 0) != 0)
		return -1;
	W.dirty = 0;
	return 1;
}

/* Explicit save also drops the recovery sidecar: the file is now current. */
static int wp_save(void)
{
	int r = wp_write_file();

	if (r == 1)
		wp_rec_kill(W.path);
	return r;
}

/* Leave-time autosave writes the real file but leaves the sidecar for a
 * possible recovery prompt; it is dropped on the next launch once the file
 * and sidecar agree. */
static void wp_autosave(void)
{
	if (W.path[0] && W.dirty)
		wp_write_file();
}

static void wp_recovery_apply(void)
{
	unsigned got = 0;

	if (W.path[0] &&
	    mmb_vfs_read(wp_rec_sidecar, W.buf, sizeof(W.buf) - 1, &got) == 0)
	{
		wp_undo_reset();
		W.len = mmb_normalize_newlines(W.buf, (int)got);
		W.buf[W.len] = 0;
		W.cx = 0;
		W.scroll = 0;
		W.dirty = 1;
		sel_clear();
	}
}

static void wp_recovery_discard(void)
{
	if (wp_rec_sidecar[0])
		mmb_vfs_kill(wp_rec_sidecar);
	wp_rec_sidecar[0] = 0;
}

/* After loading a named file, offer to restore a surviving, different sidecar
 * rather than silently replacing the buffer (or the file). */
static void wp_recovery_check(void)
{
	char rec[160];
	unsigned got = 0;

	wp_rec_sidecar[0] = 0;
	if (!W.path[0])
		return;
	wp_rec_path(W.path, rec, sizeof(rec));
	if (!mmb_vfs_exists(rec))
		return;
	if (mmb_vfs_read(rec, wp_rec_tmp, sizeof(wp_rec_tmp) - 1, &got) == 0)
	{
		if ((int)got == W.len && memcmp(wp_rec_tmp, W.buf, (unsigned)W.len) == 0)
		{
			mmb_vfs_kill(rec);
			return;
		}
	}
	strncpy(wp_rec_sidecar, rec, sizeof(wp_rec_sidecar) - 1);
	wp_rec_sidecar[sizeof(wp_rec_sidecar) - 1] = 0;
	W.dialog = WP_DLG_RECOVER;
	W.rec_at = mmb_now_ms();
}

static void wp_rec_write(void)
{
	char rec[160];

	if (!W.path[0] || !W.dirty)
		return;
	wp_rec_path(W.path, rec, sizeof(rec));
	mmb_vfs_write(rec, W.buf, (unsigned)W.len, 0);
}

static int wp_open_path(const char *path)
{
	char full[128];
	int i;

	wp_autosave();
	wp_stash();
	canon_path(path, full, sizeof(full));
	for (i = 0; i < W.ndoc; i++)
	{
		if (full[0] && wp_path_eq(docs[i].path, full))
		{
			W.cur = i;
			wp_restore();
			return 1;
		}
	}
	if (W.ndoc < WP_DOCS)
		W.cur = W.ndoc++;
	wp_load_file(full);
	wp_stash();
	return 1;
}

static void wp_new_doc(void)
{
	wp_autosave();
	wp_stash();
	if (!W.path[0] && W.len == 0)
	{
		sel_clear();
		return;
	}
	if (W.ndoc < WP_DOCS)
		W.cur = W.ndoc++;
	W.buf[0] = 0;
	W.len = 0;
	W.path[0] = 0;
	W.dirty = 0;
	W.cx = 0;
	W.scroll = 0;
	sel_clear();
	wp_stash();
}

static const char **menu_items(int menu, int *n)
{
	switch (menu)
	{
	case WP_MENU_FILE:
		*n = (int)(sizeof(file_items) / sizeof(file_items[0]));
		return file_items;
	case WP_MENU_EDIT:
		*n = (int)(sizeof(edit_items) / sizeof(edit_items[0]));
		return edit_items;
	case WP_MENU_SETTINGS:
		*n = (int)(sizeof(settings_items) / sizeof(settings_items[0]));
		return settings_items;
	default:
		*n = 0;
		return settings_items;
	}
}

static int menu_width(int menu)
{
	int n, i, w = 10;
	const char **it = menu_items(menu, &n);

	for (i = 0; i < n; i++)
	{
		int L = (int)strlen(it[i]);
		if (L + 2 > w)
			w = L + 2;
	}
	return w + 2;
}

static void close_ui(void)
{
	W.menu_open = 0;
	W.dialog = WP_DLG_NONE;
	W.dlglen = 0;
	W.dlg[0] = 0;
	wp_rec_sidecar[0] = 0;
}

static int fd_on(void)
{
	return W.dialog == WP_DLG_OPEN || W.dialog == WP_DLG_SAVEAS;
}

static int pick_on(void)
{
	return W.dialog == WP_DLG_PICK;
}

static int wp_ch_eq(char a, char b)
{
	if (a >= 'a' && a <= 'z')
		a = (char)(a - 32);
	if (b >= 'a' && b <= 'z')
		b = (char)(b - 32);
	return a == b;
}

static int wp_contains(const char *s, const char *sub)
{
	int i, j;

	if (!sub || !sub[0])
		return 1;
	if (!s)
		return 0;
	for (i = 0; s[i]; i++)
	{
		for (j = 0; sub[j] && s[i + j] && wp_ch_eq(s[i + j], sub[j]); j++)
			;
		if (!sub[j])
			return 1;
	}
	return 0;
}

/* Filename part of a quick-open label, so a typed query can prefer a match on
 * the document name over one that only occurs in a parent directory. */
static const char *pick_base(const char *rel)
{
	const char *b = rel ? rel : "";

	while (rel && *rel)
	{
		if (*rel == '/' || *rel == ':')
			b = rel + 1;
		rel++;
	}
	return b;
}

/* Quick open lists what WORDPAD edits: Markdown documents (its Open/Save As
 * picker defaults to *.MD).  Restricting the walk keeps unrelated files from
 * filling the quick-open cap (issue #539). */
static int pick_ext_ok(const char *name)
{
	const char *dot = 0, *p;

	if (!name || !name[0])
		return 0;
	for (p = name; *p; p++)
		if (*p == '.')
			dot = p;
	if (!dot || dot == name)
		return 0;
	return wp_ch_eq(dot[1], 'M') && wp_ch_eq(dot[2], 'D') && dot[3] == 0;
}

static void wp_join(char *dst, int dstsz, const char *dir, const char *name)
{
	int n;

	strncpy(dst, dir ? dir : "", (unsigned)dstsz - 1);
	dst[dstsz - 1] = 0;
	n = (int)strlen(dst);
	if (n > 0 && dst[n - 1] != '/' && dst[n - 1] != ':' && n + 1 < dstsz)
	{
		dst[n] = '/';
		dst[n + 1] = 0;
	}
	strncat(dst, name ? name : "", (unsigned)dstsz - strlen(dst) - 1);
}

static int wp_icmp(const char *a, const char *b)
{
	for (;;)
	{
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - 32);
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			return 0;
	}
}

static const char *pick_rel(const char *full)
{
	int i;

	if (!full)
		return "";
	for (i = 0; pick_root[i]; i++)
	{
		if (!full[i] || !wp_ch_eq(full[i], pick_root[i]))
			return full;
	}
	if (full[i] == '/')
		i++;
	return full[i] ? full + i : full;
}

static void pick_walk(const char *dir, int depth)
{
	char list[2048];
	char *s;
	int pass;

	if (!dir || !dir[0] || depth > WP_PICK_DEPTH || pick_n >= WP_PICK_MAX)
		return;
	list[0] = 0;
	if (mmb_vfs_list(dir, list, sizeof(list)) != 0)
		return;
	/* Two passes: add this directory's own Markdown files first, then
	 * descend into its subdirectories.  mmb_vfs_list returns folders before
	 * files, so walking it once would let a big subdirectory exhaust the cap
	 * before the current directory's documents are collected. */
	for (pass = 0; pass < 2; pass++)
	{
		s = list;
		while (*s && pick_n < WP_PICK_MAX)
		{
			char name[128];
			int n = 0, is_dir = 0;

			while (*s && *s != '\n' && *s != '\r' && n < (int)sizeof(name) - 1)
				name[n++] = *s++;
			name[n] = 0;
			while (*s == '\r' || *s == '\n')
				s++;
			if (n > 0 && name[n - 1] == '/')
			{
				name[n - 1] = 0;
				is_dir = 1;
			}
			if (!name[0] || (name[0] == '.' && (!name[1] ||
			    (name[1] == '.' && !name[2]))))
				continue;
			if ((is_dir != 0) != (pass != 0))
				continue;
			{
				char full[128];

				wp_join(full, sizeof(full), dir, name);
				if (is_dir)
					pick_walk(full, depth + 1);
				else if (pick_ext_ok(name))
				{
					strncpy(pick_path[pick_n], full, sizeof(pick_path[0]) - 1);
					pick_path[pick_n][sizeof(pick_path[0]) - 1] = 0;
					pick_n++;
				}
			}
		}
	}
}

static void set_pick_root(const char *path)
{
	char full[128];
	const char *src = (path && path[0]) ? path : mmb_vfs_cwd();

	pick_root[0] = 0;
	if (mmb_vfs_resolve(src, full, sizeof(full)) != 0)
	{
		strncpy(pick_root, mmb_vfs_cwd(), sizeof(pick_root) - 1);
		pick_root[sizeof(pick_root) - 1] = 0;
		return;
	}
	if (!(mmb_vfs_exists(full) && mmb_vfs_size(full) < 0))
	{
		char *slash = 0;
		char *q = full;

		while (*q)
		{
			if (*q == '/')
				slash = q;
			q++;
		}
		if (slash)
		{
			if (slash <= full + 2)
				slash[1] = 0;
			else
				*slash = 0;
		}
	}
	strncpy(pick_root, full, sizeof(pick_root) - 1);
	pick_root[sizeof(pick_root) - 1] = 0;
}

static void pick_sort(void)
{
	int i, j;

	for (i = 0; i < pick_n; i++)
		for (j = i + 1; j < pick_n; j++)
			if (wp_icmp(pick_rel(pick_path[j]), pick_rel(pick_path[i])) < 0)
			{
				char tmp[128];

				strncpy(tmp, pick_path[i], sizeof(tmp) - 1);
				tmp[sizeof(tmp) - 1] = 0;
				strncpy(pick_path[i], pick_path[j], sizeof(pick_path[i]) - 1);
				pick_path[i][sizeof(pick_path[i]) - 1] = 0;
				strncpy(pick_path[j], tmp, sizeof(pick_path[j]) - 1);
				pick_path[j][sizeof(pick_path[j]) - 1] = 0;
			}
}

static void pick_rebuild_view(void)
{
	int i, pass;

	pick_vn = 0;
	/* Two passes so a query that names a document ("b") selects it ahead of
	 * entries that merely live under a matching directory ("lib/"). */
	for (pass = 0; pass < 2; pass++)
	{
		for (i = 0; i < pick_n; i++)
		{
			const char *rel = pick_rel(pick_path[i]);
			const char *base = pick_base(rel);
			int hit;

			if (pass == 0)
				hit = wp_contains(base, W.dlg);
			else
				hit = wp_contains(rel, W.dlg) &&
				      !wp_contains(base, W.dlg);
			if (!hit)
				continue;
			pick_view[pick_vn++] = i;
		}
	}
	if (pick_sel >= pick_vn)
		pick_sel = pick_vn > 0 ? pick_vn - 1 : 0;
	if (pick_sel < 0)
		pick_sel = 0;
}

static void pick_geom(int *w, int *h, int *r0, int *c0)
{
	int ww = 58, hh = 16;

	if (ww > W.vid_cols - 2)
		ww = W.vid_cols - 2;
	if (hh > W.vid_rows - 2)
		hh = W.vid_rows - 2;
	if (w)
		*w = ww;
	if (h)
		*h = hh;
	if (r0)
	{
		*r0 = (W.vid_rows - hh) / 2;
		if (*r0 < 1)
			*r0 = 1;
	}
	if (c0)
	{
		*c0 = (W.vid_cols - ww) / 2;
		if (*c0 < 0)
			*c0 = 0;
	}
}

static int pick_list_h(int h)
{
	int n = h - 6;

	return n > 1 ? n : 1;
}

static void pick_move(int delta)
{
	int vis, w, h, r0, c0;

	if (pick_vn <= 0)
		return;
	pick_sel += delta;
	if (pick_sel < 0)
		pick_sel = 0;
	if (pick_sel >= pick_vn)
		pick_sel = pick_vn - 1;
	pick_geom(&w, &h, &r0, &c0);
	(void)w;
	(void)r0;
	(void)c0;
	vis = pick_list_h(h);
	if (pick_sel < pick_row0)
		pick_row0 = pick_sel;
	if (pick_sel >= pick_row0 + vis)
		pick_row0 = pick_sel - vis + 1;
}

static void open_picker(void)
{
	W.menu_open = 0;
	W.dialog = WP_DLG_PICK;
	W.dlg[0] = 0;
	W.dlglen = 0;
	pick_n = 0;
	pick_sel = 0;
	pick_row0 = 0;
	pick_vn = 0;
	if (!pick_root[0])
		set_pick_root(mmb_vfs_cwd());
	pick_walk(pick_root, 0);
	pick_sort();
	pick_rebuild_view();
}

static void pick_submit(void)
{
	if (pick_vn > 0 && pick_sel >= 0 && pick_sel < pick_vn)
	{
		int idx = pick_view[pick_sel];

		wp_open_path(pick_path[idx]);
	}
	close_ui();
}

static void fd_copy(char *dst, int n, const char *s)
{
	if (!dst || n <= 0)
		return;
	if (!s)
		s = "";
	strncpy(dst, s, (unsigned)n - 1);
	dst[n - 1] = 0;
}

static int fd_icmp(const char *a, const char *b)
{
	for (;;)
	{
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'a' && ca <= 'z')
			ca = (unsigned char)(ca - 32);
		if (cb >= 'a' && cb <= 'z')
			cb = (unsigned char)(cb - 32);
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			return 0;
	}
}

static int fd_match(const char *name, const char *pat)
{
	const char *n, *p, *star, *match;

	if (!pat || !pat[0] || (pat[0] == '*' && pat[1] == 0))
		return 1;
	n = name;
	p = pat;
	star = 0;
	match = 0;
	while (*n)
	{
		char cn = *n, cp = *p;
		if (cn >= 'a' && cn <= 'z')
			cn = (char)(cn - 32);
		if (cp >= 'a' && cp <= 'z')
			cp = (char)(cp - 32);
		if (cp == '*')
		{
			star = p++;
			match = n;
			continue;
		}
		if (cp == '?' || cn == cp)
		{
			n++;
			p++;
			continue;
		}
		if (star)
		{
			p = star + 1;
			match++;
			n = match;
			continue;
		}
		return 0;
	}
	while (*p == '*')
		p++;
	return *p == 0;
}

static int fd_has_glob(const char *s)
{
	return s && (strchr(s, '*') || strchr(s, '?'));
}

static const char *fd_rchr(const char *s, char ch)
{
	const char *last = 0;

	if (!s)
		return 0;
	for (; *s; s++)
		if (*s == ch)
			last = s;
	return last;
}

static int fd_is_dir(const char *path)
{
	if (!path || !path[0])
		return 0;
	if (!mmb_vfs_exists(path))
		return 0;
	return mmb_vfs_size(path) < 0;
}

static void fd_parent(char *path)
{
	char *slash = 0, *p;

	if (!path[0])
		return;
	if (path[0] && path[1] == ':' && path[2] == 0)
	{
		path[2] = '/';
		path[3] = 0;
		return;
	}
	for (p = path + 2; *p; p++)
		if (*p == '/')
			slash = p;
	if (!slash || slash <= path + 2)
	{
		if (path[0] && path[1] == ':')
		{
			path[2] = '/';
			path[3] = 0;
		}
		return;
	}
	*slash = 0;
}

static void fd_join(char *dst, int n, const char *dir, const char *name)
{
	int last;

	fd_copy(dst, n, dir);
	if (!name || !name[0])
		return;
	last = dst[0] ? (int)(unsigned char)dst[strlen(dst) - 1] : 0;
	if (last && last != '/' && last != ':')
		strncat(dst, "/", (unsigned)n - strlen(dst) - 1);
	else if (last == ':')
		strncat(dst, "/", (unsigned)n - strlen(dst) - 1);
	strncat(dst, name, (unsigned)n - strlen(dst) - 1);
}

static void fd_make_full(char *dst, int n, const char *name)
{
	if (!name || !name[0])
	{
		fd_copy(dst, n, fd_dir);
		return;
	}
	if (name[1] == ':')
	{
		fd_copy(dst, n, name);
		return;
	}
	fd_join(dst, n, fd_dir, name);
}

static void fd_basename(char *out, int n, const char *path)
{
	const char *s = path, *p;

	if (!path)
		path = "";
	for (p = path; *p; p++)
		if (*p == '/' || *p == ':')
			s = p + 1;
	fd_copy(out, n, s);
}

static void fd_dirname(char *out, int n, const char *path)
{
	char tmp[128];
	char *slash;

	fd_copy(tmp, sizeof(tmp), path);
	slash = (char *)fd_rchr(tmp, '/');
	if (!slash)
	{
		out[0] = 0;
		return;
	}
	if (slash <= tmp + 2 && tmp[1] == ':')
	{
		tmp[2] = '/';
		tmp[3] = 0;
		fd_copy(out, n, tmp);
		return;
	}
	*slash = 0;
	if (tmp[0] && tmp[1] == ':' && tmp[2] == 0)
	{
		tmp[2] = '/';
		tmp[3] = 0;
	}
	fd_copy(out, n, tmp);
}

static void fd_add(char arr[][WP_FD_NAME], int *n, const char *s)
{
	int i;

	if (*n >= WP_FD_MAX || !s || !s[0])
		return;
	for (i = 0; i < *n; i++)
		if (mmb_keyword_eq(arr[i], s))
			return;
	strncpy(arr[*n], s, WP_FD_NAME - 1);
	arr[*n][WP_FD_NAME - 1] = 0;
	(*n)++;
}

static void fd_swap(char arr[][WP_FD_NAME], int i, int j)
{
	char t[WP_FD_NAME];

	memcpy(t, arr[i], WP_FD_NAME);
	memcpy(arr[i], arr[j], WP_FD_NAME);
	memcpy(arr[j], t, WP_FD_NAME);
}

static int fd_dir_rank(const char *s)
{
	if (s[0] == '.' && s[1] == '.' && s[2] == 0)
		return 0;
	if (s[0] == '[' && s[1] == '-')
		return 2;
	return 1;
}

static void fd_sort_files(void)
{
	int i, j;

	for (i = 0; i < fd_nfile; i++)
		for (j = i + 1; j < fd_nfile; j++)
			if (fd_icmp(fd_files[j], fd_files[i]) < 0)
				fd_swap(fd_files, i, j);
}

static void fd_sort_dirs(void)
{
	int i, j;

	for (i = 0; i < fd_ndir; i++)
		for (j = i + 1; j < fd_ndir; j++)
		{
			int ra = fd_dir_rank(fd_dirs[i]);
			int rb = fd_dir_rank(fd_dirs[j]);
			if (rb < ra || (rb == ra && fd_icmp(fd_dirs[j], fd_dirs[i]) < 0))
				fd_swap(fd_dirs, i, j);
		}
}

static void fd_scan(void)
{
	char list[2048], drives[256];
	char *s, *nl;

	fd_nfile = 0;
	fd_ndir = 0;
	fd_add(fd_dirs, &fd_ndir, "..");
	list[0] = 0;
	if (mmb_vfs_list(fd_dir, list, sizeof(list)) != 0)
		list[0] = 0;
	s = list;
	while (*s)
	{
		char name[WP_FD_NAME];
		int n, is_dir = 0;

		nl = s;
		while (*nl && *nl != '\n' && *nl != '\r')
			nl++;
		n = (int)(nl - s);
		if (n >= WP_FD_NAME)
			n = WP_FD_NAME - 1;
		memcpy(name, s, (unsigned)n);
		name[n] = 0;
		if (n > 0 && name[n - 1] == '/')
			is_dir = 1;
		if (name[0] && !(name[0] == '.' && name[1] == 0) &&
		    !(name[0] == '.' && name[1] == '.' &&
		      (name[2] == 0 || name[2] == '/')))
		{
			if (is_dir)
				fd_add(fd_dirs, &fd_ndir, name);
			else if (fd_match(name, wp_fd_mask))
				fd_add(fd_files, &fd_nfile, name);
		}
		s = nl;
		if (*s == '\r')
			s++;
		if (*s == '\n')
			s++;
	}
	drives[0] = 0;
	mmb_vfs_drives(drives, sizeof(drives));
	s = drives;
	while (*s)
	{
		nl = s;
		while (*nl && *nl != '\n' && *nl != '\r')
			nl++;
		if (s[0] && s[1] == ':')
		{
			char spec[8];
			char L = s[0];
			if (L >= 'a' && L <= 'z')
				L = (char)(L - 32);
			spec[0] = '[';
			spec[1] = '-';
			spec[2] = L;
			spec[3] = '-';
			spec[4] = ']';
			spec[5] = 0;
			fd_add(fd_dirs, &fd_ndir, spec);
		}
		s = nl;
		if (*s == '\r')
			s++;
		if (*s == '\n')
			s++;
	}
	fd_sort_files();
	fd_sort_dirs();
	fd_fsel = 0;
	fd_dsel = 0;
	fd_ftop = 0;
	fd_dtop = 0;
}

static void fd_geom(int *c0, int *r0, int *w, int *h)
{
	*w = 62;
	*h = 18;
	if (*w > W.vid_cols - 2)
		*w = W.vid_cols - 2;
	if (*h > W.vid_rows - 2)
		*h = W.vid_rows - 2;
	*r0 = (W.vid_rows - *h) / 2;
	*c0 = (W.vid_cols - *w) / 2;
	if (*r0 < 2)
		*r0 = 2;
	if (*c0 < 0)
		*c0 = 0;
}

static int fd_list_h(void)
{
	int c0, r0, w, h, lh;

	fd_geom(&c0, &r0, &w, &h);
	lh = h - 8;
	return lh < 4 ? 4 : lh;
}

static void fd_clamp(void)
{
	int lh = fd_list_h();

	if (fd_nfile <= 0)
	{
		fd_fsel = 0;
		fd_ftop = 0;
	}
	else
	{
		if (fd_fsel < 0)
			fd_fsel = 0;
		if (fd_fsel >= fd_nfile)
			fd_fsel = fd_nfile - 1;
		if (fd_fsel < fd_ftop)
			fd_ftop = fd_fsel;
		if (fd_fsel >= fd_ftop + lh)
			fd_ftop = fd_fsel - lh + 1;
		if (fd_ftop < 0)
			fd_ftop = 0;
	}
	if (fd_ndir <= 0)
	{
		fd_dsel = 0;
		fd_dtop = 0;
	}
	else
	{
		if (fd_dsel < 0)
			fd_dsel = 0;
		if (fd_dsel >= fd_ndir)
			fd_dsel = fd_ndir - 1;
		if (fd_dsel < fd_dtop)
			fd_dtop = fd_dsel;
		if (fd_dsel >= fd_dtop + lh)
			fd_dtop = fd_dsel - lh + 1;
		if (fd_dtop < 0)
			fd_dtop = 0;
	}
}

static void fd_copy_file_to_name(void)
{
	if (fd_fsel < 0 || fd_fsel >= fd_nfile)
		return;
	fd_copy(W.dlg, sizeof(W.dlg), fd_files[fd_fsel]);
	W.dlglen = (int)strlen(W.dlg);
}

static void fd_clear_name(void)
{
	W.dlg[0] = 0;
	W.dlglen = 0;
}

static void fd_set_dir(const char *path)
{
	char resolved[128];

	if (mmb_vfs_resolve(path, resolved, sizeof(resolved)) == 0)
		fd_copy(fd_dir, sizeof(fd_dir), resolved);
	else
		fd_copy(fd_dir, sizeof(fd_dir), path);
	fd_scan();
	fd_clamp();
}

static void fd_after_dir_nav(int from_lists)
{
	if (from_lists && fd_nfile > 0)
	{
		fd_focus = WP_FD_FOCUS_FILE;
		fd_copy_file_to_name();
	}
	else
	{
		if (fd_ndir > 0)
			fd_focus = WP_FD_FOCUS_DIR;
		fd_clear_name();
	}
}

static void fd_enter_listed(const char *name, int from_lists)
{
	char full[128], tmp[128];

	if (name[0] == '[' && name[1] == '-' && name[3] == '-' && name[4] == ']')
	{
		tmp[0] = name[2];
		tmp[1] = ':';
		tmp[2] = '/';
		tmp[3] = 0;
		fd_set_dir(tmp);
		fd_after_dir_nav(from_lists);
		return;
	}
	if (name[0] == '.' && name[1] == '.' && name[2] == 0)
	{
		fd_copy(full, sizeof(full), fd_dir);
		fd_parent(full);
		fd_set_dir(full);
		fd_after_dir_nav(from_lists);
		return;
	}
	fd_copy(tmp, sizeof(tmp), name);
	if (tmp[strlen(tmp) - 1] == '/')
		tmp[strlen(tmp) - 1] = 0;
	fd_join(full, sizeof(full), fd_dir, tmp);
	fd_set_dir(full);
	fd_after_dir_nav(from_lists);
}

static void fd_submit_path(const char *path)
{
	char full[128];

	fd_copy(full, sizeof(full), path);
	ensure_md(full, sizeof(full));
	if (W.dialog == WP_DLG_OPEN)
	{
		wp_open_path(full);
		close_ui();
		return;
	}
	if (W.dialog == WP_DLG_SAVEAS)
	{
		if (full[0])
		{
			strncpy(W.path, full, sizeof(W.path) - 1);
			W.path[sizeof(W.path) - 1] = 0;
			wp_save();
		}
		close_ui();
	}
}

static void fd_apply_glob(const char *spec)
{
	const char *slash = fd_rchr(spec, '/');
	char dirpart[128];

	if (slash)
	{
		int n = (int)(slash - spec);
		if (n >= (int)sizeof(dirpart))
			n = (int)sizeof(dirpart) - 1;
		memcpy(dirpart, spec, (unsigned)n);
		dirpart[n] = 0;
		if (dirpart[0] && dirpart[1] == ':' && dirpart[2] == 0)
		{
			dirpart[2] = '/';
			dirpart[3] = 0;
		}
		if (dirpart[0])
			fd_set_dir(dirpart);
		else
			fd_scan();
		fd_copy(wp_fd_mask, sizeof(wp_fd_mask), slash + 1);
		fd_scan();
		fd_clamp();
		return;
	}
	if (spec[0] && spec[1] == ':')
	{
		dirpart[0] = spec[0];
		dirpart[1] = ':';
		dirpart[2] = '/';
		dirpart[3] = 0;
		fd_set_dir(dirpart);
		fd_copy(wp_fd_mask, sizeof(wp_fd_mask), spec + 2);
		fd_scan();
		fd_clamp();
		return;
	}
	fd_copy(wp_fd_mask, sizeof(wp_fd_mask), spec);
	fd_scan();
	fd_clamp();
}

static void fd_enter_key(void)
{
	char full[128];

	if (fd_focus == WP_FD_FOCUS_DIR)
	{
		if (fd_dsel >= 0 && fd_dsel < fd_ndir)
			fd_enter_listed(fd_dirs[fd_dsel], 1);
		return;
	}
	if (fd_focus == WP_FD_FOCUS_FILE)
	{
		if (fd_fsel >= 0 && fd_fsel < fd_nfile)
		{
			fd_make_full(full, sizeof(full), fd_files[fd_fsel]);
			fd_submit_path(full);
		}
		return;
	}
	if (!W.dlg[0])
		return;
	if (fd_has_glob(W.dlg))
	{
		fd_apply_glob(W.dlg);
		fd_clear_name();
		return;
	}
	fd_make_full(full, sizeof(full), W.dlg);
	if (fd_is_dir(full))
	{
		fd_set_dir(full);
		fd_clear_name();
		fd_focus = WP_FD_FOCUS_NAME;
		return;
	}
	fd_submit_path(full);
}

static void fd_tab(void)
{
	if (fd_focus == WP_FD_FOCUS_NAME)
		fd_focus = fd_nfile ? WP_FD_FOCUS_FILE : WP_FD_FOCUS_DIR;
	else if (fd_focus == WP_FD_FOCUS_FILE)
		fd_focus = fd_ndir ? WP_FD_FOCUS_DIR : WP_FD_FOCUS_NAME;
	else
		fd_focus = WP_FD_FOCUS_NAME;
	if (fd_focus == WP_FD_FOCUS_FILE)
		fd_copy_file_to_name();
}

static void fd_move_sel(int *sel, int n, int delta)
{
	if (n <= 0)
		return;
	*sel += delta;
	if (*sel < 0)
		*sel = 0;
	if (*sel >= n)
		*sel = n - 1;
}

static int fd_arrow(int kind)
{
	int lh = fd_list_h();

	if (fd_focus == WP_FD_FOCUS_NAME)
	{
		if (kind == 3 || kind == 4)
			fd_tab();
		return 1;
	}
	if (kind == 3 && fd_focus == WP_FD_FOCUS_FILE)
	{
		fd_focus = WP_FD_FOCUS_DIR;
		return 1;
	}
	if (kind == 4 && fd_focus == WP_FD_FOCUS_DIR)
	{
		fd_focus = fd_nfile ? WP_FD_FOCUS_FILE : WP_FD_FOCUS_NAME;
		if (fd_focus == WP_FD_FOCUS_FILE)
			fd_copy_file_to_name();
		return 1;
	}
	if (fd_focus == WP_FD_FOCUS_FILE)
	{
		if (kind == 1)
			fd_move_sel(&fd_fsel, fd_nfile, -1);
		else if (kind == 2)
			fd_move_sel(&fd_fsel, fd_nfile, 1);
		else if (kind == 5)
			fd_fsel = 0;
		else if (kind == 6)
			fd_fsel = fd_nfile ? fd_nfile - 1 : 0;
		else if (kind == 9)
			fd_move_sel(&fd_fsel, fd_nfile, -lh);
		else if (kind == 10)
			fd_move_sel(&fd_fsel, fd_nfile, lh);
		fd_clamp();
		fd_copy_file_to_name();
		return 1;
	}
	if (kind == 1)
		fd_move_sel(&fd_dsel, fd_ndir, -1);
	else if (kind == 2)
		fd_move_sel(&fd_dsel, fd_ndir, 1);
	else if (kind == 5)
		fd_dsel = 0;
	else if (kind == 6)
		fd_dsel = fd_ndir ? fd_ndir - 1 : 0;
	else if (kind == 9)
		fd_move_sel(&fd_dsel, fd_ndir, -lh);
	else if (kind == 10)
		fd_move_sel(&fd_dsel, fd_ndir, lh);
	fd_clamp();
	return 1;
}

static void open_dialog(int which)
{
	char dir[128], base[128];

	W.menu_open = 0;
	W.dialog = which;
	W.dlg[0] = 0;
	W.dlglen = 0;
	if (which == WP_DLG_OPEN || which == WP_DLG_SAVEAS)
	{
		fd_focus = WP_FD_FOCUS_NAME;
		fd_copy(wp_fd_mask, sizeof(wp_fd_mask), "*.MD");
		fd_copy(fd_dir, sizeof(fd_dir), mmb_vfs_cwd());
		if (which == WP_DLG_SAVEAS && W.path[0])
		{
			fd_dirname(dir, sizeof(dir), W.path);
			fd_basename(base, sizeof(base), W.path);
			if (dir[0])
				fd_copy(fd_dir, sizeof(fd_dir), dir);
			fd_copy(W.dlg, sizeof(W.dlg), base);
			W.dlglen = (int)strlen(W.dlg);
		}
		fd_scan();
		fd_clamp();
	}
}

static void open_menu(int which)
{
	W.dialog = WP_DLG_NONE;
	W.menu_open = 1;
	W.menu = which;
	W.menu_item = 0;
}

static void activate_menu(void)
{
	int menu = W.menu;
	int item = W.menu_item;

	W.menu_open = 0;
	if (menu == WP_MENU_FILE)
	{
		if (item == 0)
			wp_new_doc();
		else if (item == 1)
			open_dialog(WP_DLG_OPEN);
		else if (item == 2)
		{
			if (!wp_save())
				open_dialog(WP_DLG_SAVEAS);
		}
		else if (item == 3)
			open_dialog(WP_DLG_SAVEAS);
		else if (item == 4)
			wp_leave();
	}
	else if (menu == WP_MENU_EDIT)
	{
		if (item == 0)
			copy_selection();
		else if (item == 1)
			cut_selection();
		else
			paste_clip();
	}
	else if (menu == WP_MENU_SETTINGS)
	{
		W.wide = !W.wide;
		W.scroll = 0;
	}
}

static int handle_alt(char c)
{
	if (c >= 'A' && c <= 'Z')
		c = (char)(c - 'A' + 'a');
	if (c == 'f')
	{
		open_menu(WP_MENU_FILE);
		return 1;
	}
	if (c == 'e')
	{
		open_menu(WP_MENU_EDIT);
		return 1;
	}
	if (c == 's')
	{
		open_menu(WP_MENU_SETTINGS);
		return 1;
	}
	if (c == 'x')
	{
		wp_leave();
		return 1;
	}
	return 0;
}

static void do_fkey(int n)
{
	if (n == 2)
	{
		if (!wp_save())
			open_dialog(WP_DLG_SAVEAS);
	}
	else if (n == 3)
		open_dialog(WP_DLG_OPEN);
	else if (n == 10)
		open_menu(WP_MENU_FILE);
}

static int handle_arrow_or_special(int kind, int mod)
{
	int shift = 0, ctrl = 0;

	if (mod > 1)
	{
		shift = ((mod - 1) & 1) != 0;
		ctrl = ((mod - 1) & 4) != 0;
	}
	if (pick_on())
	{
		int vis, w, h, r0, c0;
		pick_geom(&w, &h, &r0, &c0);
		(void)w;
		(void)r0;
		(void)c0;
		vis = pick_list_h(h);
		if (kind == 1)
			pick_move(-1);
		else if (kind == 2)
			pick_move(1);
		else if (kind == 5)
			pick_move(-pick_vn);
		else if (kind == 6)
			pick_move(pick_vn);
		else if (kind == 9)
			pick_move(-vis);
		else if (kind == 10)
			pick_move(vis);
		return 1;
	}
	if (fd_on())
	{
		fd_arrow(kind);
		return 1;
	}
	if (W.menu_open)
	{
		int n;
		menu_items(W.menu, &n);
		if (kind == 1)
		{
			if (W.menu_item > 0)
				W.menu_item--;
			else
				W.menu_item = n - 1;
		}
		else if (kind == 2)
			W.menu_item = (W.menu_item + 1) % n;
		else if (kind == 4)
		{
			W.menu = (W.menu + WP_MENU_COUNT - 1) % WP_MENU_COUNT;
			W.menu_item = 0;
		}
		else if (kind == 3)
		{
			W.menu = (W.menu + 1) % WP_MENU_COUNT;
			W.menu_item = 0;
		}
		return 1;
	}
	if (kind == 7)
	{
		if (shift)
			cut_selection();
		else
			delete_char();
		return 1;
	}
	if (kind == 8)
	{
		if (ctrl)
			copy_selection();
		else if (shift)
			paste_clip();
		return 1;
	}
	if (shift && !ctrl && (kind == 1 || kind == 2))
	{
		if (!W.sel)
		{
			W.sel_anchor = line_start(W.cx);
			W.sel = 1;
		}
		else
			sel_prepare(shift);
		if (kind == 2)
		{
			int e = line_end(W.cx);
			if (e < W.len && W.buf[e] == '\n')
				W.cx = e + 1;
			else
				W.cx = e;
		}
		else
		{
			int s = line_start(W.cx);
			if (s > 0)
				W.cx = line_start(s - 1);
			else
				W.cx = 0;
		}
		return 1;
	}
	sel_prepare(shift);
	if (kind == 1)
		move_up();
	else if (kind == 2)
		move_down();
	else if (kind == 3)
		move_right();
	else if (kind == 4)
		move_left();
	else if (kind == 5)
		move_home();
	else if (kind == 6)
		move_end();
	else if (kind == 9)
		page_up();
	else if (kind == 10)
		page_down();
	return 1;
}

static int handle_escape(char c)
{
	if (W.esc_state == WP_ESC_GOT)
	{
		if (c == '[')
		{
			W.esc_state = WP_ESC_CSI;
			W.csi_n = 0;
			W.csi_arg = 0;
			W.csi_semi = 0;
			return 1;
		}
		if (c == 'O')
		{
			W.esc_state = WP_ESC_SS3;
			return 1;
		}
		W.esc_state = WP_ESC_NONE;
		if (W.menu_open || W.dialog)
		{
			close_ui();
			return 1;
		}
		return 0;
	}
	if (W.esc_state == WP_ESC_SS3)
	{
		W.esc_state = WP_ESC_NONE;
		if (c >= 'A' && c <= 'E')
			do_fkey(c - 'A' + 1);
		return 1;
	}
	if (W.esc_state == WP_ESC_CSI)
	{
		if (c >= '0' && c <= '9')
		{
			W.csi_arg = W.csi_arg * 10 + (c - '0');
			return 1;
		}
		if (c == ';')
		{
			if (!W.csi_n)
				W.csi_n = W.csi_arg;
			W.csi_arg = 0;
			W.csi_semi = 1;
			return 1;
		}
		if (c == '[')
		{
			W.esc_state = WP_ESC_SS3;
			return 1;
		}
		W.esc_state = WP_ESC_NONE;
		{
			int mod = W.csi_semi ? W.csi_arg : 0;
			if (c == 'A')
				handle_arrow_or_special(1, mod);
			else if (c == 'B')
				handle_arrow_or_special(2, mod);
			else if (c == 'C')
				handle_arrow_or_special(3, mod);
			else if (c == 'D')
				handle_arrow_or_special(4, mod);
			else if (c == 'H')
				handle_arrow_or_special(5, mod);
			else if (c == 'F')
				handle_arrow_or_special(6, mod);
			else if (c == 'Z')
				indent_line(-1);
			else if (c == '~')
			{
				int n = W.csi_semi ? W.csi_n : W.csi_arg;
				if (n == 1 || n == 7)
					handle_arrow_or_special(5, mod);
				else if (n == 4 || n == 8)
					handle_arrow_or_special(6, mod);
				else if (n == 3)
					handle_arrow_or_special(7, mod);
				else if (n == 2)
					handle_arrow_or_special(8, mod);
				else if (n == 5)
					handle_arrow_or_special(9, mod);
				else if (n == 6)
					handle_arrow_or_special(10, mod);
				else if (n >= 11 && n <= 15)
					do_fkey(n - 10);
				else if (n >= 17 && n <= 21)
					do_fkey(n - 11);
			}
		}
		return 1;
	}
	return 0;
}

static int dialog_key(char c)
{
	if (!W.dialog)
		return 0;
	if (c == 27)
	{
		W.esc_state = WP_ESC_GOT;
		W.esc_at = mmb_now_ms();
		return 1;
	}
	if (W.dialog == WP_DLG_RECOVER)
	{
		char u = c;
		if (u >= 'A' && u <= 'Z')
			u = (char)(u - 'A' + 'a');
		if (u == 'y' || c == '\r' || c == '\n')
			wp_recovery_apply();
		else if (u == 'n')
			wp_recovery_discard();
		else
			return 1;
		close_ui();
		return 1;
	}
	if (c == '\r' || c == '\n')
	{
		if (pick_on())
			pick_submit();
		else
			fd_enter_key();
		return 1;
	}
	if (fd_on() && c == 9)
	{
		fd_tab();
		return 1;
	}
	if (c == 8 || c == 127)
	{
		if (fd_on() && fd_focus != WP_FD_FOCUS_NAME)
			fd_focus = WP_FD_FOCUS_NAME;
		if (W.dlglen > 0)
			W.dlg[--W.dlglen] = 0;
		if (pick_on())
			pick_rebuild_view();
		return 1;
	}
	if (c >= 32 && c < 127 && W.dlglen < (int)sizeof(W.dlg) - 1)
	{
		if (fd_on() && fd_focus != WP_FD_FOCUS_NAME)
		{
			fd_focus = WP_FD_FOCUS_NAME;
			fd_clear_name();
		}
		W.dlg[W.dlglen++] = c;
		W.dlg[W.dlglen] = 0;
		if (pick_on())
			pick_rebuild_view();
		return 1;
	}
	return 1;
}

static void draw_hot_str(int x, int y, const char *word, char hot, unsigned fg,
			 unsigned bg, unsigned hot_fg)
{
	int i, used = 0;

	if (!G.plat || !G.plat->tui_glyph || !word)
		return;
	for (i = 0; word[i]; i++)
	{
		unsigned c_fg = fg;
		char ch = word[i];

		if (!used && (ch == hot || ch == hot + 32 || ch == hot - 32))
		{
			c_fg = hot_fg;
			used = 1;
		}
		G.plat->tui_glyph(x++, y, (unsigned)ch, c_fg, bg);
	}
}

static void draw_menu_bar(void)
{
	unsigned mbg = dlg_bg();
	int i, x = 0;
	char line[160];
	int pos = 0;

	wp_fill_row(0, WP_MENU_FG, mbg);
	for (i = 0; i < WP_MENU_COUNT; i++)
	{
		int sel = W.menu_open && W.menu == i;
		unsigned fg = sel ? WP_SEL_FG : WP_MENU_FG;
		unsigned bg = sel ? WP_SEL_BG : mbg;
		unsigned hot = sel ? WP_HOT : WP_HEAD;

		menu_x[i] = x;
		draw_hot_str(x, 0, menu_names[i], menu_hots[i], fg, bg, hot);
		x += (int)strlen(menu_names[i]);
		wp_puts(x, 0, "  ", WP_MENU_FG, mbg);
		x += 2;
		if (pos + 32 < (int)sizeof(line))
		{
			int k;
			for (k = 0; menu_names[i][k] && pos + 1 < (int)sizeof(line); k++)
				line[pos++] = menu_names[i][k];
			line[pos++] = ' ';
			line[pos++] = ' ';
		}
	}
	line[pos] = 0;
	serial_row(line);
}

static void draw_dropdown(void)
{
	unsigned sbg = dlg_bg();
	int n, i, w, x0, y0, j;
	const char **it = menu_items(W.menu, &n);

	w = menu_width(W.menu);
	x0 = menu_x[W.menu];
	if (x0 + w > W.vid_cols)
		x0 = W.vid_cols - w;
	if (x0 < 0)
		x0 = 0;
	y0 = 1;
	wp_box(x0, y0, w, n + 2, WP_HEAD, sbg);
	for (i = 0; i < n; i++)
	{
		unsigned fg = (i == W.menu_item) ? WP_SEL_FG : WP_FG;
		unsigned bg = (i == W.menu_item) ? WP_SEL_BG : sbg;
		unsigned hot = (i == W.menu_item) ? WP_HOT : WP_HEAD;
		char row[64];

		for (j = 0; j < w - 2; j++)
			G.plat->tui_glyph(x0 + 1 + j, y0 + 1 + i, ' ', fg, bg);
		strncpy(row, it[i], sizeof(row) - 1);
		row[sizeof(row) - 1] = 0;
		draw_hot_str(x0 + 1, y0 + 1 + i, row, row[0], fg, bg, hot);
		serial_row(it[i]);
	}
}

static void draw_file_dialog(void)
{
	unsigned sbg = dlg_bg();
	int c0, r0, w, h, lh, i, c, r;
	unsigned nfg, nbg, ffg, fbg, dfg, dbg;
	char st[128];

	fd_geom(&c0, &r0, &w, &h);
	lh = fd_list_h();
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			G.plat->tui_glyph(c0 + c, r0 + r, ' ', WP_FG, sbg);
	wp_box(c0, r0, w, h, WP_HEAD, sbg);
	nfg = (fd_focus == WP_FD_FOCUS_NAME) ? WP_SEL_FG : WP_FG;
	nbg = (fd_focus == WP_FD_FOCUS_NAME) ? WP_SEL_BG : WP_DIM;
	ffg = (fd_focus == WP_FD_FOCUS_FILE) ? WP_SEL_FG : WP_FG;
	fbg = (fd_focus == WP_FD_FOCUS_FILE) ? WP_SEL_BG : sbg;
	dfg = (fd_focus == WP_FD_FOCUS_DIR) ? WP_SEL_FG : WP_FG;
	dbg = (fd_focus == WP_FD_FOCUS_DIR) ? WP_SEL_BG : sbg;
	wp_puts(c0 + 2, r0 + 1, W.dialog == WP_DLG_OPEN ? "Open" : "Save As", WP_HEAD, sbg);
	wp_puts(c0 + 2, r0 + 2, "Name:", WP_DIM, sbg);
	{
		char namebuf[WP_DLG + 4];
		namebuf[0] = ' ';
		strncpy(namebuf + 1, W.dlg, sizeof(namebuf) - 2);
		namebuf[sizeof(namebuf) - 1] = 0;
		wp_puts(c0 + 2, r0 + 3, namebuf, nfg, nbg);
	}
	wp_puts(c0 + 2, r0 + 5, "Files", WP_DIM, sbg);
	wp_puts(c0 + 2 + w / 2, r0 + 5, "Directories", WP_DIM, sbg);
	for (i = 0; i < lh; i++)
	{
		int fi = fd_ftop + i;
		int di = fd_dtop + i;
		const char *fn = (fi >= 0 && fi < fd_nfile) ? fd_files[fi] : "";
		const char *dn = (di >= 0 && di < fd_ndir) ? fd_dirs[di] : "";
		unsigned sfg = ffg, sbg2 = fbg;

		if (fi == fd_fsel && fd_nfile > 0)
		{
			sfg = (fd_focus == WP_FD_FOCUS_FILE) ? WP_SEL_FG : WP_FG;
			sbg2 = (fd_focus == WP_FD_FOCUS_FILE) ? WP_SEL_BG : WP_DIM;
		}
		wp_puts(c0 + 2, r0 + 6 + i, fn, sfg, sbg2);
		sfg = dfg;
		sbg2 = dbg;
		if (di == fd_dsel && fd_ndir > 0)
		{
			sfg = (fd_focus == WP_FD_FOCUS_DIR) ? WP_SEL_FG : WP_FG;
			sbg2 = (fd_focus == WP_FD_FOCUS_DIR) ? WP_SEL_BG : WP_DIM;
		}
		wp_puts(c0 + 2 + w / 2, r0 + 6 + i, dn, sfg, sbg2);
	}
	st[0] = 0;
	strncat(st, fd_dir, sizeof(st) - 1);
	strncat(st, "  ", sizeof(st) - strlen(st) - 1);
	strncat(st, wp_fd_mask, sizeof(st) - strlen(st) - 1);
	wp_puts(c0 + 2, r0 + h - 3, st, WP_DIM, sbg);
	wp_puts(c0 + 2, r0 + h - 2, "Tab  Enter=OK  Esc=Cancel", WP_DIM, sbg);
}

static void draw_recover_dialog(void)
{
	unsigned sbg = dlg_bg();
	int w = 44, h = 7, c0, r0, i, c, msgx;
	const char *title = " Recover ";
	const char *msg = "Recover unsaved changes?";
	const char *msg2 = "Y Yes   N No   Esc later";

	if (w > W.vid_cols - 2)
		w = W.vid_cols - 2;
	if (h > W.vid_rows - 2)
		h = W.vid_rows - 2;
	if (w < 20)
		w = 20;
	c0 = (W.vid_cols - w) / 2;
	if (c0 < 0)
		c0 = 0;
	r0 = (W.vid_rows - h) / 2;
	if (r0 < 1)
		r0 = 1;
	for (i = 0; i < h; i++)
		for (c = 0; c < w; c++)
			G.plat->tui_glyph(c0 + c, r0 + i, ' ', WP_FG, sbg);
	wp_box(c0, r0, w, h, WP_HEAD, sbg);
	{
		int left = (w - 2 - (int)strlen(title)) / 2;
		if (left < 1)
			left = 1;
		wp_puts(c0 + 1 + left, r0, title, WP_HEAD, sbg);
	}
	msgx = (w - 2 - (int)strlen(msg)) / 2;
	if (msgx < 1)
		msgx = 1;
	wp_puts(c0 + 1 + msgx, r0 + 2, msg, WP_FG, sbg);
	msgx = (w - 2 - (int)strlen(msg2)) / 2;
	if (msgx < 1)
		msgx = 1;
	wp_puts(c0 + 1 + msgx, r0 + 4, msg2, WP_DIM, sbg);
	serial_row("Recover unsaved changes? Y/N");
}

static void draw_picker(void)
{
	unsigned sbg = dlg_bg();
	int w, h, r0, c0, i, vis, y, c, r;
	const char *title = " Quick open ";

	pick_geom(&w, &h, &r0, &c0);
	vis = pick_list_h(h);
	if (pick_sel < pick_row0)
		pick_row0 = pick_sel;
	if (pick_sel >= pick_row0 + vis)
		pick_row0 = pick_sel - vis + 1;
	if (pick_row0 < 0)
		pick_row0 = 0;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			G.plat->tui_glyph(c0 + c, r0 + r, ' ', WP_FG, sbg);
	wp_box(c0, r0, w, h, WP_HEAD, sbg);
	{
		int left = (w - 2 - (int)strlen(title)) / 2;
		if (left < 1)
			left = 1;
		wp_puts(c0 + left, r0, title, WP_HEAD, sbg);
	}
	serial_row("Quick open");
	wp_puts(c0 + 2, r0 + 1, pick_root, WP_DIM, sbg);
	wp_puts(c0 + 2, r0 + 2, W.dlg[0] ? W.dlg : "(type to filter)",
		W.dlg[0] ? WP_SEL_FG : WP_DIM,
		W.dlg[0] ? WP_SEL_BG : sbg);
	for (i = 0; i < vis; i++)
	{
		unsigned fg = WP_FG, bg = sbg;
		const char *lab = "";

		y = r0 + 3 + i;
		if (pick_row0 + i < pick_vn)
		{
			int idx = pick_view[pick_row0 + i];
			lab = pick_rel(pick_path[idx]);
			if (pick_row0 + i == pick_sel)
			{
				fg = WP_SEL_FG;
				bg = WP_SEL_BG;
			}
			serial_row(lab);
		}
		wp_puts(c0 + 2, y, lab, fg, bg);
	}
	wp_puts(c0 + 2, r0 + h - 2, "Enter=open  Esc=cancel  Up/Down", WP_DIM, sbg);
}

static void draw_cursor_cell(int x_px, int y_px, unsigned ch, unsigned fg, unsigned bg,
			     int scale)
{
	int left = 0;
	int adv;

	if (scale > 1)
	{
		adv = heading_metrics(scale, ch, &left);
		wp_fill_px(x_px, y_px, adv, scale * 16, fg);
		wp_glyph_px(x_px - left, y_px, ch, bg, fg, scale);
	}
	else
		wp_glyph(x_px / 8, y_px / 16, ch, bg, fg, scale);
}

static void draw_body(void)
{
	int vr, c, i, col, screen_y, vis;
	unsigned fg, bg;
	char srow[WP_WRAP_W + 1];
	int spos;

	wp_build_layout();
	ensure_scroll();
	for (i = 0; i < W.text_rows; i++)
	{
		int screen_row = W.text_top + i;
		wp_fill_row(screen_row, WP_FG, WP_BG);
		for (c = 0; c < W.pane_left; c++)
			G.plat->tui_glyph(c, screen_row, ' ', WP_BG, WP_BG);
		for (c = W.pane_left + W.pane_width; c < W.vid_cols; c++)
			G.plat->tui_glyph(c, screen_row, ' ', WP_BG, WP_BG);
	}
	screen_y = 0;
	for (vr = 0; vr < W.total_vrows; vr++)
	{
		int scale = vrow_h(vr);
		int vh = scale;
		int ls = W.vrows[vr].line_start;
		int style = W.vrows[vr].style;
		int hide_prefix = W.vrows[vr].hide_prefix;
		int prefix_len = W.vrows[vr].prefix_len;
		int indent_len = W.vrows[vr].indent_len;
		int screen_row;
		int le;
		int x_px;
		int max_px;

		max_px = W.pane_left * 8 + W.pane_width * 8;
		le = ls;
		while (le < W.len && W.buf[le] != '\n')
			le++;
		vis = screen_y - W.scroll;
		screen_row = W.text_top + vis;
		spos = 0;
		if (vis + vh > 0 && vis < W.text_rows)
		{
			i = W.vrows[vr].off0;
			col = 0;
			x_px = W.pane_left * 8;
			fg = style_fg(style);
			bg = style_bg(style);
			/* A fenced code block paints a full-width band, not just the
			 * text run, so short lines and blank lines stay inside the
			 * block chrome (issue #582). */
			if (bg != WP_BG && vis >= 0 && vis < W.text_rows)
				wp_fill_px(W.pane_left * 8, screen_row * 16,
					   W.pane_width * 8, vh * 16, bg);
			if (vis >= 0)
			{
				while (i < W.vrows[vr].off1)
				{
					unsigned chfg, chbg;
					unsigned ch;
					int skip, left, adv, on_cur;

					if (!vis_ch_at(style, hide_prefix, i, ls, indent_len,
						       prefix_len, &ch))
					{
						i++;
						continue;
					}
					skip = skip_emph_mark(i, le, style);
					if (skip)
					{
						i += skip;
						continue;
					}
					if (ch == '\r')
					{
						i++;
						continue;
					}
					left = 0;
					adv = heading_metrics(scale, ch, &left);
					if (col > 0 && x_px + adv > max_px)
						break;
					chfg = fg;
					chbg = bg;
					if (emph_at(i, ls, le, style))
						chfg = intense_fg();
					if (in_sel(i))
					{
						chfg = WP_SEL_FG;
						chbg = WP_SEL_BG;
					}
					on_cur = (vr == W.cx_vrow && col == W.cx_vcol && !W.dialog);
					if (on_cur)
					{
						chfg = bg;
						chbg = fg;
					}
					if (vis >= 0 && vis < W.text_rows)
					{
						if (scale > 1)
						{
							if (on_cur)
								wp_fill_px(x_px, screen_row * 16,
									   adv, scale * 16, chbg);
							wp_glyph_px(x_px - left, screen_row * 16,
								    (unsigned)ch, chfg, chbg,
								    scale);
						}
						else
							wp_glyph(W.pane_left + col, screen_row,
								 (unsigned)ch, chfg, chbg, scale);
					}
					if (spos < W.pane_width)
						srow[spos++] = (char)ch;
					x_px += adv;
					col++;
					i++;
				}
				if (vr == W.cx_vrow && W.cx_vcol >= col && !W.dialog)
				{
					if (vis >= 0 && vis < W.text_rows)
						draw_cursor_cell(x_px, screen_row * 16, ' ',
								 fg, bg, scale);
				}
				while (spos < W.pane_width)
					srow[spos++] = ' ';
				srow[spos] = 0;
				serial_row(srow);
			}
		}
		screen_y += vh;
	}
	if (W.total_vrows == 0 && !W.dialog)
	{
		int screen_row = W.text_top;
		draw_cursor_cell(W.pane_left * 8, screen_row * 16, ' ', WP_FG, WP_BG, 1);
		serial_row("");
	}
	else if (W.cx_vrow >= W.total_vrows && !W.dialog)
	{
		int vis2 = screen_row_of_vrow(W.total_vrows) - W.scroll;
		if (vis2 >= 0 && vis2 < W.text_rows)
			draw_cursor_cell(W.pane_left * 8, (W.text_top + vis2) * 16, ' ',
					 WP_FG, WP_BG, 1);
	}
}

static void draw_status(void)
{
	char chip[160];
	char right[64];
	char *p;
	int row = W.vid_rows - 1;
	int words = count_words();
	const char *name = wp_basename();
	unsigned cbg = dlg_bg();

	wp_fill_row(row, WP_DIM, WP_BG);
	p = chip;
	*p++ = ' ';
	*p++ = W.dirty ? '*' : ' ';
	while (*name && p < chip + sizeof(chip) - 2)
		*p++ = *name++;
	*p++ = ' ';
	*p = 0;
	wp_puts(0, row, chip, WP_FG, cbg);
	p = right;
	p = put_uint(p, words);
	strncpy(p, " words ", sizeof(right) - (size_t)(p - right) - 1);
	right[sizeof(right) - 1] = 0;
	wp_puts(W.vid_cols - (int)strlen(right), row, right, WP_DIM, WP_BG);
	serial_row(chip);
	serial_row(right);
}

static void wp_smooth_scroll(int from, int to)
{
	int delta, x, y, w, h, pixels, step, done, dir, d;

	if (!G.plat || !G.plat->tui_scroll || from == to)
		return;
	delta = to - from;
	if (delta > 3 || delta < -3)
		return;
	x = W.pane_left * 8;
	y = W.text_top * 16;
	w = W.pane_width * 8;
	h = W.text_rows * 16;
	if (w < 8 || h < 16)
		return;
	dir = delta > 0 ? 1 : -1;
	pixels = (delta < 0 ? -delta : delta) * 16;
	step = 4;
	done = 0;
	while (done < pixels)
	{
		d = step;
		if (d > pixels - done)
			d = pixels - done;
		G.plat->tui_scroll(x, y, w, h, dir * d, WP_BG);
		done += d;
	}
}

static void wp_redraw(void)
{
	int row, chrome;
	int old_scroll;

	if (!G.plat || !G.plat->tui_glyph || !G.plat->tui_present)
		return;
	wp_layout_geom();
	chrome = wp_chrome();
	old_scroll = W.last_scroll;
	wp_build_layout();
	ensure_scroll();
	if (!W.dialog && !W.menu_open && old_scroll >= 0 &&
	    W.chrome_shown == chrome && W.scroll != old_scroll)
		wp_smooth_scroll(old_scroll, W.scroll);
	for (row = 0; row < W.vid_rows; row++)
		wp_fill_row(row, WP_FG, WP_BG);
	if (chrome)
		draw_menu_bar();
	if (!W.dialog)
		draw_body();
	if (W.menu_open)
		draw_dropdown();
	if (W.dialog == WP_DLG_PICK)
		draw_picker();
	else if (W.dialog == WP_DLG_RECOVER)
		draw_recover_dialog();
	else if (W.dialog)
		draw_file_dialog();
	if (chrome)
		draw_status();
	G.plat->tui_present(0, W.vid_rows * 16 - 1);
	W.last_scroll = W.scroll;
	W.chrome_shown = chrome;
	wp_serial_dump();
}

static int menu_key(char c)
{
	int n;
	const char **it;

	if (!W.menu_open)
		return 0;
	if (c == 27)
	{
		W.esc_state = WP_ESC_GOT;
		W.esc_at = mmb_now_ms();
		return 1;
	}
	if (c == '\r' || c == '\n')
	{
		activate_menu();
		return 1;
	}
	menu_items(W.menu, &n);
	it = menu_items(W.menu, &n);
	if (c >= '1' && c < '1' + n)
	{
		W.menu_item = c - '1';
		activate_menu();
		return 1;
	}
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 32);
	{
		int i;
		for (i = 0; i < n; i++)
		{
			const char *s = it[i];
			if (s[0] == c || (s[0] >= 'a' && s[0] <= 'z' && s[0] - 32 == c))
			{
				W.menu_item = i;
				activate_menu();
				return 1;
			}
		}
	}
	return 1;
}

static const char *wp_feed_inner(char c)
{
	G.outn = 0;
	G.out[0] = 0;
	if (!W.active)
		return G.out;
	if (W.alt_pend)
	{
		W.alt_pend = 0;
		if (handle_alt(c))
		{
			if (W.active)
				wp_redraw();
			return G.out;
		}
	}
	if (c == 1)
	{
		W.alt_pend = 1;
		wp_redraw();
		return G.out;
	}
	if (W.esc_state)
	{
		if (handle_escape(c))
		{
			if (W.active)
				wp_redraw();
			return G.out;
		}
	}
	if (c == 27)
	{
		W.esc_state = WP_ESC_GOT;
		W.esc_at = mmb_now_ms();
		return G.out;
	}
	if (menu_key(c))
	{
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (dialog_key(c))
	{
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == 16)
	{
		open_picker();
		wp_redraw();
		return G.out;
	}
	if (c == 26) /* Ctrl+Z: shared undo chord (#532) */
	{
		wp_undo();
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == 24)
	{
		wp_leave();
		return G.out;
	}
	if (c == 3)
	{
		copy_selection();
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == 11)
	{
		cut_selection();
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == 21)
	{
		paste_clip();
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == '\t')
	{
		indent_line(1);
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == '\r' || c == '\n')
	{
		insert_newline_list();
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c == 8 || c == 127)
	{
		backspace();
		if (W.active)
			wp_redraw();
		return G.out;
	}
	if (c >= 32 && c < 127)
	{
		insert_char(c);
		if (W.active)
			wp_redraw();
		return G.out;
	}
	return G.out;
}

static const char *wp_feed(char c)
{
	const char *r;
	wp_hist_active = 1;
	wp_hist_taken = 0;
	r = wp_feed_inner(c);
	wp_hist_active = 0;
	return r;
}

void mmb_cmd_wordpad(void)
{
	char path[128];
	char canon[128];

	memset(path, 0, sizeof(path));
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		if (v.type == T_STR)
			strncpy(path, v.s, sizeof(path) - 1);
	}
	memset(&W, 0, sizeof(W));
	wp_reset_globals();
	W.active = 1;
	W.wide = 0;
	W.ndoc = 1;
	W.cur = 0;
	W.last_scroll = -1;
	W.rec_at = mmb_now_ms();
	W.rec_sig = 0;
	W.saved_mode = G.gfx.mode;
	W.saved_bits = G.gfx.bits;
	if (path[0])
	{
		canon_path(path, canon, sizeof(canon));
		strncpy(W.path, canon, sizeof(W.path) - 1);
		W.path[sizeof(W.path) - 1] = 0;
		wp_load_file(W.path);
		wp_stash();
		wp_recovery_check();
	}
	ser("[WORDPAD]\r\n");
	mmb_editor_apply_tui_palette();
	mmb_hw_cursor(0);
	if (G.plat && G.plat->write_screen)
		G.plat->write_screen("\x1b[H\x1b[J", 7);
	if (G.plat && G.plat->tui_prepare)
		G.plat->tui_prepare();
	wp_layout_geom();
	wp_redraw();
}

int mmb_in_wordpad(void)
{
	return W.active;
}

const char *mmb_wordpad_key(char c)
{
	return wp_feed(c);
}

void mmb_wordpad_poll(void)
{
	int chrome;

	if (!W.active)
		return;
	if (W.esc_state == WP_ESC_GOT &&
	    mmb_now_ms() - W.esc_at >= WP_ESC_IDLE_MS)
	{
		W.esc_state = WP_ESC_NONE;
		if (W.menu_open || W.dialog)
		{
			close_ui();
			wp_redraw();
			return;
		}
	}
	if (!W.dialog && W.path[0] && W.dirty &&
	    mmb_now_ms() - W.rec_at >= WP_AUTOSAVE_MS)
	{
		unsigned s = wp_sig();

		W.rec_at = mmb_now_ms();
		if (s != W.rec_sig)
		{
			wp_rec_write();
			W.rec_sig = s;
		}
	}
	chrome = wp_chrome();
	if (chrome != W.chrome_shown)
		wp_redraw();
}
