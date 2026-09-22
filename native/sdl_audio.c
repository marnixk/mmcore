#include "sdl_audio.h"

#include <SDL.h>

#define AUDIO_RATE 44100
#define BYTES_PER_FRAME (2 * (int)sizeof(short))
/* Queue ceiling (frames). Keeps latency bounded and gives the mixer a cap. */
#define QUEUE_CAP_FRAMES (AUDIO_RATE / 2) /* 500 ms */

static SDL_AudioDeviceID s_dev;
static int s_enabled = 1;

static int open_dev(void)
{
	SDL_AudioSpec want, have;

	if (s_dev)
		return 1;
	if (!s_enabled)
		return 0;
	SDL_zero(want);
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = 0; /* queue API */
	s_dev = SDL_OpenAudioDevice(0, 0, &want, &have, 0);
	if (!s_dev)
		return 0;
	SDL_PauseAudioDevice(s_dev, 0);
	return 1;
}

static void close_dev(void)
{
	if (s_dev)
	{
		SDL_CloseAudioDevice(s_dev);
		s_dev = 0;
	}
}

void sdl_audio_set_target(int target)
{
	/* The native build uses one default device for both HDMI/JACK. */
	(void)target;
}

void sdl_audio_enable(int on)
{
	/* Lazy: only actually open the device when something plays, so booting
	 * (or running tests) on a machine with no audio stays quiet/fast. */
	s_enabled = on ? 1 : 0;
	if (!on)
		close_dev();
}

int sdl_audio_write(const short *stereo_s16, unsigned nframes)
{
	Uint32 queued;

	if (!stereo_s16 || nframes == 0)
		return 0;
	if (!s_dev && !open_dev())
		return (int)nframes; /* no device: pretend consumed */
	queued = SDL_GetQueuedAudioSize(s_dev) / BYTES_PER_FRAME;
	if (queued >= QUEUE_CAP_FRAMES)
		return 0; /* backpressure */
	if (SDL_QueueAudio(s_dev, stereo_s16, nframes * BYTES_PER_FRAME) != 0)
		return 0;
	return (int)nframes;
}

unsigned sdl_audio_free_frames(void)
{
	Uint32 queued;

	if (!s_dev)
		return 2048;
	queued = SDL_GetQueuedAudioSize(s_dev) / BYTES_PER_FRAME;
	return queued < QUEUE_CAP_FRAMES ? QUEUE_CAP_FRAMES - queued : 0;
}

unsigned sdl_audio_queued_frames(void)
{
	if (!s_dev)
		return 0;
	return SDL_GetQueuedAudioSize(s_dev) / BYTES_PER_FRAME;
}

int sdl_audio_have_device(void)
{
	return s_dev ? 1 : 0;
}

void sdl_audio_kick(void)
{
	if (!s_dev)
		open_dev();
	if (s_dev)
		SDL_PauseAudioDevice(s_dev, 0);
}

void sdl_audio_flush(void)
{
	if (s_dev)
		SDL_ClearQueuedAudio(s_dev);
}

void sdl_audio_shutdown(void)
{
	close_dev();
}
