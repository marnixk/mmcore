#include "mmb_priv.h"
#include <math.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#include "hxcmod.h"

#undef ALIGN
#define JAR_XM_IMPLEMENTATION
#include "jar_xm.h"

#define MIX_RATE     44100
#define MIX_CHUNK    512
#define MIX_PREROLL  1024
/* Keep ~160ms in the DMA queue (two HDMI IEC958 periods are ~87ms). */
#define MIX_TARGET   ((MIX_RATE * 160) / 1000)
#define MIX_FILL_MAX 24

static drmp3 s_mp3;
static int s_mp3_on;
static modcontext s_mod;
static int s_mod_on;
static jar_xm_context_t *s_xm;
static unsigned char *s_moddata;
static unsigned char *s_xmdata;
static double s_tone_hz_l, s_tone_hz_r;
static double s_tone_ph_l, s_tone_ph_r;
static unsigned s_tone_left; /* ~0u = hold until STOP */
static unsigned s_mix_origin;
static unsigned s_pause_at;
static short s_pending[MIX_CHUNK * 2];
static unsigned s_pending_n;

static void apply_vol(short *pcm, unsigned nframes)
{
	int i, n = (int)(nframes * 2);
	int vl = G.audio.vol_l, vr = G.audio.vol_r;
	if (vl < 0) vl = 0;
	if (vl > 100) vl = 100;
	if (vr < 0) vr = 0;
	if (vr > 100) vr = 100;
	if (vl == 100 && vr == 100)
		return;
	for (i = 0; i < n; i += 2)
	{
		int l = (pcm[i] * vl) / 100;
		int r = (pcm[i + 1] * vr) / 100;
		if (l > 32767) l = 32767;
		if (l < -32768) l = -32768;
		if (r > 32767) r = 32767;
		if (r < -32768) r = -32768;
		pcm[i] = (short)l;
		pcm[i + 1] = (short)r;
	}
}

static int flush_pending(void)
{
	int w;

	if (!s_pending_n)
		return 1;
	if (!G.opt.audio_on || !G.plat || !G.plat->audio_write)
	{
		G.audio.samples_decoded += s_pending_n;
		s_pending_n = 0;
		return 1;
	}
	w = G.plat->audio_write(s_pending, s_pending_n);
	if (w < 0)
		w = 0;
	if ((unsigned)w >= s_pending_n)
	{
		G.audio.samples_decoded += s_pending_n;
		s_pending_n = 0;
		return 1;
	}
	if (w > 0)
	{
		unsigned left = s_pending_n - (unsigned)w;
		memmove(s_pending, s_pending + (unsigned)w * 2,
			left * 2 * sizeof(short));
		s_pending_n = left;
		G.audio.samples_decoded += (unsigned)w;
	}
	return 0;
}

static unsigned emit_pcm(short *pcm, unsigned nframes)
{
	int w;

	if (!nframes)
		return 0;
	apply_vol(pcm, nframes);
	if (!G.opt.audio_on || !G.plat || !G.plat->audio_write)
	{
		G.audio.samples_decoded += nframes;
		return nframes;
	}
	w = G.plat->audio_write(pcm, nframes);
	if (w < 0)
		w = 0;
	if ((unsigned)w < nframes)
	{
		unsigned left = nframes - (unsigned)w;
		if (left > MIX_CHUNK)
			left = MIX_CHUNK;
		memcpy(s_pending, pcm + (unsigned)w * 2, left * 2 * sizeof(short));
		s_pending_n = left;
	}
	G.audio.samples_decoded += (unsigned)w;
	return (unsigned)w;
}

static void play_teardown(void)
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
	s_tone_left = 0;
	s_tone_hz_l = s_tone_hz_r = 0;
	s_tone_ph_l = s_tone_ph_r = 0;
	s_pending_n = 0;
}

void mmb_play_stop(void)
{
	if (G.plat && G.plat->audio_flush)
		G.plat->audio_flush();
	play_teardown();
}

void mmb_audio_apply_options(void)
{
	if (!G.plat)
		return;
	if (G.plat->audio_set_target)
		G.plat->audio_set_target(G.opt.audio_target);
	if (G.plat->audio_enable)
		G.plat->audio_enable(G.opt.audio_on);
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

static void play_begin(int kind, const char *path)
{
	G.audio.playing = kind;
	G.audio.paused = 0;
	G.audio.vol_l = G.audio.vol_r = 100;
	G.audio.samples_decoded = 0;
	G.audio.name[0] = 0;
	if (path)
		strncpy(G.audio.name, path, sizeof(G.audio.name) - 1);
	s_mix_origin = mmb_now_ms();
	s_pause_at = 0;
	mmb_audio_apply_options();
	/* Queue a DMA preroll before the caller redraws HDMI. */
	mmb_play_mix();
}

int mmb_play_mp3(const char *path)
{
	unsigned n = 0;
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
	play_begin(1, path);
	return 0;
}

int mmb_play_mod(const char *path)
{
	unsigned n = 0;
	mmb_play_stop();
	if (load_bytes(path, &s_moddata, &n) != 0)
		return -1;
	hxcmod_init(&s_mod);
	hxcmod_setcfg(&s_mod, MIX_RATE, 1, 1);
	if (!hxcmod_load(&s_mod, s_moddata, (int)n))
		return -1;
	s_mod_on = 1;
	play_begin(2, path);
	return 0;
}

int mmb_play_xm(const char *path)
{
	unsigned n = 0;
	mmb_play_stop();
	if (load_bytes(path, &s_xmdata, &n) != 0)
		return -1;
	if (jar_xm_create_context_safe(&s_xm, (const char *)s_xmdata, n, MIX_RATE) != 0)
		return -1;
	jar_xm_set_max_loop_count(s_xm, 0);
	play_begin(3, path);
	return 0;
}

static unsigned due_frames(void)
{
	unsigned free_n = ~0u;

	if (G.opt.audio_on && G.plat && G.plat->audio_free_frames)
		free_n = G.plat->audio_free_frames();

	if (G.opt.audio_on && G.plat && G.plat->audio_have_device &&
	    G.plat->audio_have_device())
	{
		unsigned queued = 0;
		unsigned room;

		if (G.plat->audio_queued_frames)
			queued = G.plat->audio_queued_frames();
		if (queued >= MIX_TARGET)
			return 0;
		room = MIX_TARGET - queued;
		if (free_n < room)
			room = free_n;
		if (room > MIX_CHUNK)
			room = MIX_CHUNK;
		return room;
	}

	{
		unsigned elapsed = mmb_now_ms() - s_mix_origin;
		unsigned due = MIX_PREROLL +
			(unsigned)((unsigned long)elapsed * MIX_RATE / 1000u);
		if (due < G.audio.samples_decoded)
			return 0;
		due -= G.audio.samples_decoded;
		if (due > MIX_CHUNK)
			due = MIX_CHUNK;
		if (free_n < due)
			due = free_n;
		return due;
	}
}

static int mix_mp3(unsigned nframes)
{
	short pcm[MIX_CHUNK * 2];
	drmp3_uint64 got;
	unsigned i;

	got = drmp3_read_pcm_frames_s16(&s_mp3, nframes, pcm);
	if (got == 0)
		return 0;
	if (s_mp3.channels == 1)
	{
		for (i = (unsigned)got; i-- > 0;)
		{
			short s = pcm[i];
			pcm[i * 2] = s;
			pcm[i * 2 + 1] = s;
		}
	}
	emit_pcm(pcm, (unsigned)got);
	return got >= nframes ? 1 : 0;
}

static int mix_mod(unsigned nframes)
{
	msample pcm[MIX_CHUNK * 2];
	hxcmod_fillbuffer(&s_mod, pcm, nframes, 0, 0);
	emit_pcm((short *)pcm, nframes);
	return 1;
}

static int mix_xm(unsigned nframes)
{
	float tmp[MIX_CHUNK * 2];
	short pcm[MIX_CHUNK * 2];
	unsigned i;
	jar_xm_generate_samples(s_xm, tmp, nframes);
	for (i = 0; i < nframes * 2; i++)
	{
		int v = (int)(tmp[i] * 32767.0f);
		if (v > 32767) v = 32767;
		if (v < -32768) v = -32768;
		pcm[i] = (short)v;
	}
	emit_pcm(pcm, nframes);
	return 1;
}

static int mix_tone(unsigned nframes)
{
	short pcm[MIX_CHUNK * 2];
	unsigned i;
	double step_l = 2.0 * M_PI * s_tone_hz_l / (double)MIX_RATE;
	double step_r = 2.0 * M_PI * s_tone_hz_r / (double)MIX_RATE;

	if (s_tone_left != ~0u)
	{
		if (s_tone_left == 0)
			return 0;
		if (nframes > s_tone_left)
			nframes = s_tone_left;
	}
	for (i = 0; i < nframes; i++)
	{
		pcm[i * 2] = (short)(sin(s_tone_ph_l) * 8000.0);
		pcm[i * 2 + 1] = (short)(sin(s_tone_ph_r) * 8000.0);
		s_tone_ph_l += step_l;
		s_tone_ph_r += step_r;
		if (s_tone_ph_l > 2.0 * M_PI) s_tone_ph_l -= 2.0 * M_PI;
		if (s_tone_ph_r > 2.0 * M_PI) s_tone_ph_r -= 2.0 * M_PI;
	}
	emit_pcm(pcm, nframes);
	if (s_tone_left != ~0u)
	{
		if (s_tone_left <= nframes)
		{
			s_tone_left = 0;
			return 0;
		}
		s_tone_left -= nframes;
	}
	return 1;
}

void mmb_play_mix(void)
{
	unsigned n;
	int keep = 1;
	int loops;

	if (!G.audio.playing || G.audio.paused)
		return;
	if (!flush_pending())
		return;
	for (loops = 0; loops < MIX_FILL_MAX; loops++)
	{
		n = due_frames();
		if (n == 0)
			break;
		if (G.audio.playing == 1 && s_mp3_on)
			keep = mix_mp3(n);
		else if (G.audio.playing == 2 && s_mod_on)
			keep = mix_mod(n);
		else if (G.audio.playing == 3 && s_xm)
			keep = mix_xm(n);
		else if (G.audio.playing == 4)
			keep = mix_tone(n);
		else
			keep = 0;
		if (!keep)
			break;
		if (s_pending_n)
			break;
	}
	if (!keep)
	{
		(void)flush_pending();
		if (G.plat && G.plat->audio_kick)
			G.plat->audio_kick();
		play_teardown();
	}
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
		if (G.audio.playing && !G.audio.paused)
		{
			G.audio.paused = 1;
			s_pause_at = mmb_now_ms();
		}
		return;
	}
	if (mmb_match("RESUME"))
	{
		if (G.audio.playing && G.audio.paused)
		{
			G.audio.paused = 0;
			s_mix_origin += mmb_now_ms() - s_pause_at;
		}
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
		mmb_val l = mmb_expr();
		mmb_val r;
		unsigned dur = 0;
		mmb_skip_sp();
		if (*G.p != ',')
			mmb_syntax();
		G.p++;
		r = mmb_expr();
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			dur = (unsigned)mmb_as_int(mmb_expr());
		}
		mmb_play_stop();
		s_tone_hz_l = mmb_as_float(l);
		s_tone_hz_r = mmb_as_float(r);
		s_tone_ph_l = s_tone_ph_r = 0;
		if (dur)
		{
			s_tone_left = (unsigned)((unsigned long)dur * MIX_RATE / 1000u);
			if (s_tone_left == 0)
				s_tone_left = 1;
		}
		else
			s_tone_left = ~0u;
		play_begin(4, "TONE");
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
