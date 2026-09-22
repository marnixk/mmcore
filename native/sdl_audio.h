/*
 * SDL2 audio backend for the native build (LN-10).
 *
 * Queues the interpreter's 44100 Hz interleaved stereo s16 through SDL. The
 * queue-occupancy hooks must stay accurate: mmbasic/src/cmd_play.c paces its
 * mixer off audio_queued_frames()/audio_free_frames().
 */
#ifndef MMB_SDL_AUDIO_H
#define MMB_SDL_AUDIO_H

void sdl_audio_set_target(int target);
void sdl_audio_enable(int on);
int sdl_audio_write(const short *stereo_s16, unsigned nframes);
unsigned sdl_audio_free_frames(void);
unsigned sdl_audio_queued_frames(void);
int sdl_audio_have_device(void);
void sdl_audio_kick(void);
void sdl_audio_flush(void);
void sdl_audio_shutdown(void);

#endif
