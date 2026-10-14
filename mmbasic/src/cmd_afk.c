#include "mmb_priv.h"

/*
 * AFK: a Win31 "Mystify"-style screensaver. Two polygons of 3-6 vertices
 * bounce around a 960x540/MODE 12 screen, each vertex trailing a fading ring
 * of positions. When the console is already in MODE 19/20 (the Chromebook
 * panel sizes) AFK renders in MODE 19 instead. Enter or the break key exits
 * and restores the prior mode.
 */

#define AFK_SETS 2
#define AFK_MAXV 6
#define AFK_HIST 24
#define AFK_FRAME_MS 16
#define AFK_MODE 12
#define AFK_MODE_WIDE 19
#define AFK_PASS_MAX 32
/* Soft pages 0 and 2 are the front/back buffers; page 1 is the CMM2 overlay. */
#define AFK_PAGE_A 0
#define AFK_PAGE_B 2

typedef struct {
	int x, y, dx, dy;
	int hx[AFK_HIST];
	int hy[AFK_HIST];
} afk_vert;

static struct {
	int active;
	int locked;            /* OPTION AFK PASSWORD: showing the lock prompt */
	char input[AFK_PASS_MAX + 1];
	int inlen;
	int saved_mode, saved_bits;
	int saved_write_page, saved_display_page, saved_write_fb;
	int nv[AFK_SETS];
	afk_vert v[AFK_SETS][AFK_MAXV];
	unsigned rgb[AFK_SETS];
	int w, h;
	int front;
	unsigned last_ms;
	unsigned rng;
} A_s[MMB_MAX_CONSOLES];
#define A (A_s[g_console])

/* OPTION AFK PASSWORD stores a cheap FNV-1a hash (16 hex digits) so the plain
 * text never reaches .mmbasic.ini. This is a deterrent, not security. The same
 * function hashes the typed input for the lock-screen check. */
void mmb_afk_hash_password(const char *pass, char *out, int outsz)
{
	static const char hex[] = "0123456789abcdef";
	uint64_t h = 1469598103934665603ULL;
	int i;

	if (!out || outsz <= 0)
		return;
	if (outsz < 17)
	{
		out[0] = 0;
		return;
	}
	if (!pass)
		pass = "";
	for (; *pass; pass++)
	{
		h ^= (unsigned char)*pass;
		h *= 1099511628211ULL;
	}
	for (i = 0; i < 16; i++)
		out[i] = hex[(h >> (60 - i * 4)) & 0xf];
	out[16] = 0;
}

static unsigned afk_rand(void)
{
	unsigned x = A.rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	A.rng = x;
	return x;
}

static int afk_range(int lo, int hi)
{
	if (hi <= lo)
		return lo;
	return lo + (int)(afk_rand() % (unsigned)(hi - lo + 1));
}

static int afk_sign(void)
{
	return (afk_rand() & 1) ? 1 : -1;
}

static unsigned afk_scale(unsigned c, int pct)
{
	int r = (int)((c >> 16) & 255) * pct / 100;
	int g = (int)((c >> 8) & 255) * pct / 100;
	int b = (int)(c & 255) * pct / 100;
	return (unsigned)((r << 16) | (g << 8) | b);
}

/* Lift a set colour until it is clearly visible on black. */
static unsigned afk_bright(unsigned c)
{
	int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
	int lum = (r * 299 + g * 587 + b * 114) / 1000;
	int min = 96, den;
	if (lum >= min)
		return c;
	den = 255 - lum + 1;
	r += (255 - r) * (min - lum) / den;
	g += (255 - g) * (min - lum) / den;
	b += (255 - b) * (min - lum) / den;
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	return (unsigned)((r << 16) | (g << 8) | b);
}

static unsigned afk_theme(const char *name, unsigned fallback)
{
	unsigned rgb = 0;
	if (mmb_editor_theme_rgb(name, &rgb))
		return afk_bright(rgb);
	return afk_bright(fallback);
}

static void afk_init_set(int s)
{
	int i, a;
	A.nv[s] = afk_range(3, AFK_MAXV);
	for (i = 0; i < A.nv[s]; i++)
	{
		afk_vert *v = &A.v[s][i];
		v->x = afk_range(0, A.w - 1);
		v->y = afk_range(0, A.h - 1);
		v->dx = afk_range(2, 7) * afk_sign();
		v->dy = afk_range(2, 7) * afk_sign();
		for (a = 0; a < AFK_HIST; a++)
		{
			v->hx[a] = v->x;
			v->hy[a] = v->y;
		}
	}
}

static void afk_step(void)
{
	int s, i, a;
	for (s = 0; s < AFK_SETS; s++)
	{
		for (i = 0; i < A.nv[s]; i++)
		{
			afk_vert *v = &A.v[s][i];
			v->x += v->dx;
			v->y += v->dy;
			if (v->x < 0)
			{
				v->x = 0;
				v->dx = -v->dx;
			}
			else if (v->x >= A.w)
			{
				v->x = A.w - 1;
				v->dx = -v->dx;
			}
			if (v->y < 0)
			{
				v->y = 0;
				v->dy = -v->dy;
			}
			else if (v->y >= A.h)
			{
				v->y = A.h - 1;
				v->dy = -v->dy;
			}
			for (a = AFK_HIST - 1; a > 0; a--)
			{
				v->hx[a] = v->hx[a - 1];
				v->hy[a] = v->hy[a - 1];
			}
			v->hx[0] = v->x;
			v->hy[0] = v->y;
		}
	}
}

static void afk_frame(void)
{
	int s, i, a, back;

	/* Pace the loop to the vertical blank before rendering the next frame.
	 * The page-flip present waits for VSync again before SetVirtualOffset,
	 * so the swap lands in blanking; on platforms without the flip path the
	 * wait still starts the blit at the top of a frame. wait_vsync is a
	 * no-op under QEMU, so simulated runs are unaffected. */
	if (G.plat && G.plat->wait_vsync)
		G.plat->wait_vsync();

	/* Render the next frame into the hidden soft page, then flip. */
	back = (A.front == AFK_PAGE_A) ? AFK_PAGE_B : AFK_PAGE_A;
	G.gfx.write_page = back;
	G.gfx.write_fb = 0;
	mmb_gfx_cls(0);
	/* Draw by depth, not by set: the oldest (faintest) trails from both sets
	 * go down first, then each newer, brighter ring, so the brightest lines
	 * always end up on top regardless of which set they belong to (#1174). */
	for (a = AFK_HIST - 1; a >= 0; a--)
	{
		int pct = 100 - (a * 95) / (AFK_HIST - 1);
		for (s = 0; s < AFK_SETS; s++)
		{
			unsigned c = afk_scale(A.rgb[s], pct);
			for (i = 0; i < A.nv[s]; i++)
			{
				int j = (i + 1) % A.nv[s];
				mmb_gfx_line(A.v[s][i].hx[a], A.v[s][i].hy[a],
					     A.v[s][j].hx[a], A.v[s][j].hy[a], c, 1);
			}
		}
	}
	G.gfx.display_page = back;
	mmb_gfx_dirty_add(0, 0, A.w, A.h);
	if (G.plat && G.plat->present_set_flip)
		G.plat->present_set_flip(1);
	mmb_gfx_present();
	A.front = back;
}

static void afk_leave(void)
{
	if (!A.active)
		return;
	A.active = 0;
	if (A.saved_mode != G.gfx.mode || A.saved_bits != G.gfx.bits)
		mmb_gfx_set_mode(A.saved_mode, A.saved_bits);
	mmb_gfx_reset_console(1);
}

/* OPTION AFK PASSWORD: stop the animation and draw the lock prompt over the
 * frozen frame. The typed password shows as asterisks. */
static void afk_draw_lock(void)
{
	int bw = 360, bh = 132;
	int bx, by, i;
	unsigned edge = 0xF0F0F0u, fg = 0xFFFFFFu, bg = 0x101018u, hint = 0x9090A0u;
	char stars[AFK_PASS_MAX + 1];

	bx = (A.w - bw) / 2;
	by = (A.h - bh) / 2;
	if (bx < 0)
		bx = 0;
	if (by < 0)
		by = 0;
	G.gfx.write_page = A.front;
	G.gfx.write_fb = 0;
	mmb_gfx_box(bx, by, bw, bh, edge, 2, (int)bg);
	mmb_gfx_text(bx + 24, by + 24, "PASSWORD:", fg);
	for (i = 0; i < A.inlen; i++)
		stars[i] = '*';
	stars[A.inlen] = 0;
	mmb_gfx_text(bx + 24, by + 56, stars, fg);
	mmb_gfx_text(bx + 24, by + 96, "ENTER=UNLOCK  ESC=CANCEL", hint);
	G.gfx.display_page = A.front;
	mmb_gfx_dirty_add(0, 0, A.w, A.h);
	if (G.plat && G.plat->present_set_flip)
		G.plat->present_set_flip(1);
	mmb_gfx_present();
}

void mmb_cmd_afk(void)
{
	int s;
	if (G.running)
		mmb_error("?Not available in RUN");
	memset(&A, 0, sizeof(A));
	A.saved_mode = G.gfx.mode;
	A.saved_bits = G.gfx.bits;
	A.saved_write_page = G.gfx.write_page;
	A.saved_display_page = G.gfx.display_page;
	A.saved_write_fb = G.gfx.write_fb;
	/* The Chromebook (Lenovo 100e Gen 2) runs its console in MODE 19/20.
	 * AFK's 960x540 design looks wrong on that panel, so render in MODE 19
	 * (1366x768) when the console is already one of those modes; afk_leave
	 * restores the saved mode either way. */
	mmb_gfx_set_mode((A.saved_mode == 19 || A.saved_mode == 20) ? AFK_MODE_WIDE
								    : AFK_MODE,
			 8);
	A.w = G.gfx.w > 0 ? G.gfx.w : 960;
	A.h = G.gfx.h > 0 ? G.gfx.h : 540;
	A.rng = (unsigned)(mmb_now_ms() * 2654435761u) + 0x9E3779B9u;
	if (!A.rng)
		A.rng = 0x12345678u;
	A.rgb[0] = afk_theme("MENU_HOT", 0xFFFF55u);
	A.rgb[1] = afk_theme("STRING_FG", 0x55FFFFu);
	for (s = 0; s < AFK_SETS; s++)
		afk_init_set(s);
	A.last_ms = mmb_now_ms();
	A.front = AFK_PAGE_A;
	A.active = 1;
	afk_frame();
}

int mmb_in_afk(void)
{
	return A.active;
}

void mmb_afk_key(char c)
{
	if (!A.active)
		return;
	if (G.opt.afk_password[0])
	{
		/* The lock is a deterrent: every key (including Ctrl+C) stops the
		 * animation and asks for the password instead of leaving. */
		if (!A.locked)
		{
			A.locked = 1;
			A.inlen = 0;
			A.input[0] = 0;
			afk_draw_lock();
			return;
		}
		if (c == '\r' || c == '\n')
		{
			char h[17];
			mmb_afk_hash_password(A.input, h, sizeof(h));
			if (strcmp(h, G.opt.afk_password) == 0)
			{
				afk_leave();
				return;
			}
			A.inlen = 0;
			A.input[0] = 0;
			afk_draw_lock();
			return;
		}
		if (c == 8 || c == 127)
		{
			if (A.inlen > 0)
				A.input[--A.inlen] = 0;
			afk_draw_lock();
			return;
		}
		if (c == 27)
		{
			/* Escape cancels the dialog: drop the lock, discard the
			 * input and resume the screensaver without leaving AFK. */
			A.locked = 0;
			A.inlen = 0;
			A.input[0] = 0;
			A.last_ms = mmb_now_ms();
			afk_frame();
			return;
		}
		if (c >= 32 && c < 127 && A.inlen < AFK_PASS_MAX)
		{
			A.input[A.inlen++] = c;
			A.input[A.inlen] = 0;
			afk_draw_lock();
		}
		return;
	}
	if (c == '\r' || c == '\n' || c == 3)
		afk_leave();
}

void mmb_afk_poll(void)
{
	unsigned now;
	if (!A.active || A.locked)
		return;
	now = mmb_now_ms();
	if (now < A.last_ms + AFK_FRAME_MS)
		return;
	A.last_ms = now;
	afk_step();
	afk_frame();
}
