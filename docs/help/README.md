# Help topics (`docs/help/`)

Authoritative HELP text for the interactive viewer. The firmware does not
edit these by hand in C: `scripts/gen_help.py` compiles every `*.txt` file
here into `mmbasic/src/help_data.c` at build time. Adding a file is enough;
nothing in the command table or Makefile lists individual topics.

`GAP_ANALYSIS_*.md` files may live in this directory for convenience. They
are Markdown only and are **not** loaded by `gen_help.py` (which reads
`*.txt` exclusively), so they never enter the firmware HELP image. Refresh
them with `scripts/gap_analysis.py` or the `/update-help` skill.

## File format

```
name: CLS
kind: command
alias: COLOR

CLS [colour]

Clear the screen...
See ~PIXEL~ and ~LINE~.
```

- `name` — topic as typed after HELP (required).
- `kind` — `command`, `language`, or `page`.
- `alias` — optional comma-separated extra names (repeat the field for more).
- A blank line ends the header. The rest is the body.

`OVERVIEW` and `CONTENTS` are required pages. Bare HELP opens Overview.
`HELP BASIC` is an alias of Contents. The A-Z Index is generated from
command and language topics (pages are omitted).

## Links

Wrap a topic in tildes to make a link: `~FOR~`. The viewer shows `<FOR>`
and Enter follows it. Unmarked text is never turned into a link, even when
it matches a command name.

`~~` is a literal tilde.

Allowed link targets are topic names, aliases, and `Overview`, `Contents`,
`Index`, `Back`, and `BASIC`. The generator fails the build on unknown
`~names~`.
