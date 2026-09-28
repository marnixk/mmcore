#include "mmb_priv.h"

/*
 * JUKE: a first-party retro music player (ScreamTracker-era feel).
 *
 *   JUKE "file"    play one file
 *   JUKE "folder"  queue every supported file in a folder (continuous)
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

#define JUKE_MODE      12      /* 960x540 (32-bit pages; see juke_load_colours) */
#define JUKE_PAGE_A    0
#define JUKE_PAGE_B    2
#define JUKE_FRAME_MS  33
#define JUKE_MAX_QUEUE 64
#define JUKE_PATH_MAX  160
#define JUKE_LIST_MAX  2048
#define JUKE_SCOPE_N    64     /* scope samples drawn per frame      */
#define JUKE_SCOPE_HIST 6      /* ghost history frames (AFK-style)   */

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
	short scope_hist[JUKE_SCOPE_HIST][JUKE_SCOPE_N];
	int scope_head;
	uint32_t *logo;                /* decoded graffiti wordmark (#914) */
	int logo_w, logo_h;
	unsigned col_bg, col_panel, col_panel2, col_track, col_text, col_dim;
	unsigned col_bar_lo, col_bar_hi, col_scan, col_peak;
	unsigned col_scope_lo, col_vol, col_base;
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

/* Muted grey base -> lime tip, for the per-bar gradient. */
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
	U.col_bar_lo = 0x232724u;   /* near-black bar base   */
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

static void juke_draw_logo(int x0, int y0)
{
	int i, j;

	if (!U.logo)
		return;
	for (j = 0; j < U.logo_h; j++)
		for (i = 0; i < U.logo_w; i++)
		{
			uint32_t c = U.logo[j * U.logo_w + i];
			if (!(c >> 24))
				continue;
			mmb_gfx_plot(x0 + i, y0 + j, c & 0xFFFFFFu);
		}
}

static void juke_paint(int w, int h)
{
	float bands[MMB_AUDIO_BANDS];
	short scope[JUKE_SCOPE_N];
	int i, n, x0, x1, bw, gap, base, maxh, fy, vol;
	int px, py0, py1, cy, amp, age;
	const char *title;
	char buf[128];

	mmb_audio_spectrum(bands, MMB_AUDIO_BANDS);
	n = mmb_audio_scope(scope, JUKE_SCOPE_N);
	if (n > JUKE_SCOPE_N)
		n = JUKE_SCOPE_N;

	/* AFK-style ghost history: keep a ring of past scope frames so older
	 * traces fade and cool toward the base grey while the newest stays lime. */
	U.scope_head = (U.scope_head + 1) % JUKE_SCOPE_HIST;
	for (i = 0; i < JUKE_SCOPE_N; i++)
		U.scope_hist[U.scope_head][i] = i < n ? scope[i] : 0;

	mmb_gfx_cls(U.col_bg);

	/* Header: full-size graffiti wordmark and right-aligned status. The
	 * wordmark is 50px tall, so the bar field below is shortened to fit. */
	juke_draw_logo(14, 1);
	{
		const char *fmt = s_q.cur >= 0 ? juke_ext(juke_track(s_q.cur)) : "";
		sprintf(buf, "%s  %s  %d/%d%s%s", juke_state_str(), fmt,
			s_q.cur >= 0 ? s_q.cur + 1 : 0, s_q.n,
			s_q.shuffle ? "  SHUF" : "",
			s_q.truncated ? "  more" : "");
		juke_text(w - 14 - (int)strlen(buf) * 8, 20, buf, U.col_dim, 1);
	}

	/* Now-playing line and folder. */
	title = s_q.cur >= 0 ? juke_basename(juke_track(s_q.cur)) : "(no track)";
	juke_text(14, 60, title, U.col_text, 1);
	if (s_q.dir[0])
		juke_text(14, 78, s_q.dir, U.col_dim, 1);

	/* Oscilloscope: its own bordered inset under the title, with a ghost
	 * history of past frames (oldest first so the newest lands on top). */
	{
		int bx = 8, by = 100, bw2 = w - 16, bh2 = 52;
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

	/* Spectrum: 24 grey-to-lime bars, no midfield guide lines. The bar field
	 * is deliberately short so the full-size wordmark still fits above it,
	 * and it is centred so the leftover width splits evenly on both sides
	 * instead of pooling to the right of the last band. */
	base = h - 71;
	maxh = base - 190;
	if (maxh < 24)
		maxh = 24;
	gap = 3;
	bw = (w - 28) / MMB_AUDIO_BANDS - gap;
	if (bw < 2)
		bw = 2;
	{
		int span = MMB_AUDIO_BANDS * bw + (MMB_AUDIO_BANDS - 1) * gap;
		x0 = 14 + (w - 28 - span) / 2;
		if (x0 < 14)
			x0 = 14;
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
		/* Vertical per-bar gradient: grey base -> lime tip. */
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

	/* Footer: transport legend, then shuffle / volume indicators. */
	fy = h - 48;
	juke_text(14, fy + 6,
		  "SPACE play/pause   P prev   N next   R shuf   -/+ vol   M mute   S stop   ESC quit",
		  U.col_dim, 1);

	/* Shuffle chip: a solid swatch that lights up when shuffle is on. */
	mmb_gfx_fill_rect(14, fy + 27, 14, 14,
			  s_q.shuffle ? U.col_peak : U.col_track);
	juke_text(34, fy + 28, "SHUF", s_q.shuffle ? U.col_text : U.col_dim, 1);

	/* Volume level bar: grey body with a muted logo-accent top edge. */
	vol = g_audio.vol_l;
	if (vol < 0)
		vol = 0;
	if (vol > 100)
		vol = 100;
	juke_text(110, fy + 28, "VOL", U.col_dim, 1);
	mmb_gfx_fill_rect(146, fy + 27, 220, 14, U.col_track);
	if (vol > 0)
	{
		int vw = vol * 220 / 100;
		int vx;
		mmb_gfx_fill_rect(146, fy + 27, vw, 14, U.col_vol);
		for (vx = 0; vx < vw; vx += 2)
		{
			int seg = vw - vx < 2 ? vw - vx : 2;
			mmb_gfx_fill_rect(146 + vx, fy + 27, seg, 2,
					  juke_logo_grad((float)vx / 220.0f));
		}
	}
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
	mmb_gfx_set_mode(JUKE_MODE, 32);
	U.w = G.gfx.w > 0 ? G.gfx.w : 960;
	U.h = G.gfx.h > 0 ? G.gfx.h : 540;
	U.front = JUKE_PAGE_A;
	juke_load_colours();
	juke_load_logo();
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
		mmb_yield_remove(juke_yield);
		mmb_play_stop();
		return "";
	}
	return "";
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
	if (now - U.last_ms < JUKE_FRAME_MS)
		return;
	U.last_ms = now;
	juke_frame();
}
