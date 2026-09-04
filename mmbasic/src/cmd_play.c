#include "mmb_priv.h"
#include <stdbool.h>

/* Decoders pulled in locally so we do not compile picomite-fork. */
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#include "hxcmod.h"

#undef ALIGN
#define JAR_XM_IMPLEMENTATION
#include "jar_xm.h"

static drmp3 s_mp3;
static int s_mp3_on;
static modcontext s_mod;
static int s_mod_on;
static jar_xm_context_t *s_xm;
static unsigned char *s_moddata;
static unsigned char *s_xmdata;

void mmb_play_stop(void)
{
	if (s_mp3_on)
	{
		drmp3_uninit(&s_mp3);
		s_mp3_on = 0;
	}
	if (s_mod_on)
	{
		hxcmod_unload(&s_mod);
		s_mod_on = 0;
	}
	if (s_xm)
	{
		jar_xm_free_context(s_xm);
		s_xm = 0;
	}
	if (s_moddata)
	{
		G.plat->free(s_moddata);
		s_moddata = 0;
	}
	if (s_xmdata)
	{
		G.plat->free(s_xmdata);
		s_xmdata = 0;
	}
	G.audio.playing = 0;
	G.audio.paused = 0;
	G.audio.samples_decoded = 0;
	G.audio.name[0] = 0;
}

static int load_bytes(const char *path, unsigned char **out, unsigned *n)
{
	int sz = mmb_vfs_size(path);
	unsigned got = 0;
	if (sz < 0)
		return -1;
	*out = G.plat->alloc((unsigned)sz);
	if (!*out)
		return -1;
	if (mmb_vfs_read(path, *out, (unsigned)sz, &got) != 0)
	{
		G.plat->free(*out);
		*out = 0;
		return -1;
	}
	*n = got;
	return 0;
}

int mmb_play_mp3(const char *path)
{
	unsigned n = 0;
	drmp3_int16 pcm[256];
	mmb_play_stop();
	if (load_bytes(path, &s_moddata, &n) != 0)
		return -1;
	if (!drmp3_init_memory(&s_mp3, s_moddata, n, NULL))
	{
		G.plat->free(s_moddata);
		s_moddata = 0;
		return -1;
	}
	s_mp3_on = 1;
	G.audio.samples_decoded = (unsigned)drmp3_read_pcm_frames_s16(&s_mp3, 128, pcm);
	G.audio.playing = 1;
	G.audio.vol_l = G.audio.vol_r = 100;
	strncpy(G.audio.name, path, sizeof(G.audio.name) - 1);
	return G.audio.samples_decoded ? 0 : -1;
}

int mmb_play_mod(const char *path)
{
	unsigned n = 0;
	msample out[256];
	mmb_play_stop();
	if (load_bytes(path, &s_moddata, &n) != 0)
		return -1;
	hxcmod_init(&s_mod);
	hxcmod_setcfg(&s_mod, 44100, 1, 1);
	if (!hxcmod_load(&s_mod, s_moddata, (int)n))
		return -1;
	s_mod_on = 1;
	hxcmod_fillbuffer(&s_mod, out, 64, 0, 0);
	G.audio.samples_decoded = 64;
	G.audio.playing = 2;
	G.audio.vol_l = G.audio.vol_r = 100;
	strncpy(G.audio.name, path, sizeof(G.audio.name) - 1);
	return 0;
}

int mmb_play_xm(const char *path)
{
	unsigned n = 0;
	float out[128];
	mmb_play_stop();
	if (load_bytes(path, &s_xmdata, &n) != 0)
		return -1;
	if (jar_xm_create_context_safe(&s_xm, (const char *)s_xmdata, n, 44100) != 0)
		return -1;
	jar_xm_generate_samples(s_xm, out, 64);
	G.audio.samples_decoded = 64;
	G.audio.playing = 3;
	G.audio.vol_l = G.audio.vol_r = 100;
	strncpy(G.audio.name, path, sizeof(G.audio.name) - 1);
	return 0;
}

void mmb_cmd_play(void)
{
	if (mmb_match("STOP"))
	{
		mmb_play_stop();
		return;
	}
	if (mmb_match("PAUSE"))
	{
		G.audio.paused = 1;
		return;
	}
	if (mmb_match("RESUME"))
	{
		G.audio.paused = 0;
		return;
	}
	if (mmb_match("VOLUME"))
	{
		G.audio.vol_l = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			G.audio.vol_r = (int)mmb_as_int(mmb_expr());
		}
		else
			G.audio.vol_r = G.audio.vol_l;
		return;
	}
	if (mmb_match("TONE"))
	{
		/* PLAY TONE l, r [, dur] — mark as playing */
		mmb_expr();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			mmb_expr();
		}
		G.audio.playing = 4;
		G.audio.paused = 0;
		return;
	}
	if (mmb_match("MP3"))
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		if (mmb_play_mp3(v.s) != 0)
			mmb_error("?MP3");
		return;
	}
	if (mmb_match("MODFILE") || mmb_match("MOD"))
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		if (mmb_play_mod(v.s) != 0)
			mmb_error("?MOD");
		return;
	}
	if (mmb_match("XM") || mmb_match("XMFILE"))
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		if (mmb_play_xm(v.s) != 0)
			mmb_error("?XM");
		return;
	}
	mmb_syntax();
}

/* stdio leftovers referenced by jar_xm's unused from_file helper */
#include <stdio.h>
FILE *fopen(const char *p, const char *m)
{
	(void)p;
	(void)m;
	return 0;
}
int fclose(FILE *f)
{
	(void)f;
	return 0;
}
size_t fread(void *p, size_t s, size_t n, FILE *f)
{
	(void)p;
	(void)s;
	(void)n;
	(void)f;
	return 0;
}
int fseek(FILE *f, long o, int w)
{
	(void)f;
	(void)o;
	(void)w;
	return -1;
}
long ftell(FILE *f)
{
	(void)f;
	return 0;
}
int fprintf(FILE *f, const char *fmt, ...)
{
	(void)f;
	(void)fmt;
	return 0;
}
