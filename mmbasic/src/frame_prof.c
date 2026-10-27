#include "frame_prof.h"
#include "mmb_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MMB_PLATFORM_POSIX)
#include <time.h>
#endif

#ifndef MMB_FP_MAX_FRAMES
#define MMB_FP_MAX_FRAMES 4096
#endif

enum {
	FP_INTERP = 0,
	FP_DRAW = 1,
	FP_PRESENT = 2,
	FP_WAIT = 3,
	FP_PHASES = 4
};

typedef struct {
	uint32_t interp_us;
	uint32_t draw_us;
	uint32_t present_us;
	uint32_t wait_us;
	uint32_t stmts;
	uint32_t present_calls;
	uint64_t wall_us;
} fp_frame;

static fp_frame s_frames[MMB_FP_MAX_FRAMES];
static unsigned s_nframes;
static int s_active;
static int s_phase;
static int s_depth_draw;
static int s_depth_present;
static int s_depth_wait;
static uint64_t s_phase_t0_us;
static uint64_t s_run_t0_us;
static uint64_t s_sum_us[FP_PHASES];
static uint32_t s_frame_stmts;
static uint64_t s_clock_reads;
static char s_outpath[128];

#if !defined(MMB_PLATFORM_POSIX)
unsigned long long mmb_circle_now_us(void);
#endif

unsigned long long mmb_now_us(void)
{
#if defined(MMB_PLATFORM_POSIX)
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull +
	       (unsigned long long)ts.tv_nsec / 1000ull;
#else
	return mmb_circle_now_us();
#endif
}

static void ensure_active(void)
{
	if (!G.opt.frame_profile || !G.running || s_active)
		return;
	mmb_frame_prof_run_begin();
}

static int enabled(void)
{
	ensure_active();
	return s_active && G.running;
}

static void clock_sample(void)
{
	s_clock_reads++;
}

static fp_frame *cur_frame(void)
{
	if (s_nframes >= MMB_FP_MAX_FRAMES)
		return 0;
	return &s_frames[s_nframes];
}

static void phase_flush(uint64_t now_us)
{
	uint64_t dt;
	uint32_t add;
	fp_frame *f;

	if (!enabled() || s_phase_t0_us == 0)
		return;
	dt = now_us - s_phase_t0_us;
	if (dt > 0xffffffffull)
		dt = 0xffffffffull;
	add = (uint32_t)dt;
	s_sum_us[s_phase] += dt;
	f = cur_frame();
	if (f)
	{
		switch (s_phase)
		{
		case FP_DRAW:
			f->draw_us += add;
			break;
		case FP_PRESENT:
			f->present_us += add;
			break;
		case FP_WAIT:
			f->wait_us += add;
			break;
		default:
			f->interp_us += add;
			break;
		}
	}
	s_phase_t0_us = now_us;
}

static void set_phase(int phase)
{
	uint64_t now;

	if (!enabled())
		return;
	clock_sample();
	now = mmb_now_us();
	clock_sample();
	phase_flush(now);
	s_phase = phase;
}

static void frame_close(void)
{
	fp_frame *f;
	uint64_t now;

	if (!enabled() || s_nframes >= MMB_FP_MAX_FRAMES)
		return;
	clock_sample();
	now = mmb_now_us();
	clock_sample();
	phase_flush(now);
	f = &s_frames[s_nframes];
	f->stmts = s_frame_stmts;
	f->present_calls = 1;
	f->wall_us = now - s_run_t0_us;
	s_nframes++;
	s_frame_stmts = 0;
	s_run_t0_us = now;
	s_phase_t0_us = now;
}

void mmb_frame_prof_set_output_path(const char *path)
{
	if (!path || !*path)
		return;
	strncpy(s_outpath, path, sizeof(s_outpath) - 1);
	s_outpath[sizeof(s_outpath) - 1] = 0;
}

static void default_path(void)
{
	char letter;

	if (s_outpath[0])
		return;
	letter = G.drive ? (char)G.drive : 'C';
	snprintf(s_outpath, sizeof(s_outpath), "%c:/.mmcore-frame-profile.csv",
		 letter);
}

static int env_truthy(const char *v)
{
	return v && (*v == '1' || *v == 'y' || *v == 'Y' ||
		     (*v == 'o' && (v[1] == 'n' || v[1] == 'N')));
}

void mmb_frame_prof_env_init(void)
{
	const char *env = getenv("MMCORE_FRAME_PROFILE");

	s_outpath[0] = 0;
	if (!env || !*env)
		return;
	if (env_truthy(env))
		G.opt.frame_profile = 1;
	else
	{
		G.opt.frame_profile = 1;
		mmb_frame_prof_set_output_path(env);
	}
}

void mmb_frame_prof_run_begin(void)
{
	unsigned i;

	if (!G.opt.frame_profile || s_active)
		return;
	if (G.opt.frame_profile_path[0])
		mmb_frame_prof_set_output_path(G.opt.frame_profile_path);
	s_active = 1;
	s_nframes = 0;
	s_phase = FP_INTERP;
	s_depth_draw = 0;
	s_depth_present = 0;
	s_depth_wait = 0;
	s_frame_stmts = 0;
	s_clock_reads = 0;
	for (i = 0; i < FP_PHASES; i++)
		s_sum_us[i] = 0;
	clock_sample();
	s_run_t0_us = mmb_now_us();
	s_phase_t0_us = s_run_t0_us;
	clock_sample();
}

static void write_csv(void)
{
	char line[160];
	char *chunk;
	unsigned pos = 0;
	unsigned cap;
	unsigned i;
	const char *path;
	uint64_t total_us = 0;

	if (!G.opt.frame_profile || !s_nframes)
		return;
	default_path();
	path = s_outpath;
	for (i = 0; i < FP_PHASES; i++)
		total_us += s_sum_us[i];

	cap = 512u + s_nframes * 80u;
	chunk = (char *)malloc(cap);
	if (!chunk)
		return;
	chunk[0] = 0;
	pos = (unsigned)snprintf(chunk, cap,
				 "# mmcore-frame-profile version=1\n"
				 "# frames=%u clock_reads=%llu\n"
				 "# sum_interp_us=%llu sum_draw_us=%llu "
				 "sum_present_us=%llu sum_wait_us=%llu "
				 "sum_total_us=%llu\n"
				 "index,wall_us,interp_us,draw_us,present_us,"
				 "wait_us,stmts,present_calls\n",
				 s_nframes, (unsigned long long)s_clock_reads,
				 (unsigned long long)s_sum_us[FP_INTERP],
				 (unsigned long long)s_sum_us[FP_DRAW],
				 (unsigned long long)s_sum_us[FP_PRESENT],
				 (unsigned long long)s_sum_us[FP_WAIT],
				 (unsigned long long)total_us);
	for (i = 0; i < s_nframes; i++)
	{
		const fp_frame *f = &s_frames[i];
		int n;

		if (pos + 72u >= cap)
			break;
		n = snprintf(chunk + pos, cap - pos,
			     "%u,%llu,%u,%u,%u,%u,%u,%u\n", i,
			     (unsigned long long)f->wall_us, f->interp_us,
			     f->draw_us, f->present_us, f->wait_us, f->stmts,
			     f->present_calls);
		if (n <= 0)
			break;
		pos += (unsigned)n;
	}
	if (mmb_vfs_write(path, chunk, pos, 0) < 0)
	{
		snprintf(s_outpath, sizeof(s_outpath),
			 "A:/.mmcore-frame-profile.csv");
		(void)mmb_vfs_write(s_outpath, chunk, pos, 0);
	}
	free(chunk);
}

void mmb_frame_prof_run_end(void)
{
	if (!s_active)
		return;
	clock_sample();
	phase_flush(mmb_now_us());
	clock_sample();
	write_csv();
	s_active = 0;
}

void mmb_frame_prof_draw_begin(void)
{
	if (!enabled())
		return;
	if (s_depth_draw++ == 0)
		set_phase(FP_DRAW);
}

void mmb_frame_prof_draw_end(void)
{
	if (!enabled() || s_depth_draw <= 0)
		return;
	if (--s_depth_draw == 0)
		set_phase(FP_INTERP);
}

void mmb_frame_prof_present_begin(void)
{
	if (!enabled())
		return;
	if (s_depth_present++ == 0)
		set_phase(FP_PRESENT);
}

void mmb_frame_prof_present_end(void)
{
	if (!enabled() || s_depth_present <= 0)
		return;
	if (--s_depth_present == 0)
	{
		frame_close();
		set_phase(FP_INTERP);
	}
}

void mmb_frame_prof_wait_begin(void)
{
	if (!enabled())
		return;
	if (s_depth_wait++ == 0)
		set_phase(FP_WAIT);
}

void mmb_frame_prof_wait_end(void)
{
	if (!enabled() || s_depth_wait <= 0)
		return;
	if (--s_depth_wait == 0)
		set_phase(FP_INTERP);
}

void mmb_frame_prof_stmt_tick(void)
{
	if (!enabled())
		return;
	s_frame_stmts++;
}
