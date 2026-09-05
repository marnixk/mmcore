#ifndef CONSOLE_AUDIO_H
#define CONSOLE_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_TARGET_JACK 0
#define AUDIO_TARGET_HDMI 1
#define AUDIO_RATE        44100

void audio_init(void);
void audio_set_target(int target);
void audio_enable(int on);
int audio_write(const short *stereo_s16, unsigned nframes);
unsigned audio_free_frames(void);

#ifdef __cplusplus
}
#endif

#endif
