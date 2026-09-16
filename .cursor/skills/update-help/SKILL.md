---
name: update-help
description: >-
  Audit and refresh mmCore HELP docs, produce a command gap analysis
  (mmCore vs PicoMite MMBasic vs QuickBasic), add missing HELP topics,
  rewrite entries in QuickBasic style, keep Overview/ME/boot message
  current. Use when the user asks to /update-help, refresh help, fill
  HELP gaps, or regenerate GAP_ANALYSIS.
---

# /update-help

Standing process for this repository. Prefer running in phases when the
user asks to cut work up; otherwise run the full pipeline.

**mmCore** is the operating-system wrapper (console, drives, builtins)
around the MMBasic language implementation in `mmbasic/`.

**Objective:** every command and function implemented in mmCore has a
plain, user-facing HELP topic; Overview lists them by category;
`HELP ME` introduces the system; and
`docs/help/GAP_ANALYSIS_<YYYY-MM-DD>.md` compares mmCore, MMBasic
(PicoMite), and QuickBasic. Gap analysis informs future GitHub issues —
do **not** open tickets unless asked.

## Phases

1. **Analysis + skill** — regenerate gap analysis; keep this skill
   current. Stop here if the user wants approval before rewrites.
2. **HELP completion** — rewrite/add topics, Overview boxes, `me.txt`,
   boot banner, regenerate `help_data.c`, build and test.

## Sources of truth

| Inventory | Where |
|-----------|--------|
| mmCore commands | `mmbasic/src/core.c` → `try_tok_cmd()` |
| mmCore functions | `mmbasic/src/expr.c` → `mmb_try_function()` |
| mmCore MATH/STRUCT extras | `cmd_math.c`, `cmd_struct.c` (subcommand rows OK in gap tables) |
| HELP topics | `docs/help/*.txt` only → `scripts/gen_help.py` |
| PicoMite MMBasic | `picomite-fork/AllCommands.h` (reference only — never compile) |
| QuickBasic | https://gamma.zem.fi/~fis/qb.html (complete overview) |

`docs/help/GAP_ANALYSIS_*.md` lives next to the topics for convenience
but must **never** be compiled into the firmware image. `gen_help.py`
loads `*.txt` only; keep analysis as `.md`.

Implementation / delta notes for agents belong outside HELP bodies
(this skill, gap analysis, or a private notes file) — never in user-
visible topic text.

## Pipeline

### 1. Sync and branch

```bash
git fetch origin master
git checkout master && git pull origin master
git checkout -b task/update-help-<short>-d51a
```

### 2. Regenerate gap analysis

```bash
.venv/bin/python scripts/gap_analysis.py
```

Writes `docs/help/GAP_ANALYSIS_<YYYY-MM-DD>.md` with categories ordered
by common usage (graphics later). Per category, a table:

| COMMANDNAME | 5-word description | mmCore | MMBasic | QuickBasic |

Mark presence with `x`. Include functions. Subcommands may be separate
rows. Canonicalise names to UPPERCASE (strip trailing `(` from Pico
tokens).

Optional:

```bash
.venv/bin/python scripts/gap_analysis.py --audit-help
```

Lists mmCore commands/functions missing a HELP topic (aliases count;
multi-subcommand families need one parent topic).

### 3. HELP rewrite / add (phase 2)

Body shape (QuickBasic-inspired). Show keyword shapes with examples
(FOR counting up/down; DO LOOP UNTIL / DO WHILE; etc.):

```
What the command does (functions: what it returns).

COMMANDNAME param, [param2], [param3]

  param - description
  param2 - description (optional)
  param3 - description (optional)

Example:

    .. short example ..
    .. optional-param variants ..

    .. longer example ..
```

Rules:

- Audience: mmCore user on a Pi — strip history / agent / “not
  implemented” chatter from HELP bodies.
- Multi-subcommand families (`ON`, `SPRITE`, `MATH`, `OPTION`, `PLAY`,
  `STRUCT`, …): **one** topic covering all forms.
- Links: `~TOPIC~` only for real topics/aliases.
- Soft-wrap body prose at **80 columns** (do not wrap early at ~60–72).
- After edits: `scripts/gen_help.py` (also via `scripts/build.sh`).

### 4. Orientation pages and Overview (phase 2)

Overview top: ASCII boxes for **Orientation** and **Quick Reference**:

- Orientation → `USING HELP`, `SYNTAX CONVENTIONS`
- Quick Reference → `ASCII CHARACTER CODES`, `KEYBOARD SCAN CODES`

Lift ASCII / scan-code tables from the QuickBasic help page so they
match closely. Then list every command/function under categories:

```
String Operations

    ~LPAD$~ - short description
```

### 5. HELP ME and boot banner (phase 2)

- `docs/help/me.txt` — `name: ME`, `kind: page`: intro to mmCore,
  drives (`A:` ramdisk, `C:` SD, `D:`+), builtins (`FILES`, `WORDPAD`,
  `EDIT`, `TERM`), Wi-Fi / Ethernet.
- `mmb_print_startup()` in `mmbasic/src/util.c` — mention `HELP ME`
  (e.g. `Type HELP ME for a short introduction.`).

### 6. Build, test, commit

```bash
scripts/build.sh
.venv/bin/python -m pytest tests/test_help_docs.py tests/test_help.py -q
```

Update host tests that assert Overview/Contents prose when those pages
change.

## Category order (default)

1. Control Flow  
2. Variables and Types  
3. Procedures  
4. Console and Text I/O  
5. String Operations  
6. Math Functions  
7. File System  
8. File I/O  
9. Time and Events  
10. System and Options  
11. Network and Terminal  
12. Sound and Play  
13. Graphics  
14. Sprites and Blit  
15. JSON and Structures  
16. Environment (MM.*)  
17. Hardware and Devices (mostly PicoMite)  
18. QuickBasic Legacy (QB-only)

## Invocation

`/update-help` or “refresh help / gap analysis” → follow this skill.
If the user says analysis-first, complete phase 1 and wait.
