/*
 * Native (Linux/macOS) entry point. LN-01: a headless stdin/stdout REPL over
 * the portable interpreter. The SDL2 window/event loop arrives in LN-09.
 *
 * Usage:
 *   mmbasic                 interactive REPL on stdin/stdout
 *   mmbasic "RUN \"...\""   execute one line and exit
 *   mmbasic path/to/x.app   run a packaged .APP (app VM), then exit
 *   mmbasic --term HOST:23  run a sealed TERM session, then exit
 */
#include "win_compat.h"

#include "mmb_priv.h"
#include "cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

void mmb_platform_bind_stdio(void);

static void run_line(const char *line)
{
	const char *result = mmb_exec_line(line);

	if (result && *result)
		fputs(result, stdout);
	fflush(stdout);
}

/* Sealed TERM session for the headless build: forward stdin bytes to TERM and
 * poll the network until the session ends, then return so main() can exit. */
static void run_term_sealed(void)
{
	run_line(mmb_cli_term_run_line());
	while (mmb_in_term())
	{
		int c;

#ifdef _WIN32
		if (mmb_stdin_ready())
#else
		fd_set rfds;
		struct timeval tv;

		FD_ZERO(&rfds);
		FD_SET(0, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 50000;
		if (select(1, &rfds, 0, 0, &tv) > 0)
#endif
		{
			c = fgetc(stdin);
			if (c == EOF)
				break;
			else
			{
				const char *out = mmb_term_key((char)c);

				if (out && out[0])
				{
					fputs(out, stdout);
					fflush(stdout);
				}
			}
		}
		mmb_poll();
	}
}

int main(int argc, char **argv)
{
	char line[MMB_LINE_LEN];
	const struct mmb_cli_opts *cli;

	cli = mmb_cli_parse(argc, argv);
	mmb_platform_bind_stdio();
	mmb_print_startup();

	if ((cli->mode == MMB_CLI_APP || cli->mode == MMB_CLI_TERM) && !cli->stay)
		signal(SIGINT, SIG_IGN);

	if (cli->mode == MMB_CLI_APP)
	{
		const char *cmd = mmb_cli_app_run_line();

		if (!cmd)
		{
			fprintf(stderr, "mmbasic: cannot run '%s'\n",
				cli->app_path ? cli->app_path : "?");
			return 2;
		}
		run_line(cmd);
		if (!cli->stay)
			return 0;
	}
	else if (cli->mode == MMB_CLI_TERM)
	{
		run_term_sealed();
		if (!cli->stay)
			return 0;
	}
	else if (cli->mode == MMB_CLI_LINE && cli->line)
	{
		run_line(cli->line);
		return 0;
	}

	fputs("> ", stdout);
	fflush(stdout);
	while (fgets(line, (int)sizeof line, stdin))
	{
		size_t n = strlen(line);

		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		mmb_poll();
		run_line(line);
		if (mmb_take_quit())
			return 0;
		fputc('\n', stdout);
		fputs("> ", stdout);
		fflush(stdout);
	}
	/* EOF at the empty prompt is Ctrl+D: run QUIT for a clean shutdown,
	 * matching the SDL window path (#646). */
	run_line("QUIT");
	return 0;
}
