#ifndef MMB_CLI_H
#define MMB_CLI_H

/* Startup modes for the native binaries. */
enum mmb_cli_mode {
	MMB_CLI_REPL = 0, /* interactive REPL */
	MMB_CLI_LINE,     /* run one BASIC line, then exit */
	MMB_CLI_APP,      /* run a packaged .APP, then exit unless --repl */
	MMB_CLI_TERM      /* sealed TERM session; exit when it ends */
};

struct mmb_cli_opts {
	enum mmb_cli_mode mode;
	const char *line;      /* MMB_CLI_LINE: the line to run */
	const char *app_path;  /* MMB_CLI_APP: host path to the .APP */
	const char *term_host; /* MMB_CLI_TERM: host, or NULL for the menu */
	int term_port;         /* MMB_CLI_TERM: TCP port (default 23) */
	int stay;              /* --repl/--stay: fall back to the REPL after */
	int fullscreen;        /* --fullscreen: SDL window opens fullscreen */
};

/* Parse startup options (--drive, --drive-root, --term, --repl, and a
 * positional .APP path or BASIC line). Must be called before the platform is
 * bound (which initialises storage), because --drive mounts at parse time.
 * Returns a pointer to a static options struct; exits for --help. */
const struct mmb_cli_opts *mmb_cli_parse(int argc, char **argv);

/* For MMB_CLI_APP: bind the package's host directory to a spare drive and
 * return a static `RUN "<drive>:/name.app"` line, or NULL on failure. Call
 * after mmb_cli_parse and before the platform is bound. */
const char *mmb_cli_app_run_line(void);

/* For MMB_CLI_TERM: return a static `TERM ...` line for the parsed host/port
 * (plain `TERM` when no host was given). */
const char *mmb_cli_term_run_line(void);

#endif
