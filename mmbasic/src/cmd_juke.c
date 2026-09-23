#include "mmb_priv.h"

/*
 * JUKE: a first-party retro music player (ScreamTracker-era feel).
 *
 *   JUKE "file"    play one file
 *   JUKE "folder"  queue every supported file in a folder (continuous)
 *   JUKE           queue the current directory
 *
 * Player only: no pattern or sample editing. Supported formats are the ones
 * the audio engine already decodes: MP3, MOD, XM and WAV. The mixer runs from
 * mmb_poll, so leaving JUKE for another screen keeps the music going; the
 * queue also keeps advancing while JUKE is in the background.
 *
 * UI state is per virtual console; the playback queue is global because there
 * is a single audio engine.
 */

#define JUKE_MODE      12      /* 960x540, RGB444 */
#define JUKE_PAGE_A    0
#define JUKE_PAGE_B    2
#define JUKE_FRAME_MS  33
#define JUKE_MAX_QUEUE 64
#define JUKE_PATH_MAX  160
#define JUKE_LIST_MAX  2048

typedef struct {
	int active;
	int saved_mode, saved_bits;
	int saved_write_page, saved_display_page, saved_write_fb;
	int saved_font_scale;
	int w, h;
	int front;
	unsigned last_ms;
	float peak[MMB_AUDIO_BANDS];
	unsigned col_bg, col_panel, col_panel2, col_hot, col_text, col_dim;
	unsigned col_bar_lo, col_bar_mid, col_bar_hi, col_scan;
} juke_ui;

typedef struct {
	int active;
	int n;
	int cur;
	char dir[JUKE_PATH_MAX];
	char item[JUKE_MAX_QUEUE][JUKE_PATH_MAX];
} juke_queue;

static juke_ui s_ui[MMB_MAX_CONSOLES];
#define U (s_ui[g_console])
static juke_queue s_q;

/* ---- colour helpers --------------------------------------------------- */

static unsigned juke_theme(const char *name, unsigned fallback)
{
	unsigned rgb = 0;
	if (mmb_editor_theme_rgb(name, &rgb))
		return rgb & 0xFFFFFFu;
	return fallback & 0xFFFFFFu;
}

/* Lift a colour until it is clearly readable on a dark background. */
static unsigned juke_lift(unsigned c)
{
	int r = (int)((c >> 16) & 255);
	int g = (int)((c >> 8) & 255);
	int b = (int)(c & 255);
	int lum = (r * 299 + g * 587 + b * 114) / 1000;
	int min = 110, den;
	if (lum >= min)
		return c;
	den = 255 - lum + 1;
	r += (255 - r) * (min - lum) / den;
	g += (255 - g) * (min - lum) / den;
	b += (255 - b) * (min - lum) / den;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (unsigned)((r << 16) | (g << 8) | b);
}

static unsigned juke_dim(unsigned c)
{
	int r = (int)((c >> 16) & 255) / 3;
	int g = (int)((c >> 8) & 255) / 3;
	int b = (int)(c & 255) / 3;
	return (unsigned)((r << 16) | (g << 8) | b);
}

static void juke_load_colours(void)
{
	U.col_bg = juke_theme("TEXT_BG", 0x0A0A12u);
	U.col_panel = juke_theme("BORDER_BG", 0x14142Au);
	U.col_panel2 = juke_theme("MENU_BG", 0x22224Cu);
	U.col_hot = juke_lift(juke_theme("MENU_HOT", 0xFFFF55u));
	U.col_text = juke_lift(juke_theme("TEXT_FG", 0xDDDDDDu));
	U.col_dim = juke_lift(juke_theme("COMMENT_FG", 0x8A8AA0u));
	U.col_bar_lo = juke_lift(juke_theme("STRING_FG", 0x33CC55u));
	U.col_bar_mid = juke_lift(juke_theme("MENU_HOT", 0xFFFF55u));
	U.col_bar_hi = juke_lift(juke_theme("NUMBER_FG", 0xFF5533u));
	U.col_scan = juke_lift(juke_theme("SELECT_FG", 0x66DDFFu));
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
	       mmb_keyword_eq(e, "XM") || mmb_keyword_eq(e, "WAV");
}

static const char *juke_basename(const char *p)
{
	const char *slash = juke_last(p, '/');
	return slash ? slash + 1 : p;
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

static int juke_build_queue(const char *spec)
{
	char list[JUKE_LIST_MAX];
	const char *p;

	s_q.n = 0;
	s_q.cur = -1;
	s_q.dir[0] = 0;
	if (mmb_vfs_isdir(spec))
	{
		if (mmb_vfs_list(spec, list, sizeof(list)) != 0)
			return -1;
		strncpy(s_q.dir, spec, sizeof(s_q.dir) - 1);
		s_q.dir[sizeof(s_q.dir) - 1] = 0;
		p = list;
		while (*p && s_q.n < JUKE_MAX_QUEUE)
		{
			char name[JUKE_PATH_MAX];
			int l = 0;
			while (*p && *p != '\n' && l < (int)sizeof(name) - 1)
				name[l++] = *p++;
			name[l] = 0;
			while (*p == '\n')
				p++;
			if (l == 0 || name[l - 1] == '/')
				continue;
			if (!juke_ext_ok(name))
				continue;
			juke_join(s_q.item[s_q.n], JUKE_PATH_MAX, spec, name);
			s_q.n++;
		}
	}
	else
	{
		if (!juke_ext_ok(spec) || !mmb_vfs_exists(spec))
			return -1;
		strncpy(s_q.item[0], spec, JUKE_PATH_MAX - 1);
		s_q.item[0][JUKE_PATH_MAX - 1] = 0;
		strncpy(s_q.dir, spec, sizeof(s_q.dir) - 1);
		s_q.dir[sizeof(s_q.dir) - 1] = 0;
		s_q.n = 1;
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
	if (mmb_keyword_eq(juke_ext(p), "WAV"))
		return mmb_play_wav(p);
	return -1;
}

static int juke_start(int idx)
{
	if (s_q.n <= 0)
		return -1;
	if (idx < 0)
		idx = s_q.n - 1;
	if (idx >= s_q.n)
		idx = 0;
	if (juke_play_path(s_q.item[idx]) != 0)
		return -1;
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
	if (G.audio.playing)
	{
		if (s_q.cur >= 0 &&
		    !mmb_keyword_eq(G.audio.name, s_q.item[s_q.cur]))
			s_q.active = 0; /* some other PLAY took the engine */
		return;
	}
	if (G.audio.paused)
		return;
	if (mmb_play_take_ended())
		juke_advance();
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

static const char *juke_state_str(void)
{
	if (G.audio.playing && G.audio.paused)
		return "PAUSED";
	if (G.audio.playing)
		return "PLAY";
	if (s_q.active)
		return "READY";
	return "STOP";
}

static void juke_paint(int w, int h)
{
	float bands[MMB_AUDIO_BANDS];
	short scope[64];
	int i, n, x0, x1, bw, gap, base, maxh, mid;
	const char *title;
	char buf[128];

	mmb_audio_spectrum(bands, MMB_AUDIO_BANDS);
	n = mmb_audio_scope(scope, 64);

	mmb_gfx_cls(U.col_bg);

	/* Header. */
	mmb_gfx_fill_rect(0, 0, w, 46, U.col_panel);
	mmb_gfx_fill_rect(0, 46, w, 2, U.col_hot);
	juke_text(14, 8, "JUKE", U.col_hot, 2);
	{
		const char *fmt = s_q.cur >= 0 ? juke_ext(s_q.item[s_q.cur]) : "";
		sprintf(buf, "%s  %s  %d/%d", juke_state_str(), fmt,
			s_q.cur >= 0 ? s_q.cur + 1 : 0, s_q.n);
		juke_text(w - 14 - (int)strlen(buf) * 8, 17, buf, U.col_dim, 1);
	}

	/* Now-playing line. */
	title = s_q.cur >= 0 ? juke_basename(s_q.item[s_q.cur]) : "(no track)";
	juke_text(14, 54, title, U.col_text, 1);
	if (s_q.dir[0])
		juke_text(14, 72, s_q.dir, U.col_dim, 1);

	/* Visualiser. */
	x0 = 14;
	x1 = w - 14;
	base = h - 66;
	maxh = base - 100;
	if (maxh < 24)
		maxh = 24;
	mid = base - maxh / 2;
	gap = 3;
	bw = (x1 - x0) / MMB_AUDIO_BANDS - gap;
	if (bw < 2)
		bw = 2;

	for (i = 1; i <= 3; i++)
		mmb_gfx_fill_rect(x0, base - maxh * i / 4, x1 - x0, 1,
				  U.col_panel2);

	for (i = 0; i < MMB_AUDIO_BANDS; i++)
	{
		int bx = x0 + i * (bw + gap);
		int bh = (int)(bands[i] * (float)maxh);
		int y, rh, py;
		float p;

		if (bh < 2)
			bh = 2;
		for (y = 0; y < bh; y += 4)
		{
			float frac = (float)y / (float)maxh;
			unsigned c;
			int seg_h = 4;
			if (frac > 0.72f)
				c = U.col_bar_hi;
			else if (frac > 0.45f)
				c = U.col_bar_mid;
			else
				c = U.col_bar_lo;
			if (y + seg_h > bh)
				seg_h = bh - y;
			mmb_gfx_fill_rect(bx, base - y - seg_h, bw, seg_h, c);
		}

		/* Reflection. */
		rh = bh / 4;
		if (rh > 16)
			rh = 16;
		mmb_gfx_fill_rect(bx, base + 3, bw, rh, juke_dim(U.col_bar_lo));

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
		py = base - (int)(p * (float)maxh) - 3;
		if (py < base - maxh - 3)
			py = base - maxh - 3;
		mmb_gfx_fill_rect(bx, py, bw, 2, U.col_hot);
	}

	mmb_gfx_fill_rect(x0, base, x1 - x0, 2, U.col_panel2);

	/* Oscilloscope trace. */
	if (n > 1)
	{
		int px = -1, py = 0;
		for (i = 0; i < n; i++)
		{
			int xx = x0 + (int)((long)(x1 - x0) * i / (n - 1));
			int yy = mid + (int)((long)scope[i] * (maxh / 2) / 32768);
			if (yy < base - maxh)
				yy = base - maxh;
			if (yy > base)
				yy = base;
			if (px >= 0)
				mmb_gfx_line(px, py, xx, yy, U.col_scan, 1);
			px = xx;
			py = yy;
		}
	}

	/* Footer. */
	mmb_gfx_fill_rect(0, h - 30, w, 30, U.col_panel);
	juke_text(14, h - 22,
		  "SPACE pause   </> track   S stop   ESC quit (keeps playing)",
		  U.col_dim, 1);
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

	if (have)
	{
		if (juke_build_queue(path) != 0)
			mmb_error("?FILE");
		if (juke_start(0) != 0)
			mmb_error("?FILE");
	}
	else if (!s_q.active || s_q.n == 0)
	{
		if (juke_build_queue(mmb_vfs_cwd()) != 0)
			mmb_error("?DIRECTORY");
		if (juke_start(0) != 0)
			mmb_error("?FILE");
	}

	memset(&U, 0, sizeof(U));
	U.saved_mode = G.gfx.mode;
	U.saved_bits = G.gfx.bits;
	U.saved_write_page = G.gfx.write_page;
	U.saved_display_page = G.gfx.display_page;
	U.saved_write_fb = G.gfx.write_fb;
	U.saved_font_scale = G.gfx.font_scale;
	mmb_gfx_set_mode(JUKE_MODE, 8);
	U.w = G.gfx.w > 0 ? G.gfx.w : 960;
	U.h = G.gfx.h > 0 ? G.gfx.h : 540;
	U.front = JUKE_PAGE_A;
	juke_load_colours();
	U.active = 1;
	U.last_ms = mmb_now_ms();
	juke_frame();
}

int mmb_in_juke(void)
{
	return U.active;
}

const char *mmb_juke_key(char c)
{
	if (!U.active)
		return "";
	if (c == 27 || c == 3 || c == 'q' || c == 'Q' || c == '\r' || c == '\n')
	{
		juke_leave();
		return "";
	}
	if (c == ' ')
	{
		mmb_play_pause(!G.audio.paused);
		return "";
	}
	if (c == 'n' || c == 'N' || c == '>' || c == '.')
	{
		if (s_q.n > 0)
			(void)juke_start(s_q.cur + 1);
		return "";
	}
	if (c == 'p' || c == 'P' || c == '<' || c == ',')
	{
		if (s_q.n > 0)
			(void)juke_start(s_q.cur - 1);
		return "";
	}
	if (c == 's' || c == 'S')
	{
		s_q.active = 0;
		mmb_play_stop();
		return "";
	}
	return "";
}

void mmb_juke_poll(void)
{
	unsigned now;

	juke_manage();
	if (!U.active)
		return;
	now = mmb_now_ms();
	if (now - U.last_ms < JUKE_FRAME_MS)
		return;
	U.last_ms = now;
	juke_frame();
}
