/*
 * mmcore-jack - report the headphone/microphone/line-out jack switch state.
 *
 * On many Chromebooks the 3.5 mm jack is not an ALSA kcontrol at all: the
 * codec/EC reports plug/unplug as an input switch (EV_SW /
 * SW_HEADPHONE_INSERT) on /dev/input/event*, normally consumed by a sound
 * server. The AC97/HDA "Headphone Jack" kcontrol the #1059 helper looked for
 * simply does not exist, so the helper no-ops on those boards.
 *
 * BusyBox has no way to read a switch state, so this tiny EVIOCGSW reader
 * supplies it. The audio boot helper finds the event devices with their
 * handlers in /proc/bus/input/devices and passes them as arguments; with no
 * arguments every /dev/input/event* is probed.
 *
 * Output (one line; only switches the device actually supports are printed):
 *   headphone=0 microphone=1 lineout=0
 *
 * Exit status is 0 when at least one switch-capable device was read, and 1
 * when no switch device was found, so the caller can fall back to an ALSA
 * kcontrol without treating "no switch" as "no headphones".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef SW_MAX
#define SW_MAX 0x0f
#endif

#define NIPBITS (8 * sizeof(unsigned long))
#define NBITS ((SW_MAX / NIPBITS) + 1)

#define BIT_SET(bitmap, bit) \
	(((const unsigned long *)(bitmap))[(bit) / NIPBITS] & \
	 (1UL << ((bit) % NIPBITS)))

struct jack_state {
	int seen_headphone;
	int headphone;
	int seen_mic;
	int microphone;
	int seen_lineout;
	int lineout;
};

static void read_device(const char *path, struct jack_state *st, int *found)
{
	unsigned long caps[NBITS] = {0};
	unsigned long sw[NBITS] = {0};
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd < 0)
		return;
	if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(caps)), caps) == 0 &&
	    ioctl(fd, EVIOCGSW(sizeof(sw)), sw) == 0) {
		if (BIT_SET(caps, SW_HEADPHONE_INSERT)) {
			st->seen_headphone = 1;
			*found = 1;
			if (BIT_SET(sw, SW_HEADPHONE_INSERT))
				st->headphone = 1;
		}
		if (BIT_SET(caps, SW_MICROPHONE_INSERT)) {
			st->seen_mic = 1;
			*found = 1;
			if (BIT_SET(sw, SW_MICROPHONE_INSERT))
				st->microphone = 1;
		}
		if (BIT_SET(caps, SW_LINEOUT_INSERT)) {
			st->seen_lineout = 1;
			*found = 1;
			if (BIT_SET(sw, SW_LINEOUT_INSERT))
				st->lineout = 1;
		}
	}
	close(fd);
}

int main(int argc, char **argv)
{
	struct jack_state st = {0};
	int found = 0;
	int i;

	if (argc > 1) {
		for (i = 1; i < argc; i++)
			read_device(argv[i], &st, &found);
	} else {
		glob_t g;

		if (glob("/dev/input/event*", 0, NULL, &g) == 0) {
			for (i = 0; (size_t)i < g.gl_pathc; i++)
				read_device(g.gl_pathv[i], &st, &found);
			globfree(&g);
		}
	}

	if (!found)
		return 1;

	if (st.seen_headphone)
		printf("headphone=%d ", st.headphone);
	if (st.seen_mic)
		printf("microphone=%d ", st.microphone);
	if (st.seen_lineout)
		printf("lineout=%d ", st.lineout);
	printf("\n");
	return 0;
}
