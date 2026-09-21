#ifndef MMB_CLI_H
#define MMB_CLI_H

/* Parse startup options (--drive, --drive-root). Must be called before the
 * platform is bound (which initialises storage). Returns the first positional
 * argument (a line to run) or NULL; exits for --help. */
const char *mmb_cli_parse(int argc, char **argv);

#endif
