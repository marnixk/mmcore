#ifndef MMB_FRAME_PROF_H
#define MMB_FRAME_PROF_H

/* Opt-in frame-phase profiler (MMCORE_FRAME_PROFILE / OPTION FRAMEPROFILE).
 * Collects in RAM only; writes CSV once when a RUN ends or is broken. */

void mmb_frame_prof_env_init(void);
void mmb_frame_prof_run_begin(void);
void mmb_frame_prof_run_end(void);

void mmb_frame_prof_draw_begin(void);
void mmb_frame_prof_draw_end(void);
void mmb_frame_prof_present_begin(void);
void mmb_frame_prof_present_end(void);
void mmb_frame_prof_wait_begin(void);
void mmb_frame_prof_wait_end(void);

void mmb_frame_prof_set_output_path(const char *path);

#endif
