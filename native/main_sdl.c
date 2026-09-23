/*
 * SDL2 entry point (LN-09). Drives the portable front end (mmb_front_*) from
 * SDL keyboard input and the terminal, renders through the SDL platform, and
 * keeps the window alive while a program runs (via platform poll_input).
 *
 * Alt+Enter at the prompt toggles fullscreen on the primary display.
 * stdin is still accepted (one line at a time) so the binary is testable
 * headlessly; EOF on a non-tty exits.
 *
 * App-VM modes (issue #490/#491): a positional `path.app` runs the package as
 * a self-contained app, and `--term [host[:port]]` starts a sealed TERM
 * session. Both exit the process when they end unless `--repl`/`--stay`;
 * while sealed they never paint the REPL prompt and swallow BREAK.
 */
#include "win_compat.h"

#include "mmb_priv.h"
#include "frontend.h"
#include "session.h"
#include "cli.h"
#include "sdl_video.h"
#include "sdl_input.h"
#include "sdl_clipboard.h"

#include <SDL.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void run_line(const char *line)
{
	const char *result = mmb_exec_line(line);

	if (result && result[0])
		front_emit(0, result, (unsigned)strlen(result));
}

static int stdin_line_ready(void)
{
#ifdef _WIN32
	return mmb_stdin_ready();
#else
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(0, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	return select(1, &rfds, 0, 0, &tv) > 0;
#endif
}

/* A GUI launch (desktop entry / file manager) gives the process /dev/null or
 * a closed stdin. That is an immediate EOF, which must not quit the app; only
 * a real tty or pipe should drive the REPL. */
static int stdin_usable(void)
{
	struct stat st, nul;

	if (fstat(0, &st) != 0)
		return 0;
	if (!S_ISCHR(st.st_mode))
		return 1; /* tty, pipe, or regular file */
	if (stat("/dev/null", &nul) == 0 && st.st_rdev == nul.st_rdev)
		return 0; /* /dev/null */
	return 1; /* a tty */
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

int main(int argc, char **argv)
{
	char line[MMB_LINE_LEN];
	const struct mmb_cli_opts *cli;
	int stdin_open, sealed, app_mode, term_mode;

	cli = mmb_cli_parse(argc, argv);
	app_mode = cli->mode == MMB_CLI_APP;
	term_mode = cli->mode == MMB_CLI_TERM;
	sealed = (app_mode || term_mode) && !cli->stay;
	stdin_open = stdin_usable();

	if (!sdl_video_open(640, 480))
	{
		fprintf(stderr, "mmbasic: could not open SDL window: %s\n",
			SDL_GetError());
		return 1;
	}

	mmb_platform_bind_sdl();
	sdl_clipboard_init();
	SDL_StartTextInput();
	mmb_front_init(front_emit, 0);
	mmb_console_init();
	if (sealed)
	{
		mmb_front_set_sealed(1);
		/* BREAK (terminal Ctrl+C) must not interrupt a sealed session. */
		signal(SIGINT, SIG_IGN);
	}
	mmb_print_startup();

	if (app_mode)
	{
		const char *cmd = mmb_cli_app_run_line();

		if (!cmd)
		{
			fprintf(stderr, "mmbasic: cannot run '%s'\n",
				cli->app_path ? cli->app_path : "?");
			sdl_video_close();
			return 2;
		}
		run_line(cmd);
		if (!cli->stay)
		{
			sdl_video_present();
			sdl_video_close();
			return 0;
		}
	}
	else if (term_mode)
	{
		run_line(mmb_cli_term_run_line());
		if (!cli->stay && !mmb_in_term())
		{
			sdl_video_present();
			sdl_video_close();
			return 0; /* TERM never started */
		}
	}
	else if (cli->mode == MMB_CLI_LINE && cli->line)
	{
		run_line(cli->line);
		sdl_video_present();
		sdl_video_close();
		return 0;
	}
	else
	{
		mmb_front_prompt();
	}
	sdl_video_present();

	while (!sdl_video_should_quit())
	{
		sdl_input_pump();
		mmb_poll(); /* CONNECT/TERM/FTP, audio mix, ON TICK at the prompt */
		mmb_console_poll();

		if (term_mode && !mmb_in_term())
			break; /* sealed TERM session ended */

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
		if (mmb_take_quit())
			break; /* QUIT: end the application */
		sdl_video_present();
	}

	if (getenv("MMB_SDL_DUMP"))
		sdl_video_dump_ppm(getenv("MMB_SDL_DUMP"));

	sdl_video_close();
	return 0;
}
