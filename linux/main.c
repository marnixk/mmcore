/*
 * Native (Linux/macOS) entry point. LN-01: a headless stdin/stdout REPL over
 * the portable interpreter. The SDL2 window/event loop arrives in LN-09.
 *
 * Usage:
 *   mmbasic                 interactive REPL on stdin/stdout
 *   mmbasic "RUN \"...\""   execute one line and exit
 */
#include "mmb_priv.h"
#include "cli.h"

#include <stdio.h>
#include <string.h>

void mmb_platform_bind_stdio(void);

static void run_line(const char *line)
{
	const char *result = mmb_exec_line(line);

	if (result && *result)
		fputs(result, stdout);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	char line[MMB_LINE_LEN];
	const char *one_line;

	one_line = mmb_cli_parse(argc, argv);
	mmb_platform_bind_stdio();
	mmb_print_startup();

	if (one_line)
	{
		run_line(one_line);
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
		fputc('\n', stdout);
		fputs("> ", stdout);
		fflush(stdout);
	}
	return 0;
}
