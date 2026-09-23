#include "win_compat.h"

#include "cli.h"

#include "storage_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* --drive DIR binds the host directory DIR to the D: drive. */
static int parse_drive(const char *dir)
{
	if (!dir || !dir[0])
		return -1;
	return storage_posix_mount('D', dir);
}

static struct mmb_cli_opts s_opts;
static int s_drive_used;
static char s_term_host[256];
static char s_term_line[320];
static char s_app_line[512];

static int is_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_app_name(const char *path)
{
	size_t n = strlen(path);

	return n >= 4 && strcasecmp(path + n - 4, ".app") == 0;
}

/* Accept host or host:port; anything after the last colon is a port only when
 * it is all digits (so bare IPv6-ish hosts still work). */
static void set_term_host(const char *spec)
{
	const char *colon = strrchr(spec, ':');
	const char *p;
	int port;

	if (colon && colon[1])
	{
		p = colon + 1;
		for (; *p; p++)
			if (*p < '0' || *p > '9')
				break;
		if (*p == 0)
		{
			port = atoi(colon + 1);
			if (port < 1 || port > 65535)
				port = 23;
			s_opts.term_port = port;
			snprintf(s_term_host, sizeof s_term_host, "%.*s",
				 (int)(colon - spec), spec);
			s_term_host[sizeof s_term_host - 1] = 0;
			if (!s_term_host[0])
				s_opts.term_host = 0;
			else
				s_opts.term_host = s_term_host;
			return;
		}
	}
	snprintf(s_term_host, sizeof s_term_host, "%s", spec);
	s_term_host[sizeof s_term_host - 1] = 0;
	s_opts.term_host = s_term_host[0] ? s_term_host : 0;
}

static void help(const char *prog)
{
	printf("Usage: %s [options] [file.app | \"line to run\"]\n\n", prog);
	printf("  --drive DIR       mount the host directory DIR as the D: drive\n");
	printf("  --drive-root DIR  base directory for C: (default ~/.mmbasic)\n");
	printf("                    (--drive=DIR and --drive-root=DIR are also accepted)\n");
	printf("  --term [HOST[:PORT]]  run TERM as a sealed session (exit when it ends)\n");
	printf("  --repl, --stay    return to the REPL when an .app or TERM session ends\n");
	printf("  --help, -h        show this help\n");
	printf("\n"
	       "A positional argument that names an existing .app file runs it as a\n"
	       "self-contained app: the package mounts read-only as B: and MAIN.BAS\n"
	       "runs, then the process exits (use --repl to stay).\n");
	printf("\nEnvironment:\n");
	printf("  MMB_DRIVE_ROOT DIR     base directory for the C: drive (default ~/.mmbasic)\n");
	printf("  MMB_SDL_DUMP FILE      write the framebuffer to FILE as PPM on exit (SDL build)\n");
	printf("  MMB_SDL_SERIAL=1       mirror the serial stream to stdout even on a TTY\n");
	printf("  MMB_CLIPBOARD TEXT     seed the host clipboard at startup (SDL build)\n");
	printf("  SDL_VIDEODRIVER=dummy  run the SDL build without a display\n");
	printf("\n"
	       "Native SDL build only: EDIT/WORDPAD copy to the host OS clipboard, and\n"
	       "Ctrl+Shift+V pastes the host clipboard into the active input.\n");
	printf("\nExamples:\n");
	printf("  %s --drive /media/usb\n", prog);
	printf("  %s --drive /media/usb \"DIR \\\"D:/\\\"\"\n", prog);
	printf("  %s ~/games/SantaCatch.app\n", prog);
	printf("  %s --term bbs.example.net:23\n", prog);
	exit(0);
}

const struct mmb_cli_opts *mmb_cli_parse(int argc, char **argv)
{
	int i;

	memset(&s_opts, 0, sizeof s_opts);
	s_opts.mode = MMB_CLI_REPL;
	s_opts.term_port = 23;

	for (i = 1; i < argc; i++)
	{
		const char *a = argv[i];

		if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
			help(argv[0]);
		if (strcmp(a, "--drive") == 0 && i + 1 < argc)
		{
			s_drive_used = 1;
			if (parse_drive(argv[++i]) != 0)
				fprintf(stderr, "mmbasic: bad --drive spec '%s'\n",
					argv[i]);
		}
		else if (strncmp(a, "--drive=", 8) == 0)
		{
			s_drive_used = 1;
			if (parse_drive(a + 8) != 0)
				fprintf(stderr, "mmbasic: bad --drive spec '%s'\n",
					a + 8);
		}
		else if (strcmp(a, "--drive-root") == 0 && i + 1 < argc)
		{
			setenv("MMB_DRIVE_ROOT", argv[++i], 1);
		}
		else if (strncmp(a, "--drive-root=", 13) == 0)
		{
			setenv("MMB_DRIVE_ROOT", a + 13, 1);
		}
		else if (strcmp(a, "--term") == 0)
		{
			s_opts.mode = MMB_CLI_TERM;
			if (i + 1 < argc && argv[i + 1][0] != '-' &&
			    !is_app_name(argv[i + 1]))
				set_term_host(argv[++i]);
		}
		else if (strncmp(a, "--term=", 7) == 0)
		{
			s_opts.mode = MMB_CLI_TERM;
			if (a[7])
				set_term_host(a + 7);
		}
		else if (strcmp(a, "--repl") == 0 || strcmp(a, "--stay") == 0)
		{
			s_opts.stay = 1;
		}
		else if (a[0] != '-')
		{
			if (is_app_name(a) && is_file(a))
			{
				s_opts.mode = MMB_CLI_APP;
				s_opts.app_path = a;
			}
			else
			{
				s_opts.mode = MMB_CLI_LINE;
				s_opts.line = a;
			}
		}
	}
	return &s_opts;
}

const char *mmb_cli_app_run_line(void)
{
	const char *path = s_opts.app_path;
	const char *slash;
	char dir[512];
	const char *base;
	char drive;

	if (!path)
		return 0;
	slash = strrchr(path, '/');
	if (slash)
	{
		int n = (int)(slash - path);

		if (n == 0)
			n = 1; /* "/game.app" -> dir "/" */
		if (n >= (int)sizeof dir)
			return 0;
		memcpy(dir, path, (size_t)n);
		dir[n] = 0;
		base = slash + 1;
	}
	else
	{
		snprintf(dir, sizeof dir, ".");
		base = path;
	}
	if (!base[0])
		return 0;

	/* --drive already owns D:; bind the app directory to E: in that case. */
	drive = s_drive_used ? 'E' : 'D';
	if (storage_posix_mount(drive, dir) != 0)
		return 0;
	snprintf(s_app_line, sizeof s_app_line, "RUN \"%c:/%s\"", drive, base);
	return s_app_line;
}

const char *mmb_cli_term_run_line(void)
{
	if (!s_opts.term_host || !s_opts.term_host[0])
		snprintf(s_term_line, sizeof s_term_line, "TERM");
	else
		snprintf(s_term_line, sizeof s_term_line, "TERM \"%s\",%d",
			 s_opts.term_host, s_opts.term_port);
	return s_term_line;
}
