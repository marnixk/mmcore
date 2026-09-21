/*
 * SDL2 entry point (LN-09). Drives the portable front end (mmb_front_*) from
 * SDL keyboard input and the terminal, renders through the SDL platform, and
 * keeps the window alive while a program runs (via platform poll_input).
 *
 * Alt+Enter at the prompt toggles fullscreen on the primary display.
 * stdin is still accepted (one line at a time) so the binary is testable
 * headlessly; EOF on a non-tty exits.
 */
#include "mmb_priv.h"
#include "frontend.h"
#include "sdl_video.h"
#include "sdl_input.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

void mmb_platform_bind_sdl(void);

static void front_emit(void *ctx, const char *s, unsigned n)
{
	(void)ctx;
	if (!G.plat)
		return;
	if (G.plat->write_serial)
		G.plat->write_serial(s, n);
	if (G.plat->write_screen)
		G.plat->write_screen(s, n);
}

static int stdin_line_ready(void)
{
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(0, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	return select(1, &rfds, 0, 0, &tv) > 0;
}

static int read_stdin_line(char *line, int cap)
{
	char buf[512];
	size_t n = 0;
	int c;

	while ((c = fgetc(stdin)) != EOF)
	{
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n + 1 < sizeof buf)
			buf[n++] = (char)c;
	}
	if (c == EOF && n == 0)
		return 0;
	buf[n] = 0;
	snprintf(line, (size_t)cap, "%s", buf);
	return 1;
}

int main(void)
{
	char line[MMB_LINE_LEN];
	int stdin_open = 1;

	if (!sdl_video_open(640, 480))
	{
		fprintf(stderr, "mmbasic: could not open SDL window: %s\n",
			SDL_GetError());
		return 1;
	}

	mmb_platform_bind_sdl();
	SDL_StartTextInput();
	mmb_front_init(front_emit, 0);
	mmb_print_startup();
	mmb_front_prompt();
	sdl_video_present();

	while (!sdl_video_should_quit())
	{
		sdl_input_pump();
		mmb_poll(); /* CONNECT/TERM/FTP, audio mix, ON TICK at the prompt */

		if (stdin_open && stdin_line_ready())
		{
			if (!read_stdin_line(line, (int)sizeof line))
				break; /* stdin closed (headless/pipe): exit */
			mmb_front_feed(line, (unsigned)strlen(line));
			mmb_front_feed_byte('\n');
		}
		else
		{
			SDL_Delay(5);
		}
		sdl_video_present();
	}

	if (getenv("MMB_SDL_DUMP"))
		sdl_video_dump_ppm(getenv("MMB_SDL_DUMP"));

	sdl_video_close();
	return 0;
}
