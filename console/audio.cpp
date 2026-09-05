#include "audio.h"
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/new.h>

#define AUDIO_QUEUE_MS   200
#define HDMI_CHUNK       (384 * 10)
#define PWM_CHUNK        2048

static CSoundBaseDevice *s_dev;
static int s_target = AUDIO_TARGET_HDMI;
static int s_enabled = 1;
static int s_started;

#ifndef NO_SDHOST
static CSoundBaseDevice *make_dev(int target)
{
	CInterruptSystem *irq = CInterruptSystem::Get();
	if (!irq)
		return 0;
	if (target == AUDIO_TARGET_JACK)
		return new CPWMSoundBaseDevice(irq, AUDIO_RATE, PWM_CHUNK);
	return new CHDMISoundBaseDevice(irq, AUDIO_RATE, HDMI_CHUNK);
}
#endif

static void close_dev(void)
{
	unsigned n;

	if (!s_dev)
		return;
	if (s_started)
	{
		s_dev->Cancel();
		for (n = 0; n < 20 && s_dev->IsActive(); n++)
			CTimer::SimpleMsDelay(5);
	}
	delete s_dev;
	s_dev = 0;
	s_started = 0;
}

static void open_dev(void)
{
	if (s_dev)
		return;
#ifdef NO_SDHOST
	/* QEMU has no PWM jack or HDMI audio DMA. Decode still runs. */
	return;
#else
	s_dev = make_dev(s_target);
	if (!s_dev)
		return;
	if (!s_dev->AllocateQueue(AUDIO_QUEUE_MS))
	{
		delete s_dev;
		s_dev = 0;
		return;
	}
	s_dev->SetWriteFormat(SoundFormatSigned16, 2);
	if (s_enabled && s_dev->Start())
		s_started = 1;
#endif
}

void audio_init(void)
{
	s_target = AUDIO_TARGET_HDMI;
	s_enabled = 1;
	s_started = 0;
	s_dev = 0;
}

void audio_set_target(int target)
{
	if (target != AUDIO_TARGET_JACK)
		target = AUDIO_TARGET_HDMI;
	if (target == s_target && s_dev)
		return;
	close_dev();
	s_target = target;
	if (s_enabled)
		open_dev();
}

void audio_enable(int on)
{
	s_enabled = on ? 1 : 0;
	if (!s_enabled)
		close_dev();
	else
		open_dev();
}

int audio_write(const short *stereo_s16, unsigned nframes)
{
	int n;

	if (!stereo_s16 || nframes == 0)
		return 0;
	if (!s_enabled)
		return (int)nframes;
	open_dev();
	if (!s_dev)
		return (int)nframes;
	n = s_dev->Write(stereo_s16, nframes * 2 * sizeof(short));
	if (n < 0)
		return 0;
	return n / (int)(2 * sizeof(short));
}

unsigned audio_free_frames(void)
{
	unsigned q, used;

	if (!s_enabled)
		return 2048;
	open_dev();
	if (!s_dev)
		return 2048;
	q = s_dev->GetQueueSizeFrames();
	used = s_dev->GetQueueFramesAvail();
	return q > used ? q - used : 0;
}
