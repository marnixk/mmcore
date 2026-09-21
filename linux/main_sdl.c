/*
 * SDL2 entry point (LN-03). Opens the window and runs a minimal loop so the
 * backend can be exercised; the full front-end/event routing arrives in LN-09.
 *
 * Input is read from stdin without blocking the window. The process exits on
 * window close (SDL_QUIT) or stdin EOF, which makes it testable headlessly
 * with SDL_VIDEODRIVER=dummy.
 */
#include "mmb_priv.h"
#include "sdl_video.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

void mmb_platform_bind_sdl(void);

static void emit(const char *s)
{
	/* Both serial (stdout) and the SDL console. */
	mmb_console_write(s);
}

static void run_line(const char *line)
{
	const char *result = mmb_exec_line(line);

	if (result && *result)
		emit(result);
	emit("\n");
	fflush(stdout);
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
	mmb_print_startup();
	emit(mmb_prompt());
	sdl_video_present();

	while (!sdl_video_should_quit())
	{
		if (sdl_video_pump() < 0)
			break;

		if (stdin_open && stdin_line_ready())
		{
			if (!read_stdin_line(line, (int)sizeof line))
				break; /* stdin closed (headless/pipe): exit */
			run_line(line);
			emit(mmb_prompt());
			sdl_video_present();
		}
		else
		{
			SDL_Delay(5);
		}
	}

	if (getenv("MMB_SDL_DUMP"))
		sdl_video_dump_ppm(getenv("MMB_SDL_DUMP"));

	sdl_video_close();
	return 0;
}
