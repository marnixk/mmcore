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

/* One audio engine, one status: shared by every virtual console so playback
 * (and the mixer pump) survives a console switch. See mmb_priv.h. */
mmb_audio g_audio;

static drmp3 s_mp3;
static int s_mp3_on;
static modcontext s_mod;
static int s_mod_on;
static jar_xm_context_t *s_xm;
static unsigned char *s_moddata;
static unsigned char *s_xmdata;
static unsigned char *s_wav;
static unsigned s_wav_n, s_wav_off;
static int s_wav_ch, s_wav_bits, s_wav_rate;
static char s_tts_cb[MMB_MAX_NAME];
static double s_tone_hz_l, s_tone_hz_r;
static double s_tone_ph_l, s_tone_ph_r;
static unsigned s_tone_left; /* ~0u = hold until STOP */
static unsigned s_mix_origin;
static unsigned s_pause_at;
static short s_pending[MIX_CHUNK * 2];
static unsigned s_pending_n;

/* ---- Visualiser tap (JUKE) -------------------------------------------- *
 * A cheap filter-bank: a chain of one-pole low-passes with increasing cutoff.
 * The difference between adjacent stages isolates a frequency band. This
 * avoids an FFT while still giving a lively spectrum for the player. The
 * scope ring holds the most recent downsampled waveform. */
#define VIZ_BANDS MMB_AUDIO_BANDS
#define VIZ_SCOPE MMB_AUDIO_SCOPE
static int s_ended_natural;
static float s_viz_lp[VIZ_BANDS + 1];
static float s_viz_alpha[VIZ_BANDS + 1];
static float s_viz_band[VIZ_BANDS];
static int s_viz_ready;
static short s_scope[VIZ_SCOPE];
static unsigned s_scope_w;

static void viz_init(void)
{
	int b;
	double nyq = (double)MIX_RATE * 0.45;
	for (b = 0; b <= VIZ_BANDS; b++)
	{
		double fc = 60.0 * pow(2.0, (double)b * 7.0 / (double)VIZ_BANDS);
		if (fc > nyq)
			fc = nyq;
		s_viz_alpha[b] =
			(float)(1.0 - exp(-2.0 * M_PI * fc / (double)MIX_RATE));
	}
	s_viz_ready = 1;
}

static void viz_tap(const short *pcm, unsigned nframes)
{
	unsigned i;

	if (!s_viz_ready)
		viz_init();
	for (i = 0; i < nframes; i++)
	{
		float x = (float)((int)pcm[i * 2] + (int)pcm[i * 2 + 1]) *
			  (1.0f / 131072.0f);
		int b;
		for (b = 0; b <= VIZ_BANDS; b++)
			s_viz_lp[b] += s_viz_alpha[b] * (x - s_viz_lp[b]);
		for (b = 0; b < VIZ_BANDS; b++)
		{
			float mag = s_viz_lp[b] - s_viz_lp[b + 1];
			if (mag < 0.0f)
				mag = -mag;
			if (mag > s_viz_band[b])
				s_viz_band[b] += (mag - s_viz_band[b]) * 0.5f;
			else
				s_viz_band[b] += (mag - s_viz_band[b]) * 0.08f;
		}
	}
	for (i = 0; i + 8 <= nframes; i += 8)
	{
		int m = ((int)pcm[i * 2] + (int)pcm[i * 2 + 1]) / 2;
		if (m > 32767)
			m = 32767;
		if (m < -32768)
			m = -32768;
		s_scope[s_scope_w & (VIZ_SCOPE - 1)] = (short)m;
		s_scope_w++;
	}
}

static void apply_vol(short *pcm, unsigned nframes)
{
	int i, n = (int)(nframes * 2);
	int vl = g_audio.vol_l, vr = g_audio.vol_r;
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
		g_audio.samples_decoded += s_pending_n;
		s_pending_n = 0;
		return 1;
	}
	w = G.plat->audio_write(s_pending, s_pending_n);
	if (w < 0)
		w = 0;
	if ((unsigned)w >= s_pending_n)
	{
		g_audio.samples_decoded += s_pending_n;
		s_pending_n = 0;
		return 1;
	}
	if (w > 0)
	{
		unsigned left = s_pending_n - (unsigned)w;
		memmove(s_pending, s_pending + (unsigned)w * 2,
			left * 2 * sizeof(short));
		s_pending_n = left;
		g_audio.samples_decoded += (unsigned)w;
	}
	return 0;
}

static unsigned emit_pcm(short *pcm, unsigned nframes)
{
	int w;

	if (!nframes)
		return 0;
	viz_tap(pcm, nframes);
	apply_vol(pcm, nframes);
	if (!G.opt.audio_on || !G.plat || !G.plat->audio_write)
	{
		g_audio.samples_decoded += nframes;
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
	g_audio.samples_decoded += (unsigned)w;
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
	if (s_wav)
	{
		G.plat->free(s_wav);
		s_wav = 0;
	}
	s_wav_n = s_wav_off = 0;
	s_tts_cb[0] = 0;
	g_audio.playing = 0;
	g_audio.paused = 0;
	g_audio.samples_decoded = 0;
	g_audio.name[0] = 0;
	g_audio.owner = -1;
	s_tone_left = 0;
	s_tone_hz_l = s_tone_hz_r = 0;
	s_tone_ph_l = s_tone_ph_r = 0;
	s_pending_n = 0;
}

void mmb_play_stop(void)
{
	s_ended_natural = 0;
	if (G.plat && G.plat->audio_flush)
		G.plat->audio_flush();
	play_teardown();
}

void mmb_play_stop_owned(void)
{
	if (g_audio.owner == g_console)
		mmb_play_stop();
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
	g_audio.playing = kind;
	g_audio.paused = 0;
	g_audio.vol_l = g_audio.vol_r = 100;
	g_audio.samples_decoded = 0;
	g_audio.name[0] = 0;
	g_audio.owner = g_console;
	if (path)
		strncpy(g_audio.name, path, sizeof(g_audio.name) - 1);
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

int mmb_play_wav(const char *path)
{
	unsigned n = 0, pos, data_off = 0, data_sz = 0;
	unsigned char *buf = 0;
	int ch = 1, bits = 16, rate = MIX_RATE, fmt = 1;

	mmb_play_stop();
	if (load_bytes(path, &buf, &n) != 0)
		return -1;
	if (n < 44 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
	{
		G.plat->free(buf);
		return -1;
	}
	pos = 12;
	while (pos + 8 <= n)
	{
		unsigned sz = (unsigned)buf[pos + 4] | ((unsigned)buf[pos + 5] << 8) |
			      ((unsigned)buf[pos + 6] << 16) | ((unsigned)buf[pos + 7] << 24);
		if (memcmp(buf + pos, "fmt ", 4) == 0 && pos + 8 + sz <= n && sz >= 16)
		{
			fmt = buf[pos + 8] | (buf[pos + 9] << 8);
			ch = buf[pos + 10] | (buf[pos + 11] << 8);
			rate = (int)((unsigned)buf[pos + 12] | ((unsigned)buf[pos + 13] << 8) |
				     ((unsigned)buf[pos + 14] << 16) | ((unsigned)buf[pos + 15] << 24));
			bits = buf[pos + 22] | (buf[pos + 23] << 8);
		}
		else if (memcmp(buf + pos, "data", 4) == 0)
		{
			data_off = pos + 8;
			data_sz = sz;
			if (data_off + data_sz > n)
				data_sz = n - data_off;
			break;
		}
		pos += 8 + ((sz + 1) & ~1u);
	}
	if (fmt != 1 || !data_sz || (bits != 8 && bits != 16))
	{
		G.plat->free(buf);
		return -1;
	}
	s_wav = G.plat->alloc(data_sz);
	if (!s_wav)
	{
		G.plat->free(buf);
		return -1;
	}
	memcpy(s_wav, buf + data_off, data_sz);
	G.plat->free(buf);
	s_wav_n = data_sz;
	s_wav_off = 0;
	s_wav_ch = ch;
	s_wav_bits = bits;
	s_wav_rate = rate > 0 ? rate : MIX_RATE;
	play_begin(5, path);
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
		if (due < g_audio.samples_decoded)
			return 0;
		due -= g_audio.samples_decoded;
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

static int mix_wav(unsigned nframes)
{
	short pcm[MIX_CHUNK * 2];
	unsigned i, pos;
	int ch = s_wav_ch < 1 ? 1 : s_wav_ch;
	int bytes = (s_wav_bits / 8) * ch;
	unsigned rate = s_wav_rate > 0 ? (unsigned)s_wav_rate : MIX_RATE;

	if (!s_wav || bytes <= 0)
		return 0;
	pos = s_wav_off;
	for (i = 0; i < nframes; i++)
	{
		unsigned src = (unsigned)((unsigned long)pos * (unsigned long)rate / MIX_RATE);
		unsigned off = src * (unsigned)bytes;
		int l = 0, r = 0;
		if (off + (unsigned)bytes > s_wav_n)
		{
			s_wav_off = s_wav_n;
			if (i)
				emit_pcm(pcm, i);
			return 0;
		}
		if (s_wav_bits == 16)
		{
			l = (short)(s_wav[off] | (s_wav[off + 1] << 8));
			if (ch > 1)
				r = (short)(s_wav[off + 2] | (s_wav[off + 3] << 8));
			else
				r = l;
		}
		else
		{
			l = ((int)s_wav[off] - 128) << 8;
			r = ch > 1 ? ((int)s_wav[off + 1] - 128) << 8 : l;
		}
		pcm[i * 2] = (short)l;
		pcm[i * 2 + 1] = (short)r;
		pos++;
	}
	s_wav_off = pos;
	emit_pcm(pcm, nframes);
	return 1;
}

void mmb_play_mix(void)
{
	unsigned n;
	int keep = 1;
	int loops;

	if (!g_audio.playing || g_audio.paused)
		return;
	if (!flush_pending())
		return;
	for (loops = 0; loops < MIX_FILL_MAX; loops++)
	{
		n = due_frames();
		if (n == 0)
			break;
		if (g_audio.playing == 1 && s_mp3_on)
			keep = mix_mp3(n);
		else if (g_audio.playing == 2 && s_mod_on)
			keep = mix_mod(n);
		else if (g_audio.playing == 3 && s_xm)
			keep = mix_xm(n);
		else if (g_audio.playing == 4)
			keep = mix_tone(n);
		else if (g_audio.playing == 5)
			keep = mix_wav(n);
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
		s_ended_natural = 1;
		play_teardown();
	}
}

void mmb_play_pause(int on)
{
	if (!g_audio.playing)
		return;
	if (on && !g_audio.paused)
	{
		g_audio.paused = 1;
		s_pause_at = mmb_now_ms();
	}
	else if (!on && g_audio.paused)
	{
		g_audio.paused = 0;
		s_mix_origin += mmb_now_ms() - s_pause_at;
	}
}

/* 1 once after the current track reached its end by itself (not PLAY STOP). */
int mmb_play_take_ended(void)
{
	int e = s_ended_natural;
	s_ended_natural = 0;
	return e;
}

void mmb_audio_spectrum(float *bands, int nbands)
{
	int i;

	if (!bands || nbands <= 0)
		return;
	if (!g_audio.playing || g_audio.paused)
	{
		for (i = 0; i < VIZ_BANDS; i++)
			s_viz_band[i] *= 0.6f;
	}
	for (i = 0; i < nbands; i++)
	{
		float v = 0.0f;
		if (i < VIZ_BANDS)
		{
			v = sqrtf(s_viz_band[i] * 12.0f);
			if (v > 1.0f)
				v = 1.0f;
		}
		bands[i] = v;
	}
}

int mmb_audio_scope(short *out, int n)
{
	int i;

	if (!out || n <= 0)
		return 0;
	if (n > VIZ_SCOPE)
		n = VIZ_SCOPE;
	for (i = 0; i < n; i++)
	{
		unsigned idx = (s_scope_w - (unsigned)n + (unsigned)i) &
			       (unsigned)(VIZ_SCOPE - 1);
		out[i] = s_scope[idx];
	}
	return n;
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
		if (g_audio.playing && !g_audio.paused)
		{
			g_audio.paused = 1;
			s_pause_at = mmb_now_ms();
		}
		return;
	}
	if (mmb_match("RESUME"))
	{
		if (g_audio.playing && g_audio.paused)
		{
			g_audio.paused = 0;
			s_mix_origin += mmb_now_ms() - s_pause_at;
		}
		return;
	}
	if (mmb_match("VOLUME"))
	{
		g_audio.vol_l = (int)mmb_as_int(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			g_audio.vol_r = (int)mmb_as_int(mmb_expr());
		}
		else
			g_audio.vol_r = g_audio.vol_l;
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
			return;
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
	if (mmb_match("WAV") || mmb_match("EFFECT") || mmb_match("SOUND"))
	{
		mmb_val v = mmb_expr();
		if (v.type != T_STR)
			mmb_syntax();
		if (mmb_play_wav(v.s) != 0)
			return; /* missing/unsupported clip: keep the program running */
		return;
	}
	if (mmb_match("TTS") || mmb_match("SPEAK"))
	{
		mmb_val v;
		char cb[MMB_MAX_NAME];
		cb[0] = 0;
		v = mmb_expr();
		(void)v;
		mmb_skip_sp();
		while (*G.p == ',')
		{
			G.p++;
			mmb_skip_sp();
			if (*G.p == ',' || *G.p == 0 || *G.p == ':' || *G.p == '\'')
				continue;
			if (((*G.p >= 'A' && *G.p <= 'Z') || (*G.p >= 'a' && *G.p <= 'z') || *G.p == '_') &&
			    strchr(G.p, '(') != G.p)
			{
				const char *save = G.p;
				mmb_ident(cb, sizeof(cb));
				mmb_type_suffix(cb);
				mmb_skip_sp();
				if (*G.p == 0 || *G.p == ':' || *G.p == '\'' || *G.p == ',')
					continue;
				G.p = save;
				cb[0] = 0;
			}
			(void)mmb_expr();
			mmb_skip_sp();
		}
		mmb_play_stop();
		s_tone_hz_l = s_tone_hz_r = 660;
		s_tone_ph_l = s_tone_ph_r = 0;
		s_tone_left = (unsigned)(80ul * MIX_RATE / 1000u);
		if (s_tone_left == 0)
			s_tone_left = 1;
		play_begin(4, "TTS");
		if (cb[0])
			mmb_call_named_sub(cb);
		return;
	}
	mmb_syntax();
}

void mmb_cmd_beep(void)
{
	/* QuickBasic BEEP. Optional frequency (Hz) and duration (ms). */
	double hz = 800.0;
	unsigned dur = 120;
	mmb_skip_sp();
	if (*G.p && *G.p != ':' && *G.p != '\'')
	{
		hz = mmb_as_float(mmb_expr());
		mmb_skip_sp();
		if (*G.p == ',')
		{
			G.p++;
			dur = (unsigned)mmb_as_int(mmb_expr());
		}
	}
	if (hz <= 0)
		hz = 800.0;
	if (dur == 0)
		dur = 120;
	mmb_play_stop();
	s_tone_hz_l = s_tone_hz_r = hz;
	s_tone_ph_l = s_tone_ph_r = 0;
	s_tone_left = (unsigned)((unsigned long)dur * MIX_RATE / 1000u);
	if (s_tone_left == 0)
		s_tone_left = 1;
	play_begin(4, "BEEP");
}

#if !defined(MMB_PLATFORM_POSIX)
/* jar_xm's stdio API is unused (we call jar_xm_create_context_safe), but its
 * strong definitions would interpose on libc in a native build. */
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
#endif /* !MMB_PLATFORM_POSIX */
