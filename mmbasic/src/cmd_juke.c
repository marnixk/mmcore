#include "mmb_priv.h"
#include "frontend.h"

/*
 * JUKE: a first-party retro music player (ScreamTracker-era feel).
 *
 *   JUKE "file"    play one file
 *   JUKE "folder"  queue supported files in a folder and its subfolders
 *   JUKE           queue the current directory
 *
 * Player only: no pattern or sample editing. Supported formats are the ones
 * the audio engine already decodes: MP3, MOD, XM, S3M and WAV. The mixer runs
 * from mmb_poll, so leaving JUKE for another screen keeps the music going; the
 * queue also keeps advancing while JUKE is in the background.
 *
 * UI state is per virtual console; the playback queue is global because there
 * is a single audio engine.
 */

/* JUKE renders in whatever video mode the console is already in. The layout is
 * derived from that framebuffer with a single percent scale relative to the
 * 960x540 design, so a MODE 19/20 (or any other) screen gets a proportional
 * player instead of a 960x540 one stretched or letterboxed into it. */
#define JUKE_REF_W     960
#define JUKE_REF_H     540
#define JUKE_PAGE_A    0
#define JUKE_PAGE_B    2
#define JUKE_FRAME_MS  33
#define JUKE_PATH_MAX   160
#define JUKE_TITLE_MAX  40
#define JUKE_QUEUE_INIT 32      /* queue slots allocated up front        */
#define JUKE_DIR_INIT   32      /* directory listing grows by doubling   */
#define JUKE_DIR_MAX    4096    /* soft limit: entries kept per directory */
#define JUKE_SCAN_INIT  16      /* scan stack grows by doubling          */
#define JUKE_SCOPE_N    64     /* scope samples drawn per frame      */
#define JUKE_SCOPE_HIST 6      /* ghost history frames (AFK-style)   */
#define JUKE_MID_Y      100    /* oscilloscope inset top (at scale 100) */
#define JUKE_SCOPE_H     52    /* oscilloscope inset height          */
#define JUKE_LIST_ROW    18
#define JUKE_LINE_H      16    /* fixed pixel-font line height (#1118)  */
#define JUKE_LIST_SEL   0x3A4650u
#define JUKE_ESC_IDLE_MS 60

typedef struct {
	int active;
	int saved_mode, saved_bits;
	int saved_write_page, saved_display_page, saved_write_fb;
	int saved_font_scale;
	int w, h;
	int s;         /* layout scale percent: 100 matches the 960x540 design */
	int front;
	int muted;
	int vol_saved;
	unsigned last_ms;
	float peak[MMB_AUDIO_BANDS];
	short scope_hist[JUKE_SCOPE_HIST][JUKE_SCOPE_N];
	int scope_head;
	uint32_t *logo;                /* decoded graffiti wordmark (#914) */
	int logo_w, logo_h;
	unsigned col_bg, col_panel, col_panel2, col_track, col_text, col_dim;
	unsigned col_bar_lo, col_bar_hi, col_scan, col_peak;
	unsigned col_scope_lo, col_vol, col_base;
	int list_on;
	int sel;
	int sel_item;
	int list_top;
	int esc;
	unsigned esc_at;
	int help_on;   /* `?` shortcut modal is up (#1114) */
} juke_ui;

typedef struct {
	int active;
	int n;
	int cap;       /* allocated slots in the parallel arrays below */
	int cur;
	int shuffle;
	int owner;     /* virtual console that started this queue (#805) */
	int *order;    /* play order: item index at each queue slot */
	char dir[JUKE_PATH_MAX];
	char **item;   /* each entry is a malloc'd full path */
	char **title;  /* lazily allocated per-track metadata title */
	unsigned char *titled;
} juke_queue;

static juke_ui s_ui[MMB_MAX_CONSOLES];
#define U (s_ui[g_console])
/* Scale a design-space (960x540) coordinate to the current framebuffer. */
#define JS(v) ((v) * U.s / 100)
static juke_queue s_q;
static unsigned s_rand = 0x9E3779B9u;

/* Header line and panel anchors. The title/path offsets are scaled by the
 * layout percent but the pixel font is a fixed 16px at every scale, so below
 * ~89% the scaled gap between the two header lines drops under one line and
 * the path draws over the title (#1118). Clamp each line to at least one fixed
 * line below the one above, then push the scope (and hence the list) below the
 * header when it grows. At scale 100 these reduce to the design coordinates. */
static int juke_title_y(void)
{
	return JS(60);
}

static int juke_path_y(void)
{
	int y = JS(78);
	int floor = juke_title_y() + JUKE_LINE_H;
	return y < floor ? floor : y;
}

static int juke_scope_y(void)
{
	int gap = JS(6);
	int y = JS(JUKE_MID_Y);
	int floor;

	if (gap < 4)
		gap = 4;
	floor = juke_path_y() + JUKE_LINE_H + gap;
	return y < floor ? floor : y;
}

static int juke_list_y(void)
{
	return juke_scope_y() + JS(JUKE_SCOPE_H) + JS(8);
}

/* Footer rows are anchored to the bottom with a fixed 16px font, so the top
 * row (the transport legend) sits at a fixed offset above the margin while the
 * playlist panel bottom is a scaled design coordinate. Below ~80% the scaled
 * panel bottom crossed the legend, so both `juke_paint()` and the list panel
 * share this geometry and the panel is clamped above `legend_y` (#1123). */
typedef struct
{
	int fy;       /* top of the volume row    */
	int cy;       /* chip / bar top           */
	int ly;       /* text baseline row        */
	int legend_y; /* transport legend row top */
} juke_footer_geom;

static juke_footer_geom juke_footer_geometry(int h)
{
	juke_footer_geom g;
	int line = JUKE_LINE_H;
	int bar_h = JS(14);
	int vol_row_h, pad, gap;

	if (bar_h < 8)
		bar_h = 8;
	vol_row_h = bar_h > line ? bar_h : line;
	pad = JS(8);
	if (pad < 4)
		pad = 4;
	gap = JS(8);
	if (gap < 6)
		gap = 6;
	g.fy = h - pad - vol_row_h;
	g.cy = g.fy + (vol_row_h - bar_h) / 2;
	g.ly = g.fy + (vol_row_h - line) / 2;
	g.legend_y = g.fy - gap - line;
	return g;
}

/* Small xorshift PRNG; no libc rand() on bare metal. */
static unsigned juke_rand(void)
{
	s_rand ^= s_rand << 13;
	s_rand ^= s_rand >> 17;
	s_rand ^= s_rand << 5;
	return s_rand;
}

/* Track name at play-order position pos ("" when out of range). */
static const char *juke_track(int pos)
{
	int idx;
	if (pos < 0 || pos >= s_q.n)
		return "";
	idx = s_q.order[pos];
	if (idx < 0 || idx >= s_q.n)
		return "";
	return s_q.item[idx];
}

static void juke_set_sel(int pos)
{
	if (s_q.n <= 0)
	{
		U.sel = 0;
		U.sel_item = -1;
		return;
	}
	if (pos < 0)
		pos = 0;
	if (pos >= s_q.n)
		pos = s_q.n - 1;
	U.sel = pos;
	U.sel_item = s_q.order[pos];
}

static void juke_sel_move(int delta)
{
	if (!U.list_on || s_q.n <= 0)
		return;
	juke_set_sel(U.sel + delta);
}

/* Selection is an item, not a slot, so a shuffle keeps the same row's track. */
static void juke_rebind_selection(void)
{
	int c;

	for (c = 0; c < MMB_MAX_CONSOLES; c++)
	{
		int item = s_ui[c].sel_item;
		int i;

		if (!s_ui[c].active || item < 0)
			continue;
		s_ui[c].sel = 0;
		for (i = 0; i < s_q.n; i++)
		{
			if (s_q.order[i] == item)
			{
				s_ui[c].sel = i;
				break;
			}
		}
	}
}

/* Toggle shuffle. The currently playing track stays put; the rest is
 * reordered so NEXT/PREV follow the shuffled order. */
static void juke_set_shuffle(int on)
{
	int i, j;
	on = on ? 1 : 0;
	if (on == s_q.shuffle || s_q.n <= 1)
	{
		s_q.shuffle = on && s_q.n > 1;
		return;
	}
	j = s_q.cur >= 0 && s_q.cur < s_q.n ? s_q.order[s_q.cur] : -1;
	if (on)
	{
		for (i = 0; i < s_q.n; i++)
			s_q.order[i] = i;
		for (i = s_q.n - 1; i > 0; i--)
		{
			int k = (int)(juke_rand() % (unsigned)(i + 1));
			int t = s_q.order[i];
			s_q.order[i] = s_q.order[k];
			s_q.order[k] = t;
		}
		if (j >= 0)
		{
			for (i = 0; i < s_q.n; i++)
			{
				if (s_q.order[i] == j)
				{
					s_q.order[i] = s_q.order[s_q.cur];
					s_q.order[s_q.cur] = j;
					break;
				}
			}
		}
	}
	else
	{
		for (i = 0; i < s_q.n; i++)
			s_q.order[i] = i;
		if (j >= 0)
			s_q.cur = j;
	}
	juke_rebind_selection();
	s_q.shuffle = on;
}

/* ---- colour helpers --------------------------------------------------- *
 * JUKE owns a fixed cool-grey palette with the graffiti logo's lime accent.
 * It deliberately ignores OPTION EDIT THEME so the player looks the same
 * under every system theme; because nothing here touches the TUI palette,
 * leaving JUKE restores the user's theme untouched. */

/* Component-wise blend of a and b by t in [0,1]. */
static unsigned juke_mix(unsigned a, unsigned b, float t)
{
	int ar = (int)((a >> 16) & 255), ag = (int)((a >> 8) & 255), ab = (int)(a & 255);
	int br = (int)((b >> 16) & 255), bg = (int)((b >> 8) & 255), bb = (int)(b & 255);
	int r = ar + (int)((float)(br - ar) * t);
	int g = ag + (int)((float)(bg - ag) * t);
	int bl = ab + (int)((float)(bb - ab) * t);
	if (r < 0) r = 0;
	if (r > 255) r = 255;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (bl < 0) bl = 0;
	if (bl > 255) bl = 255;
	return (unsigned)((r << 16) | (g << 8) | bl);
}

/* Black base -> lime tip, for the per-bar gradient (#1055, no grey mid). */
static unsigned juke_grad(float t)
{
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	return juke_mix(U.col_bar_lo, U.col_bar_hi, t);
}

/* Muted graffiti accents: magenta -> cyan -> lime, for the VOL fill edge. */
static unsigned juke_logo_grad(float t)
{
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (t < 0.5f)
		return juke_mix(0x9736B3u, 0x2DB7B7u, t * 2.0f);
	return juke_mix(0x2DB7B7u, 0x5BBA78u, (t - 0.5f) * 2.0f);
}

static void juke_load_colours(void)
{
	/* JUKE renders at 32-bit precision so the surface reaches the panel's
	 * native depth: neutral greys stay neutral, and the bar/ghost gradients
	 * do not band into the few levels RGB332 allows. */
	U.col_bg = 0x000000u;       /* black canvas          */
	U.col_panel = 0x08090Au;    /* scope panel fill      */
	U.col_panel2 = 0x373A3Eu;   /* muted grey border     */
	U.col_track = 0x1A1D20u;    /* empty VOL / chip fill */
	U.col_text = 0xFFFFFFu;     /* plain white           */
	U.col_dim = 0x6A95ACu;      /* cool steel blue       */
	U.col_bar_lo = 0x000000u;   /* black bar base        */
	U.col_bar_hi = 0x9DEE5Eu;   /* lime bar tip          */
	U.col_scan = 0xBEE65Au;     /* bright scope trace    */
	U.col_peak = 0xC8E664u;     /* light lime cap        */
	U.col_scope_lo = 0x2A3038u; /* cool dark ghost       */
	U.col_vol = 0xB4BCC2u;      /* VOL body grey         */
	U.col_base = 0x464A50u;     /* spectrum baseline     */
}

/* ---- paths / queue ---------------------------------------------------- */

/* Circle's util.h has strchr but not strrchr; find the last occurrence. */
static const char *juke_last(const char *p, char ch)
{
	const char *found = 0;
	for (; *p; p++)
		if (*p == ch)
			found = p;
	return found;
}

static const char *juke_ext(const char *p)
{
	const char *dot = juke_last(p, '.');
	return dot ? dot + 1 : "";
}

static int juke_ext_ok(const char *name)
{
	const char *e = juke_ext(name);
	return mmb_keyword_eq(e, "MP3") || mmb_keyword_eq(e, "MOD") ||
	       mmb_keyword_eq(e, "XM") || mmb_keyword_eq(e, "S3M") ||
	       mmb_keyword_eq(e, "WAV");
}

static const char *juke_basename(const char *p)
{
	const char *slash = juke_last(p, '/');
	return slash ? slash + 1 : p;
}

/* Path under the queued folder, so nested tracks stay distinct. */
static const char *juke_dispname(const char *p)
{
	int n;

	if (!p)
		return "";
	if (s_q.dir[0])
	{
		n = (int)strlen(s_q.dir);
		if (n > 0 && strncmp(p, s_q.dir, (size_t)n) == 0 && p[n] == '/')
			return p + n + 1;
	}
	return juke_basename(p);
}

/* Metadata title when one is cached, otherwise the path under the folder. */
static const char *juke_row_title(int pos)
{
	int idx;
	const char *path;

	if (pos < 0 || pos >= s_q.n)
		return "";
	idx = s_q.order[pos];
	if (idx < 0 || idx >= s_q.n)
		return "";
	path = s_q.item[idx];
	if (!s_q.titled[idx])
	{
		if (!s_q.title[idx])
		{
			s_q.title[idx] = G.plat->alloc(JUKE_TITLE_MAX);
			if (!s_q.title[idx])
				return juke_dispname(path);
		}
		s_q.titled[idx] = 1;
		s_q.title[idx][0] = 0;
		if (s_q.cur == pos && g_audio.name[0] &&
		    mmb_keyword_eq(g_audio.name, path))
		{
			if (!mmb_audio_title(s_q.title[idx], JUKE_TITLE_MAX))
				mmb_media_title(path, s_q.title[idx], JUKE_TITLE_MAX);
		}
		else
			mmb_media_title(path, s_q.title[idx], JUKE_TITLE_MAX);
	}
	if (s_q.title[idx][0])
		return s_q.title[idx];
	return juke_dispname(path);
}

static void juke_join(char *dst, int dstsz, const char *dir, const char *name)
{
	int n = 0;
	const char *p;
	if (dir && dir[0])
	{
		for (p = dir; *p && n < dstsz - 1; p++)
			dst[n++] = *p;
		if (n > 0 && dst[n - 1] != '/' && n < dstsz - 1)
			dst[n++] = '/';
	}
	if (name)
		for (p = name; *p && n < dstsz - 1; p++)
			dst[n++] = *p;
	dst[n] = 0;
}

/* Release every path, title, and index the queue owns. The queue is global
 * and outlives individual JUKE screens, so this runs only when it is rebuilt. */
static void juke_queue_free(void)
{
	int i;

	if (!G.plat || !G.plat->free)
		return;
	if (s_q.item)
	{
		for (i = 0; i < s_q.n; i++)
			if (s_q.item[i])
				G.plat->free(s_q.item[i]);
		G.plat->free(s_q.item);
	}
	if (s_q.title)
	{
		for (i = 0; i < s_q.n; i++)
			if (s_q.title[i])
				G.plat->free(s_q.title[i]);
		G.plat->free(s_q.title);
	}
	if (s_q.order)
		G.plat->free(s_q.order);
	if (s_q.titled)
		G.plat->free(s_q.titled);
	s_q.order = 0;
	s_q.item = 0;
	s_q.title = 0;
	s_q.titled = 0;
	s_q.n = 0;
	s_q.cap = 0;
}

/* Grow the queue's parallel arrays to at least `need` slots. Doubling keeps
 * the number of reallocations small; individual paths are allocated to their
 * exact length so a large library costs only what it uses. */
static int juke_queue_reserve(int need)
{
	int ncap, i;
	int *order;
	char **item, **title;
	unsigned char *titled;

	if (need <= s_q.cap)
		return 0;
	if (!G.plat || !G.plat->alloc || !G.plat->free)
		return -1;
	ncap = s_q.cap ? s_q.cap : JUKE_QUEUE_INIT;
	while (ncap < need)
		ncap *= 2;
	order = G.plat->alloc((unsigned)ncap * sizeof(*order));
	item = G.plat->alloc((unsigned)ncap * sizeof(*item));
	title = G.plat->alloc((unsigned)ncap * sizeof(*title));
	titled = G.plat->alloc((unsigned)ncap);
	if (!order || !item || !title || !titled)
	{
		if (order) G.plat->free(order);
		if (item) G.plat->free(item);
		if (title) G.plat->free(title);
		if (titled) G.plat->free(titled);
		return -1;
	}
	for (i = 0; i < s_q.n; i++)
	{
		order[i] = s_q.order[i];
		item[i] = s_q.item[i];
		title[i] = s_q.title[i];
		titled[i] = s_q.titled[i];
	}
	for (i = s_q.n; i < ncap; i++)
	{
		item[i] = 0;
		title[i] = 0;
		titled[i] = 0;
	}
	if (s_q.order) G.plat->free(s_q.order);
	if (s_q.item) G.plat->free(s_q.item);
	if (s_q.title) G.plat->free(s_q.title);
	if (s_q.titled) G.plat->free(s_q.titled);
	s_q.order = order;
	s_q.item = item;
	s_q.title = title;
	s_q.titled = titled;
	s_q.cap = ncap;
	return 0;
}

static int juke_add_file(const char *dir, const char *name)
{
	char path[JUKE_PATH_MAX];
	int len;
	char *p;

	juke_join(path, sizeof(path), dir, name);
	if (!path[0])
		return -1;
	if (juke_queue_reserve(s_q.n + 1) != 0)
		return -1;
	len = (int)strlen(path);
	p = G.plat->alloc((unsigned)len + 1);
	if (!p)
		return -1;
	memcpy(p, path, (size_t)len + 1);
	s_q.item[s_q.n] = p;
	s_q.title[s_q.n] = 0;
	s_q.titled[s_q.n] = 0;
	s_q.order[s_q.n] = s_q.n;
	s_q.n++;
	return 0;
}

/* One sorted listing of a whole directory, growing the buffer until the
 * backend reports no truncation. JUKE_DIR_MAX bounds the transient buffer
 * (a directory with more entries keeps its first JUKE_DIR_MAX, sorted). */
static mmb_dirent *juke_list_all(const char *path, int *out_n)
{
	int cap = JUKE_DIR_INIT;

	*out_n = 0;
	for (;;)
	{
		mmb_dirent *ents = G.plat->alloc(sizeof(*ents) * (unsigned)cap);
		int n, trunc = 0, at_max = cap >= JUKE_DIR_MAX;

		if (!ents)
			return 0;
		n = mmb_vfs_list_entries(path, ents, cap, &trunc);
		if (n < 0)
		{
			G.plat->free(ents);
			return 0;
		}
		if (!trunc || at_max)
		{
			*out_n = n;
			return ents;
		}
		G.plat->free(ents);
		cap *= 2;
		if (cap > JUKE_DIR_MAX)
			cap = JUKE_DIR_MAX;
	}
}

typedef struct {
	char path[JUKE_PATH_MAX];
	int pass;
	int depth;
} juke_scan;

/* Push a scan frame, growing the stack by doubling when it is full. */
static int juke_scan_push(juke_scan **stack, int *sp, int *cap,
			  const char *path, int pass, int depth)
{
	if (*sp >= *cap)
	{
		int ncap = *cap * 2;
		juke_scan *ns = G.plat->alloc(sizeof(**stack) * (unsigned)ncap);
		if (!ns)
			return -1;
		memcpy(ns, *stack, sizeof(**stack) * (size_t)*sp);
		G.plat->free(*stack);
		*stack = ns;
		*cap = ncap;
	}
	strncpy((*stack)[*sp].path, path, JUKE_PATH_MAX - 1);
	(*stack)[*sp].path[JUKE_PATH_MAX - 1] = 0;
	(*stack)[*sp].pass = pass;
	(*stack)[*sp].depth = depth;
	(*sp)++;
	return 0;
}

/* Folders A-Z, then the files beside them. Subfolders are queued before
 * those files. The stack and each listing grow on demand, so a large or
 * deep tree is not capped by a fixed array. Recursion depth is naturally
 * bounded by JUKE_PATH_MAX (each level adds at least "/x"). */
static int juke_scan_tree(const char *root)
{
	juke_scan *stack;
	int sp = 0, cap = JUKE_SCAN_INIT, failed = 0;

	if (!G.plat || !G.plat->alloc || !G.plat->free)
		return -1;
	stack = G.plat->alloc(sizeof(*stack) * (unsigned)cap);
	if (!stack)
		return -1;
	if (juke_scan_push(&stack, &sp, &cap, root, 0, 0) != 0)
	{
		G.plat->free(stack);
		return -1;
	}
	while (sp > 0 && !failed)
	{
		juke_scan cur = stack[--sp];
		mmb_dirent *ents;
		int n = 0, i;

		ents = juke_list_all(cur.path, &n);
		if (!ents)
		{
			if (cur.pass == 0 && cur.depth == 0)
				failed = 1;
			continue;
		}
		if (cur.pass == 0)
		{
			/* The directory's own files are queued after its subtrees,
			 * so re-push it as a pass-1 frame under the subfolders. */
			if (juke_scan_push(&stack, &sp, &cap, cur.path, 1,
					   cur.depth) != 0)
			{
				G.plat->free(ents);
				failed = 1;
				break;
			}
			for (i = n - 1; i >= 0; i--)
			{
				char sub[JUKE_PATH_MAX];
				if (!ents[i].is_dir || !ents[i].name[0])
					continue;
				juke_join(sub, sizeof(sub), cur.path,
					  ents[i].name);
				if (juke_scan_push(&stack, &sp, &cap, sub, 0,
						   cur.depth + 1) != 0)
				{
					failed = 1;
					break;
				}
			}
		}
		else
		{
			for (i = 0; i < n; i++)
			{
				if (ents[i].is_dir || !juke_ext_ok(ents[i].name))
					continue;
				if (juke_add_file(cur.path, ents[i].name) != 0)
					break;
			}
		}
		G.plat->free(ents);
	}
	G.plat->free(stack);
	return failed ? -1 : 0;
}

static int juke_build_queue(const char *spec)
{
	juke_queue_free();
	s_q.cur = -1;
	s_q.shuffle = 0;
	s_q.dir[0] = 0;
	if (mmb_vfs_isdir(spec))
	{
		strncpy(s_q.dir, spec, sizeof(s_q.dir) - 1);
		s_q.dir[sizeof(s_q.dir) - 1] = 0;
		if (juke_scan_tree(spec) != 0)
		{
			juke_queue_free();
			return -1;
		}
	}
	else
	{
		if (!juke_ext_ok(spec) || !mmb_vfs_exists(spec))
			return -1;
		if (juke_add_file("", spec) != 0)
		{
			juke_queue_free();
			return -1;
		}
		strncpy(s_q.dir, spec, sizeof(s_q.dir) - 1);
		s_q.dir[sizeof(s_q.dir) - 1] = 0;
	}
	{
		int i;
		s_rand = (unsigned)mmb_now_ms() * 2654435761u + 1u;
		for (i = 0; i < s_q.n; i++)
			s_q.order[i] = i;
	}
	return s_q.n > 0 ? 0 : -1;
}

static int juke_play_path(const char *p)
{
	if (mmb_keyword_eq(juke_ext(p), "MP3"))
		return mmb_play_mp3(p);
	if (mmb_keyword_eq(juke_ext(p), "MOD"))
		return mmb_play_mod(p);
	if (mmb_keyword_eq(juke_ext(p), "XM"))
		return mmb_play_xm(p);
	if (mmb_keyword_eq(juke_ext(p), "S3M"))
		return mmb_play_s3m(p);
	if (mmb_keyword_eq(juke_ext(p), "WAV"))
		return mmb_play_wav(p);
	return -1;
}

static int juke_start(int idx)
{
	const char *p;
	int vl, vr;

	if (s_q.n <= 0)
		return -1;
	if (idx < 0)
		idx = s_q.n - 1;
	if (idx >= s_q.n)
		idx = 0;
	p = juke_track(idx);
	if (!p || !p[0])
		return -1;
	/* play_begin resets the gain to 100 and queues its preroll right away.
	 * Arm JUKE's own gain first so that first buffer is already scaled (#1113);
	 * without this the very start of every track (and each skip) played at 100
	 * for a few ms. The arm is consumed by play_begin, or dropped on failure. */
	vl = g_audio.vol_l;
	vr = g_audio.vol_r;
	mmb_play_set_begin_vol(vl, vr);
	if (juke_play_path(p) != 0)
	{
		mmb_play_set_begin_vol(-1, -1);
		return -1;
	}
	mmb_play_set_begin_vol(-1, -1);
	/* The queue runs in the background on its own console: a track change
	 * driven by the host poll must not re-attribute the engine to whichever
	 * console happens to be active (#805). */
	g_audio.owner = s_q.owner;
	g_audio.vol_l = vl;
	g_audio.vol_r = vr;
	s_q.cur = idx;
	s_q.active = 1;
	return 0;
}

static void juke_advance(void)
{
	if (s_q.n <= 0)
	{
		s_q.active = 0;
		return;
	}
	/* A single file plays once; a folder queue wraps around. */
	if (s_q.n == 1)
	{
		s_q.active = 0;
		return;
	}
	(void)juke_start(s_q.cur + 1);
}

/* Queue management is global: it keeps running while JUKE is backgrounded. */
static void juke_manage(void)
{
	if (!s_q.active)
		return;
	if (g_audio.playing)
	{
		if (s_q.cur >= 0 &&
		    !mmb_keyword_eq(g_audio.name, juke_track(s_q.cur)))
			s_q.active = 0; /* some other PLAY took the engine */
		return;
	}
	if (g_audio.paused)
		return;
	if (mmb_play_take_ended())
		juke_advance();
}

/* #858: background queue advance. Registered for the queue's owning console
 * while a queue is live; the yield registry rate-limits it to ~1 Hz so it
 * stays cheap. It runs in the owner's interpreter context so a track change
 * keeps that console's ownership (see juke_start). */
static void juke_yield(int console, void *ctx)
{
	(void)ctx;
	if (console != s_q.owner)
		return;
	if (mmb_bg_console_enter(console))
	{
		juke_manage();
		mmb_bg_console_leave();
	}
	else
		juke_manage();
	if (!s_q.active)
		mmb_yield_remove(juke_yield);
}

/* ---- drawing ---------------------------------------------------------- */

static void juke_text(int x, int y, const char *s, unsigned col, int scale)
{
	int save = G.gfx.font_scale;
	if (scale < 1)
		scale = 1;
	G.gfx.font_scale = scale;
	mmb_gfx_text(x, y, s, col);
	G.gfx.font_scale = save;
}

/* Decode the graffiti wordmark once per JUKE session (#914). It mirrors the
 * startup_logo() path: A:/juke-logo.png is a build-time ramdisk asset. */
static void juke_load_logo(void)
{
	const unsigned char *file = 0;
	unsigned n = 0;
	uint32_t *pix = 0;
	int w = 0, h = 0;

	U.logo = 0;
	U.logo_w = U.logo_h = 0;
	if (!G.plat)
		return;
	if (mmb_vfs_read_ptr("A:/juke-logo.png", &file, &n) != 0 || !file || !n)
		return;
	if (mmb_png_decode_rgba(file, n, &pix, &w, &h) != 0 || !pix)
		return;
	U.logo = pix;
	U.logo_w = w;
	U.logo_h = h;
}

/* Draw the wordmark scaled by the layout percent. Scaling it with everything
 * else keeps it clear of the title/path below in low modes: an unscaled
 * 114x50 logo collides with the JS(60) title once the scale drops (#1115). */
static void juke_draw_logo(int x0, int y0)
{
	int dw, dh, dx, dy;

	if (!U.logo)
		return;
	dw = U.logo_w * U.s / 100;
	dh = U.logo_h * U.s / 100;
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	for (dy = 0; dy < dh; dy++)
	{
		int sy = (int)((long)dy * U.logo_h / dh);
		if (sy >= U.logo_h)
			sy = U.logo_h - 1;
		for (dx = 0; dx < dw; dx++)
		{
			int sx = (int)((long)dx * U.logo_w / dw);
			uint32_t c;
			if (sx >= U.logo_w)
				sx = U.logo_w - 1;
			c = U.logo[sy * U.logo_w + sx];
			if (!(c >> 24))
				continue;
			mmb_gfx_plot(x0 + dx, y0 + dy, c & 0xFFFFFFu);
		}
	}
}

/* Draw text clipped to end before maxx, cutting long titles/paths to an
 * ellipsis so they never run into the logo or off the screen (#1115). */
static void juke_text_clip(int x, int y, const char *s, unsigned col, int maxx)
{
	char buf[JUKE_PATH_MAX + 8];
	int maxc = (maxx - x) / 8; /* 8px glyph cell at scale 1 */
	size_t n;

	if (!s)
		return;
	if (maxc < 1)
		maxc = 1;
	n = strlen(s);
	if ((int)n <= maxc)
	{
		juke_text(x, y, s, col, 1);
		return;
	}
	if (maxc > (int)sizeof(buf) - 1)
		maxc = (int)sizeof(buf) - 1;
	if (maxc >= 3)
	{
		size_t keep = (size_t)(maxc - 3);
		if (keep > n)
			keep = n;
		memcpy(buf, s, keep);
		buf[keep] = '.';
		buf[keep + 1] = '.';
		buf[keep + 2] = '.';
		buf[keep + 3] = 0;
	}
	else
	{
		memcpy(buf, s, (size_t)maxc);
		buf[maxc] = 0;
	}
	juke_text(x, y, buf, col, 1);
}

static void juke_paint_list(int w, int h)
{
	int margin = JS(8);
	int y0 = juke_list_y();
	int y1 = h - JS(56);
	int footer_top = juke_footer_geometry(h).legend_y;
	int bw = w - JS(16);
	int row = JS(JUKE_LIST_ROW);
	int sel_h;
	int rows, top, vis;
	char buf[JUKE_PATH_MAX + 8];

	/* The list row can never be shorter than the fixed 16px font, or rows
	 * would overlap when the scale drops below the 960x540 design. */
	if (row < 16)
		row = 16;
	sel_h = row - 2;
	/* The panel bottom is a scaled design coordinate but the footer legend
	 * is anchored to the margin, so keep the frame above it (#1123). */
	if (y1 > footer_top)
		y1 = footer_top;
	if (y1 < y0 + JS(24))
		y1 = y0 + JS(24);
	mmb_gfx_fill_rect(margin, y0, bw, 1, U.col_panel2);
	mmb_gfx_fill_rect(margin, y1 - 1, bw, 1, U.col_panel2);
	mmb_gfx_fill_rect(margin, y0, 1, y1 - y0, U.col_panel2);
	mmb_gfx_fill_rect(margin + bw - 1, y0, 1, y1 - y0, U.col_panel2);
	mmb_gfx_fill_rect(margin + 1, y0 + 1, bw - 2, y1 - y0 - 2, U.col_panel);
	if (s_q.n <= 0)
	{
		juke_text(JS(22), y0 + JS(4), "(empty)", U.col_dim, 1);
		return;
	}
	if (U.sel < 0 || U.sel >= s_q.n)
		juke_set_sel(s_q.cur >= 0 ? s_q.cur : 0);
	rows = (y1 - y0 - JS(8)) / row;
	if (rows < 1)
		rows = 1;
	if (U.sel < U.list_top)
		U.list_top = U.sel;
	if (U.sel >= U.list_top + rows)
		U.list_top = U.sel - rows + 1;
	if (U.list_top < 0)
		U.list_top = 0;
	top = U.list_top;
	for (vis = 0; vis < rows; vis++)
	{
		int pos = top + vis;
		int y = y0 + JS(4) + vis * row;
		const char *name;
		int maxc;

		if (pos >= s_q.n)
			break;
		if (pos == U.sel)
			mmb_gfx_fill_rect(JS(10), y, w - JS(20), sel_h,
					  JUKE_LIST_SEL);
		if (pos == s_q.cur)
			mmb_gfx_fill_rect(JS(11), y, JS(4), sel_h, U.col_bar_hi);
		name = juke_row_title(pos);
		maxc = (w - JS(48)) / 8;
		if (maxc < 8)
			maxc = 8;
		if (maxc > (int)sizeof(buf) - 1)
			maxc = (int)sizeof(buf) - 1;
		sprintf(buf, "%2d  %s", pos + 1, name);
		if ((int)strlen(buf) > maxc)
			buf[maxc] = 0;
		juke_text(JS(22), y, buf,
			  pos == U.sel ? U.col_text : U.col_dim, 1);
	}
}

/* Oscilloscope waveform inset, shared by the visualiser and the playlist view
 * so the waveform stays visible while browsing the queue (#950). */
static void juke_paint_scope(int w)
{
	short scope[JUKE_SCOPE_N];
	int i, n;
	int px, x1, py0, py1, cy, amp, age;

	n = mmb_audio_scope(scope, JUKE_SCOPE_N);
	if (n > JUKE_SCOPE_N)
		n = JUKE_SCOPE_N;

	/* AFK-style ghost history: keep a ring of past scope frames so older
	 * traces fade and cool toward the base grey while the newest stays lime. */
	U.scope_head = (U.scope_head + 1) % JUKE_SCOPE_HIST;
	for (i = 0; i < JUKE_SCOPE_N; i++)
		U.scope_hist[U.scope_head][i] = i < n ? scope[i] : 0;

	/* Oscilloscope: its own bordered inset under the title, with a ghost
	 * history of past frames (oldest first so the newest lands on top). */
	{
		int bx = JS(8), by = juke_scope_y();
		int bw2 = w - JS(16), bh2 = JS(JUKE_SCOPE_H);
		if (bh2 < 12)
			bh2 = 12;
		mmb_gfx_fill_rect(bx, by, bw2, 1, U.col_panel2);
		mmb_gfx_fill_rect(bx, by + bh2 - 1, bw2, 1, U.col_panel2);
		mmb_gfx_fill_rect(bx, by, 1, bh2, U.col_panel2);
		mmb_gfx_fill_rect(bx + bw2 - 1, by, 1, bh2, U.col_panel2);
		mmb_gfx_fill_rect(bx + 1, by + 1, bw2 - 2, bh2 - 2, U.col_panel);
		px = bx + 2;
		x1 = bx + bw2 - 3;
		py0 = by + 3;
		py1 = by + bh2 - 4;
		cy = (py0 + py1) / 2;
		amp = (py1 - py0) / 2;
		for (age = JUKE_SCOPE_HIST - 1; age >= 0; age--)
		{
			int idx = (U.scope_head + JUKE_SCOPE_HIST - age) % JUKE_SCOPE_HIST;
			float t = (float)(JUKE_SCOPE_HIST - 1 - age) /
				  (float)(JUKE_SCOPE_HIST - 1);
			unsigned col = juke_mix(U.col_scope_lo, U.col_scan, t);
			int lx = -1, ly = 0;
			for (i = 0; i < JUKE_SCOPE_N; i++)
			{
				int xx = px + (int)((long)(x1 - px) * i / (JUKE_SCOPE_N - 1));
				int yy = cy + (int)((long)U.scope_hist[idx][i] * amp / 32768);
				if (yy < py0)
					yy = py0;
				if (yy > py1)
					yy = py1;
				if (lx >= 0)
					mmb_gfx_line(lx, ly, xx, yy, col, 1);
				lx = xx;
				ly = yy;
			}
		}
	}
}

static void juke_paint_vis(int w, int h)
{
	float bands[MMB_AUDIO_BANDS];
	int i, x0, x1, bw, gap, base, maxh, pad;

	juke_paint_scope(w);
	mmb_audio_spectrum(bands, MMB_AUDIO_BANDS);

	/* Spectrum: 24 black-to-grey-to-lime bars, no midfield guide lines. The
	 * bar field is deliberately short so the full-size wordmark still fits
	 * above it, and it is centred so the leftover width splits evenly on both
	 * sides instead of pooling to the right of the last band. */
	pad = JS(14);
	base = h - JS(71);
	maxh = base - JS(190);
	if (maxh < 24)
		maxh = 24;
	gap = JS(3);
	if (gap < 2)
		gap = 2;
	bw = (w - 2 * pad) / MMB_AUDIO_BANDS - gap;
	if (bw < 2)
		bw = 2;
	{
		int span = MMB_AUDIO_BANDS * bw + (MMB_AUDIO_BANDS - 1) * gap;
		x0 = pad + (w - 2 * pad - span) / 2;
		if (x0 < pad)
			x0 = pad;
		x1 = x0 + span;
	}

	for (i = 0; i < MMB_AUDIO_BANDS; i++)
	{
		int bx = x0 + i * (bw + gap);
		int bh = (int)(bands[i] * (float)maxh);
		int y, pycap;
		float p;

		if (bh < 2)
			bh = 2;
		if (bh > maxh)
			bh = maxh;
		/* Vertical per-bar gradient: black base -> lime tip. */
		for (y = 0; y < bh; y += 4)
		{
			float frac = (float)y / (float)bh;
			int seg_h = 4;
			if (y + seg_h > bh)
				seg_h = bh - y;
			mmb_gfx_fill_rect(bx, base - y - seg_h, bw, seg_h,
					  juke_grad(frac));
		}

		/* Peak cap falls slowly. */
		p = U.peak[i];
		if (bands[i] > p)
			p = bands[i];
		else
		{
			p -= 0.012f;
			if (p < 0.0f)
				p = 0.0f;
		}
		U.peak[i] = p;
		pycap = base - (int)(p * (float)maxh) - 2;
		if (pycap < base - maxh - 2)
			pycap = base - maxh - 2;
		mmb_gfx_fill_rect(bx, pycap, bw, 1, U.col_peak);
	}

	mmb_gfx_fill_rect(x0, base, x1 - x0, 1, U.col_base);
}

/* Modal shortcut list opened with `?` (#1114). It is painted last so it sits
 * over the live player, which keeps running underneath. Geometry is clamped to
 * the framebuffer so it stays on-screen in every mode; the panel uses JUKE's
 * own palette. */
static void juke_paint_help(int w, int h)
{
	static const struct { const char *k, *d; } rows[] = {
		{ "SPACE",   "play / pause" },
		{ "P",       "previous track" },
		{ "N",       "next track" },
		{ "L",       "playlist on / off" },
		{ "UP DOWN", "move selection" },
		{ "ENTER",   "play selected track" },
		{ "R",       "shuffle on / off" },
		{ "-  +",    "volume down / up" },
		{ "M",       "mute / unmute" },
		{ "S",       "stop the queue" },
		{ "ESC",     "close help (then quits)" },
		{ "?",       "toggle this help" },
	};
	int n = (int)(sizeof(rows) / sizeof(rows[0]));
	int i, lh = 16, pad = JS(12), gap = JS(14);
	int kw = 0, dw = 0, pw, ph, px, py, ty, maxx;

	if (pad < 8)
		pad = 8;
	if (gap < 10)
		gap = 10;
	for (i = 0; i < n; i++)
	{
		int a = (int)strlen(rows[i].k) * 8;
		int b = (int)strlen(rows[i].d) * 8;
		if (a > kw)
			kw = a;
		if (b > dw)
			dw = b;
	}
	pw = pad * 2 + kw + gap + dw;
	ph = pad * 2 + (n + 1) * lh + JS(6);
	if (pw > w - JS(8))
		pw = w - JS(8);
	if (ph > h - JS(8))
		ph = h - JS(8);
	if (pw < 1)
		pw = 1;
	if (ph < 1)
		ph = 1;
	px = (w - pw) / 2;
	py = (h - ph) / 2;
	if (px < 0)
		px = 0;
	if (py < 0)
		py = 0;

	mmb_gfx_fill_rect(px, py, pw, ph, U.col_panel);
	mmb_gfx_fill_rect(px, py, pw, 1, U.col_peak);
	mmb_gfx_fill_rect(px, py + ph - 1, pw, 1, U.col_panel2);
	mmb_gfx_fill_rect(px, py, 1, ph, U.col_panel2);
	mmb_gfx_fill_rect(px + pw - 1, py, 1, ph, U.col_panel2);

	ty = py + pad;
	juke_text(px + pad, ty, "JUKE HELP", U.col_text, 1);
	ty += lh + JS(6);
	maxx = px + pw - pad;
	for (i = 0; i < n; i++)
	{
		if (ty + lh > py + ph - pad)
			break;
		juke_text(px + pad, ty, rows[i].k, U.col_bar_hi, 1);
		juke_text_clip(px + pad + kw + gap, ty, rows[i].d, U.col_dim, maxx);
		ty += lh;
	}
}

static void juke_paint(int w, int h)
{
	int vol;
	const char *title;
	char buf[128];

	mmb_gfx_cls(U.col_bg);

	/* Header: just the full-size graffiti wordmark. The top-right status
	 * chrome (VIS/LIST/MOD #951, then N/M, SHUF, and the "more" truncation
	 * hint #965) is gone; the queue no longer truncates, so there is
	 * nothing to report there. */
	juke_draw_logo(JS(14), JS(1));

	title = s_q.cur >= 0 ? juke_row_title(s_q.cur) : "(no track)";
	juke_text_clip(JS(14), juke_title_y(), title, U.col_text, w - JS(14));
	if (s_q.cur >= 0)
	{
		const char *rel = juke_dispname(juke_track(s_q.cur));
		if (rel[0] && !mmb_keyword_eq(title, rel))
			juke_text_clip(JS(14), juke_path_y(), rel, U.col_dim, w - JS(14));
		else if (s_q.dir[0])
			juke_text_clip(JS(14), juke_path_y(), s_q.dir, U.col_dim, w - JS(14));
	}

	if (U.list_on)
	{
		juke_paint_scope(w);
		juke_paint_list(w, h);
	}
	else
		juke_paint_vis(w, h);

	/* Footer: two stacked rows — the trimmed transport legend (#1114) on top,
	 * shuffle/volume below it with a fixed gap. Low modes used to scale the
	 * only row's offsets smaller than the fixed 16px font, so the legend and
	 * the volume row collided; anchoring the rows from the bottom and gating
	 * the gap keeps them apart at every scale. */
	{
		juke_footer_geom g = juke_footer_geometry(h);

		juke_text_clip(JS(14), g.legend_y,
			       "SPACE play/pause   P prev   N next   ESC quit   ? help",
			       U.col_dim, w - JS(14));

		/* Shuffle chip: a solid swatch that lights up when shuffle is on. */
		mmb_gfx_fill_rect(JS(14), g.cy, JS(14), JS(14),
				  s_q.shuffle ? U.col_peak : U.col_track);
		juke_text(JS(34), g.ly, "SHUF",
			  s_q.shuffle ? U.col_text : U.col_dim, 1);

		/* Volume level bar: grey body with a muted logo-accent top edge. */
		vol = g_audio.vol_l;
		if (vol < 0)
			vol = 0;
		if (vol > 100)
			vol = 100;
		juke_text(JS(110), g.ly, "VOL", U.col_dim, 1);
		mmb_gfx_fill_rect(JS(146), g.cy, JS(220), JS(14), U.col_track);
		if (vol > 0)
		{
			int barw = JS(220);
			int vw = vol * barw / 100;
			int vx;
			mmb_gfx_fill_rect(JS(146), g.cy, vw, JS(14), U.col_vol);
			for (vx = 0; vx < vw; vx += 2)
			{
				int seg = vw - vx < 2 ? vw - vx : 2;
				mmb_gfx_fill_rect(JS(146) + vx, g.cy, seg, 2,
						  juke_logo_grad((float)vx /
								 (float)barw));
			}
		}
		sprintf(buf, "%3d%%%s", vol, U.muted ? " MUTE" : "");
		juke_text(JS(380), g.ly, buf, U.col_text, 1);
	}

	if (U.help_on)
		juke_paint_help(w, h);
}

static void juke_frame(void)
{
	int back, w = U.w, h = U.h;

	if (!U.active)
		return;
	if (G.plat && G.plat->wait_vsync)
		G.plat->wait_vsync();
	back = (U.front == JUKE_PAGE_A) ? JUKE_PAGE_B : JUKE_PAGE_A;
	G.gfx.write_page = back;
	G.gfx.write_fb = 0;
	juke_paint(w, h);
	G.gfx.display_page = back;
	mmb_gfx_dirty_add(0, 0, w, h);
	if (G.plat && G.plat->present_set_flip)
		G.plat->present_set_flip(1);
	mmb_gfx_present();
	U.front = back;
}

static void juke_leave(void)
{
	if (!U.active)
		return;
	U.active = 0;
	if (U.logo)
	{
		if (G.plat && G.plat->free)
			G.plat->free(U.logo);
		U.logo = 0;
	}
	if (U.saved_mode != G.gfx.mode || U.saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(U.saved_mode, U.saved_bits);
	G.gfx.font_scale = U.saved_font_scale;
	mmb_gfx_reset_console(1);
}

/* ---- entry / keys / poll --------------------------------------------- */

void mmb_cmd_juke(void)
{
	char path[JUKE_PATH_MAX];
	int have = 0;

	if (G.running)
		mmb_error("?Not available in RUN");

	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		strncpy(path, v.s, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
		have = 1;
	}

	/* g_audio volumes default to 0 until the first track starts. JUKE keeps
	 * one player volume across tracks, so seed an audible level if none is
	 * set yet; juke_start() then arms that gain for play_begin() (#1113). */
	if (g_audio.vol_l <= 0)
		g_audio.vol_l = g_audio.vol_r = 100;

	if (have)
	{
		if (juke_build_queue(path) != 0)
			mmb_error("?FILE");
		s_q.owner = g_console;
		if (juke_start(0) != 0)
			mmb_error("?FILE");
	}
	else if (!s_q.active || s_q.n == 0)
	{
		if (juke_build_queue(mmb_vfs_cwd()) != 0)
			mmb_error("?DIRECTORY");
		s_q.owner = g_console;
		if (juke_start(0) != 0)
			mmb_error("?FILE");
	}

	/* #858: the queue keeps advancing while JUKE is not the active screen.
	 * Register (or refresh) the background callback for its owner console.
	 * This is only registered while a queue is live; it does not run key
	 * handling or paint off-screen. */
	if (s_q.active)
	{
		mmb_yield_remove(juke_yield);
		mmb_yield_add(s_q.owner, juke_yield, 0, 1000);
	}

	memset(&U, 0, sizeof(U));
	U.saved_mode = G.gfx.mode;
	U.saved_bits = G.gfx.bits;
	U.saved_write_page = G.gfx.write_page;
	U.saved_display_page = G.gfx.display_page;
	U.saved_write_fb = G.gfx.write_fb;
	U.saved_font_scale = G.gfx.font_scale;
	/* #1042: draw into whatever mode the console is already in (MODE 19/20
	 * on the Chromebook) instead of forcing a fixed 960x540 mode. Leaving
	 * the mode and depth untouched here also means there is nothing to
	 * restore when JUKE exits. */
	U.w = G.gfx.w > 0 ? G.gfx.w : JUKE_REF_W;
	U.h = G.gfx.h > 0 ? G.gfx.h : JUKE_REF_H;
	{
		int sw = U.w * 100 / JUKE_REF_W;
		int sh = U.h * 100 / JUKE_REF_H;
		U.s = sw < sh ? sw : sh;
		if (U.s < 60)
			U.s = 60;
		if (U.s > 200)
			U.s = 200;
	}
	U.front = JUKE_PAGE_A;
	juke_load_colours();
	juke_load_logo();
	U.active = 1;
	U.sel_item = -1;
	if (s_q.cur >= 0 && s_q.cur < s_q.n)
		juke_set_sel(s_q.cur);
	U.last_ms = mmb_now_ms();
	juke_frame();
}

int mmb_in_juke(void)
{
	return U.active;
}

/* In-JUKE player gain: the same app gain PLAY VOLUME drives. */
static void juke_volume(int delta)
{
	int v = g_audio.vol_l + delta;
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	g_audio.vol_l = v;
	g_audio.vol_r = v;
	U.muted = 0;
}

const char *mmb_juke_key(char c)
{
	if (!U.active)
		return "";
	if (U.esc != 0)
	{
		if (U.esc == 1)
		{
			if (c == '[')
			{
				U.esc = 2;
				return "";
			}
			if (c == 'O')
			{
				U.esc = 3;
				return "";
			}
			U.esc = 0;
		}
		else if (U.esc == 3)
		{
			U.esc = 0;
			if (c == 'A')
				juke_sel_move(-1);
			else if (c == 'B')
				juke_sel_move(1);
			return "";
		}
		else
		{
			unsigned char uc = (unsigned char)c;

			if (uc >= 0x40 && uc <= 0x7e)
			{
				U.esc = 0;
				if (uc == 'A')
					juke_sel_move(-1);
				else if (uc == 'B')
					juke_sel_move(1);
			}
			return "";
		}
	}
	if (c == '?')
	{
		U.help_on = !U.help_on;
		return "";
	}
	if (c == 27)
	{
		/* Esc closes the help modal only; it must not quit JUKE while the
		 * modal is up (#1114). A second Esc after that quits as usual. */
		if (U.help_on)
		{
			U.help_on = 0;
			return "";
		}
		U.esc = 1;
		U.esc_at = mmb_now_ms();
		return "";
	}
	if (c == 3 || c == 'q' || c == 'Q')
	{
		juke_leave();
		return "";
	}
	if (c == '\r' || c == '\n')
	{
		if (U.list_on && s_q.n > 0)
			(void)juke_start(U.sel);
		else
			juke_leave();
		return "";
	}
	if (c == 'l' || c == 'L')
	{
		U.list_on = !U.list_on;
		return "";
	}
	if (c == ' ')
	{
		mmb_play_pause(!g_audio.paused);
		return "";
	}
	if (c == 'n' || c == 'N' || c == '.')
	{
		if (s_q.n > 0)
			(void)juke_start(s_q.cur + 1);
		return "";
	}
	if (c == 'p' || c == 'P' || c == ',')
	{
		if (s_q.n > 0)
			(void)juke_start(s_q.cur - 1);
		return "";
	}
	if (c == 'r' || c == 'R')
	{
		juke_set_shuffle(!s_q.shuffle);
		return "";
	}
	if (c == '+' || c == '=')
	{
		juke_volume(5);
		return "";
	}
	if (c == '-' || c == '_')
	{
		juke_volume(-5);
		return "";
	}
	if (c == 'm' || c == 'M')
	{
		if (!U.muted)
		{
			U.vol_saved = g_audio.vol_l;
			g_audio.vol_l = g_audio.vol_r = 0;
			U.muted = 1;
		}
		else
		{
			int v = U.vol_saved > 0 ? U.vol_saved : 70;
			g_audio.vol_l = g_audio.vol_r = v;
			U.muted = 0;
		}
		return "";
	}
	if (c == 's' || c == 'S')
	{
		s_q.active = 0;
		mmb_yield_remove(juke_yield);
		mmb_play_stop();
		return "";
	}
	return "";
}

void mmb_juke_close(void)
{
	if (!U.active)
		return;
	U.esc = 0;
	juke_leave();
}

void mmb_juke_poll(void)
{
	unsigned now;

	/* Queue management runs from the registered background callback
	 * (juke_yield), so it advances whether or not JUKE is the active
	 * screen. Here we only repaint the visible player. */
	if (!U.active)
		return;
	now = mmb_now_ms();
	if (U.esc == 1 && now - U.esc_at >= JUKE_ESC_IDLE_MS)
	{
		U.esc = 0;
		/* While the help modal is up, an idle Esc closes it instead of
		 * quitting the player (#1114). */
		if (U.help_on)
		{
			U.help_on = 0;
			return;
		}
		juke_leave();
		mmb_front_prompt();
		return;
	}
	if (now - U.last_ms < JUKE_FRAME_MS)
		return;
	U.last_ms = now;
	juke_frame();
}
