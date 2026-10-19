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
#include "sdl_harness.h"
#include "paint.h"

#include <SDL.h>

#ifndef _WIN32
#include <dirent.h>
#endif
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

void mmb_platform_bind_sdl(void);

/*
 * Boot logging that survives the ISO's stderr redirect.
 *
 * root/.profile sends mmcore's stderr to /tmp/mmcore.stderr, which is
 * unreadable when the panel never lights up. Mirror startup diagnostics to the
 * kernel console (/dev/console is the serial tty on the live ISO: the kernel
 * line ends in `console=ttyS0`) and to ttyS0 directly, so a reporter stuck on
 * the splash can still read why over serial (#1032).
 */
static void boot_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;
#ifndef _WIN32
	static const char *const sinks[] = { "/dev/console", "/dev/ttyS0" };
	size_t i;
#endif

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof buf)
		n = (int)sizeof buf - 1;
	fputs(buf, stderr);
	fflush(stderr);
#ifndef _WIN32
	for (i = 0; i < sizeof sinks / sizeof sinks[0]; i++)
	{
		int fd = open(sinks[i], O_WRONLY | O_NOCTTY);

		if (fd >= 0)
		{
			(void)write(fd, buf, (size_t)n);
			close(fd);
		}
	}
#endif
}

#ifndef _WIN32
/* List the DRM nodes so a "no KMS/DRM" report carries the facts. */
static void boot_log_drm(void)
{
	DIR *d = opendir("/dev/dri");
	struct dirent *e;

	boot_log("mmcore: /dev/dri %s\n", d ? "contains:" : "is missing");
	if (!d)
		return;
	while ((e = readdir(d)) != 0)
	{
		if (e->d_name[0] == '.')
			continue;
		boot_log("mmcore:   /dev/dri/%s\n", e->d_name);
	}
	closedir(d);
}

/*
 * #1032: a hung KMS/DRM modeset can block SDL_Init/CreateWindow/
 * CreateRenderer forever, leaving the splash on screen with no diagnostic and
 * no respawn (tty1 never sees mmcore exit). Bound the video bring-up: on
 * timeout, log to stderr and the serial consoles and _exit(2), so the tty1
 * respawn retries instead of hanging. MMCORE_VIDEO_TIMEOUT=0 disables (e.g. a
 * debugger), and any positive value overrides the 30 s default.
 */
static void video_watchdog(int sig)
{
	static const char msg[] =
		"mmcore: SDL video init timed out (KMS/DRM modeset stuck); "
		"exiting so tty1 retries. See docs/framebuffer-and-iso.md "
		"'Intel Chromebooks'.\n";
	static const char *const sinks[] = { "/dev/console", "/dev/ttyS0" };
	size_t i;

	(void)sig;
	(void)write(2, msg, sizeof msg - 1);
	for (i = 0; i < sizeof sinks / sizeof sinks[0]; i++)
	{
		int fd = open(sinks[i], O_WRONLY | O_NOCTTY);

		if (fd >= 0)
		{
			(void)write(fd, msg, sizeof msg - 1);
			close(fd);
		}
	}
	_exit(2);
}

static void video_watchdog_arm(void)
{
	const char *env = getenv("MMCORE_VIDEO_TIMEOUT");
	long secs = (env && env[0]) ? strtol(env, 0, 10) : 30;
	struct sigaction sa;

	if (secs <= 0)
		return;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = video_watchdog;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGALRM, &sa, 0);
	alarm((unsigned)secs);
}

static void video_watchdog_disarm(void)
{
	alarm(0);
	signal(SIGALRM, SIG_DFL);
}
#else
static void boot_log_drm(void) {}
static void video_watchdog_arm(void) {}
static void video_watchdog_disarm(void) {}
#endif

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

	video_watchdog_arm();
	if (!sdl_video_open(640, 480))
	{
		video_watchdog_disarm();
		boot_log("mmcore: could not open SDL window: %s\n",
			 SDL_GetError());
		boot_log_drm();
		return 1;
	}
	video_watchdog_disarm();
	/* --double: open a 2x windowed client (integer, nearest-neighbour via
	 * sdl_scale_viewport). Set before fullscreen so leaving fullscreen
	 * restores the 2x window, not 1:1; headless builds ignore it. */
	if (cli->double_scale)
		sdl_video_set_window_scale(cli->double_scale);
	/* --fullscreen: enter fullscreen before the first frame so the boot
	 * banner is already fullscreen. Alt+Enter still toggles. */
	if (cli->fullscreen)
		sdl_video_set_fullscreen(1);

	mmb_platform_bind_sdl();
	/* Test-only override (#633): pretend a mouse is attached so headless
	 * harnesses can enter PAINT. Unset on real targets: default unchanged. */
	{
		const char *force = getenv("MMB_PAINT_FORCE_MOUSE");

		if (force && force[0] && strcmp(force, "0") != 0)
			pt_force_mouse(1);
	}
	sdl_harness_init();
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
		sdl_harness_poll(); /* test-only: one scripted step per frame */
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
