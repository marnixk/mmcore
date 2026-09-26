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
	int muted;
	int vol_saved;
	unsigned last_ms;
	float peak[MMB_AUDIO_BANDS];
	unsigned col_bg, col_panel, col_panel2, col_hot, col_text, col_dim;
	unsigned col_bar_lo, col_bar_mid, col_bar_hi, col_scan;
} juke_ui;

typedef struct {
	int active;
	int n;
	int cur;
	int shuffle;
	int truncated; /* the folder scan hit the newline buffer or the queue cap */
	int owner;     /* virtual console that started this queue (#805) */
	int order[JUKE_MAX_QUEUE]; /* play order: item index at each queue slot */
	char dir[JUKE_PATH_MAX];
	char item[JUKE_MAX_QUEUE][JUKE_PATH_MAX];
} juke_queue;

static juke_ui s_ui[MMB_MAX_CONSOLES];
#define U (s_ui[g_console])
static juke_queue s_q;
static unsigned s_rand = 0x9E3779B9u;

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
	s_q.shuffle = on;
}

/* ---- colour helpers --------------------------------------------------- *
 * JUKE owns a fixed dark cyberpunk palette. It deliberately ignores
 * OPTION EDIT THEME so the player looks the same under every system theme;
 * because nothing here touches the TUI palette, leaving JUKE restores the
 * user's theme untouched. */

static unsigned juke_dim(unsigned c)
{
	int r = (int)((c >> 16) & 255) / 3;
	int g = (int)((c >> 8) & 255) / 3;
	int b = (int)(c & 255) / 3;
	return (unsigned)((r << 16) | (g << 8) | b);
}

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

/* Cool base -> electric purple -> hot tip, for the per-bar gradient. */
static unsigned juke_grad(float t)
{
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (t < 0.5f)
		return juke_mix(U.col_bar_lo, U.col_bar_mid, t * 2.0f);
	return juke_mix(U.col_bar_mid, U.col_bar_hi, (t - 0.5f) * 2.0f);
}

static void juke_load_colours(void)
{
	U.col_bg = 0x05060Fu;      /* near-black indigo   */
	U.col_panel = 0x0C101Fu;   /* dark panel          */
	U.col_panel2 = 0x1B1440u;  /* deep purple line    */
	U.col_hot = 0xFF2BD6u;     /* neon magenta        */
	U.col_text = 0xE6F5FFu;    /* icy white           */
	U.col_dim = 0x7C89B8u;     /* muted periwinkle    */
	U.col_bar_lo = 0x00E0FFu;  /* electric cyan       */
	U.col_bar_mid = 0x9A4DFFu; /* electric purple     */
	U.col_bar_hi = 0xFF2BD6u;  /* neon magenta        */
	U.col_scan = 0x39FFEAu;    /* aqua trace          */
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
	int truncated = 0;

	s_q.n = 0;
	s_q.cur = -1;
	s_q.shuffle = 0;
	s_q.truncated = 0;
	s_q.dir[0] = 0;
	if (mmb_vfs_isdir(spec))
	{
		if (mmb_vfs_list(spec, list, sizeof(list), &truncated) != 0)
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
		/* Either the folder listing was cut, or the queue itself filled
		 * before the listing ran out (#693). */
		if (truncated || s_q.n >= JUKE_MAX_QUEUE)
			s_q.truncated = 1;
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
	/* play_begin resets the gain to 100; keep JUKE's own volume across
	 * track changes (shuffle/next must not blast the mixer). */
	vl = g_audio.vol_l;
	vr = g_audio.vol_r;
	if (juke_play_path(p) != 0)
		return -1;
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
	if (g_audio.playing && g_audio.paused)
		return "PAUSED";
	if (g_audio.playing)
		return "PLAY";
	if (s_q.active)
		return "READY";
	return "STOP";
}

static void juke_paint(int w, int h)
{
	float bands[MMB_AUDIO_BANDS];
	short scope[64];
	int i, n, x0, x1, bw, gap, base, maxh, mid, fy, vol;
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
		const char *fmt = s_q.cur >= 0 ? juke_ext(juke_track(s_q.cur)) : "";
		sprintf(buf, "%s  %s  %d/%d%s%s", juke_state_str(), fmt,
			s_q.cur >= 0 ? s_q.cur + 1 : 0, s_q.n,
			s_q.shuffle ? "  SHUF" : "",
			s_q.truncated ? "  more" : "");
		juke_text(w - 14 - (int)strlen(buf) * 8, 17, buf, U.col_dim, 1);
	}

	/* Now-playing line. */
	title = s_q.cur >= 0 ? juke_basename(juke_track(s_q.cur)) : "(no track)";
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
		/* Vertical per-bar gradient: cool base -> hot tip. */
		for (y = 0; y < bh; y += 4)
		{
			float frac = (float)y / (float)bh;
			int seg_h = 4;
			if (y + seg_h > bh)
				seg_h = bh - y;
			mmb_gfx_fill_rect(bx, base - y - seg_h, bw, seg_h,
					  juke_grad(frac));
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

	/* Footer: transport legend, then shuffle / volume indicators. */
	fy = h - 48;
	mmb_gfx_fill_rect(0, fy, w, 48, U.col_panel);
	mmb_gfx_fill_rect(0, fy, w, 1, U.col_panel2);
	juke_text(14, fy + 6,
		  "SPACE play/pause   P prev   N next   R shuf   -/+ vol   M mute   S stop   ESC quit",
		  U.col_dim, 1);

	/* Shuffle chip: a solid swatch that lights up when shuffle is on. */
	mmb_gfx_fill_rect(14, fy + 27, 14, 14,
			  s_q.shuffle ? U.col_hot : U.col_panel2);
	juke_text(34, fy + 28, "SHUF", s_q.shuffle ? U.col_hot : U.col_dim, 1);

	/* Volume level bar. */
	vol = g_audio.vol_l;
	if (vol < 0)
		vol = 0;
	if (vol > 100)
		vol = 100;
	juke_text(110, fy + 28, "VOL", U.col_dim, 1);
	mmb_gfx_fill_rect(146, fy + 27, 220, 14, U.col_panel2);
	if (vol > 0)
		mmb_gfx_fill_rect(146, fy + 27, vol * 220 / 100, 14,
				  juke_grad((float)vol / 100.0f));
	sprintf(buf, "%3d%%%s", vol, U.muted ? " MUTE" : "");
	juke_text(380, fy + 28, buf, U.col_text, 1);
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

	/* g_audio volumes default to 0 until the first track starts. JUKE keeps
	 * one player volume across tracks, so seed an audible level if none is
	 * set yet, then juke_start() saves/restores it around play_begin(). */
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
	if (c == 27 || c == 3 || c == 'q' || c == 'Q' || c == '\r' || c == '\n')
	{
		juke_leave();
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
