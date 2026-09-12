#!/usr/bin/env python3
"""Compile docs/help/*.txt into mmbasic/src/help_data.c.

Each topic is one file. New files are picked up automatically; this script
does not need a topic list in C or Make.

File format:

    name: CLS
    kind: command
    alias: COLOR

    Body text. Wrap a topic in tildes to make a link: ~FOR~.
    ~~ is a literal tilde.

kind is command, language, or page. Pages (OVERVIEW, CONTENTS) are
authorable landing pages and are omitted from the A-Z Index.
"""
from __future__ import annotations

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HELP_DIR = os.path.join(REPO, "docs", "help")
DEFAULT_OUT = os.path.join(REPO, "mmbasic", "src", "help_data.c")

KIND_MAP = {"command": 1, "language": 2, "page": 3}
KIND_NAME = {1: "HELP_CMD", 2: "HELP_LANG", 3: "HELP_PAGE"}
SPECIAL_LINKS = {"OVERVIEW", "CONTENTS", "INDEX", "BACK", "BASIC"}
LINK_RE = re.compile(r"~([^~]+)~")
FRONT_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*):\s*(.*)$")


def is_id_char(ch: str) -> bool:
    return ch.isalnum() or ch in "$_."


def c_ident(name: str) -> str:
    out = []
    for ch in name.upper():
        if ch.isalnum():
            out.append(ch)
        else:
            out.append("_")
    ident = "".join(out).strip("_")
    if not ident or ident[0].isdigit():
        ident = "T_" + ident
    return ident


def c_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
    )


def parse_file(path: str) -> dict:
    raw = open(path, encoding="utf-8").read()
    if raw.startswith("\ufeff"):
        raw = raw[1:]
    lines = raw.splitlines()
    meta: dict = {"name": "", "kind": "command", "alias": [], "path": path}
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            break
        m = FRONT_RE.match(line)
        if not m:
            break
        key = m.group(1).lower()
        val = m.group(2).strip()
        if key == "alias":
            if val:
                for part in val.split(","):
                    part = part.strip()
                    if part:
                        meta["alias"].append(part)
        elif key in ("name", "kind"):
            meta[key] = val
        else:
            raise ValueError(f"{path}: unknown field {key!r}")
        i += 1
    body = "\n".join(lines[i:])
    if body.startswith("\n"):
        body = body[1:]
    if not body.endswith("\n"):
        body += "\n"
    if not meta["name"]:
        raise ValueError(f"{path}: missing name:")
    kind = meta["kind"].lower()
    if kind not in KIND_MAP:
        raise ValueError(f"{path}: kind must be command, language, or page")
    meta["kind"] = kind
    meta["text"] = body
    return meta


def load_topics(help_dir: str = HELP_DIR) -> list[dict]:
    topics = []
    for name in sorted(os.listdir(help_dir)):
        if name.startswith(".") or name == "README.md":
            continue
        if not name.endswith(".txt"):
            continue
        topics.append(parse_file(os.path.join(help_dir, name)))
    if not topics:
        raise SystemExit(f"no help topics in {help_dir}")
    seen = {}
    for t in topics:
        key = t["name"].upper()
        if key in seen:
            raise SystemExit(f"duplicate help name {t['name']}: {t['path']} and {seen[key]}")
        seen[key] = t["path"]
    names = {t["name"].upper() for t in topics}
    if "OVERVIEW" not in names or "CONTENTS" not in names:
        raise SystemExit("docs/help must include OVERVIEW and CONTENTS pages")
    return topics


def collect_aliases(topics: list[dict]) -> list[tuple[str, str]]:
    aliases = []
    seen = set()
    topic_names = {t["name"].upper() for t in topics}
    for t in topics:
        for a in t["alias"]:
            key = (a.upper(), t["name"].upper())
            if key in seen:
                continue
            if a.upper() == t["name"].upper() and a.upper() in topic_names:
                continue
            seen.add(key)
            aliases.append((a, t["name"]))
        if t["name"].upper() == "CONTENTS":
            key = ("BASIC", "CONTENTS")
            if key not in seen:
                seen.add(key)
                aliases.append(("BASIC", "CONTENTS"))
    return aliases


def known_link_names(topics: list[dict], aliases: list[tuple[str, str]]) -> set[str]:
    names = {t["name"].upper() for t in topics}
    names.update(a.upper() for a, _ in aliases)
    names.update(SPECIAL_LINKS)
    return names


def extract_links(text: str) -> list[str]:
    out = []
    i = 0
    while i < len(text):
        if text[i] == "~" and i + 1 < len(text) and text[i + 1] == "~":
            i += 2
            continue
        if text[i] == "~":
            j = text.find("~", i + 1)
            if j < 0:
                break
            out.append(text[i + 1 : j])
            i = j + 1
            continue
        i += 1
    return out


def validate_links(topics: list[dict], aliases: list[tuple[str, str]]) -> None:
    known = known_link_names(topics, aliases)
    errors = []
    for t in topics:
        for label in extract_links(t["text"]):
            if not label.strip():
                errors.append(f"{t['path']}: empty ~link~")
                continue
            if label.upper() not in known:
                errors.append(f"{t['path']}: unknown link ~{label}~")
    if errors:
        raise SystemExit("\n".join(errors))


def wrap_names(text: str, names: list[str]) -> str:
    """Wrap standalone name matches as ~NAME~. Longest match first."""
    ordered = sorted({n for n in names if n and n != "?"}, key=len, reverse=True)
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] == "~":
            if i + 1 < n and text[i + 1] == "~":
                out.append("~~")
                i += 2
                continue
            j = text.find("~", i + 1)
            if j < 0:
                out.append(text[i:])
                break
            out.append(text[i : j + 1])
            i = j + 1
            continue
        matched = None
        if i == 0 or not is_id_char(text[i - 1]):
            for name in ordered:
                end = i + len(name)
                if end <= n and text[i:end] == name:
                    if end < n and is_id_char(text[end]):
                        continue
                    matched = name
                    break
        if matched:
            out.append("~" + matched + "~")
            i += len(matched)
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def generate_c(topics: list[dict], aliases: list[tuple[str, str]]) -> str:
    lines = [
        "/* Generated by scripts/gen_help.py from docs/help/. Do not edit. */",
        "",
        '#include "mmb_priv.h"',
        "",
        "#define HELP_CMD  1",
        "#define HELP_LANG 2",
        "#define HELP_PAGE 3",
        "",
        "typedef struct {",
        "	const char *name;",
        "	int kind;",
        "	const char *text;",
        "} help_topic;",
        "",
    ]
    used = {}
    for t in topics:
        ident = c_ident(t["name"])
        base = ident
        n = 2
        while ident in used:
            ident = f"{base}_{n}"
            n += 1
        used[ident] = t
        t["_ident"] = ident
        esc = c_escape(t["text"])
        lines.append(f"static const char kText_{ident}[] =")
        chunks = []
        buf = ""
        for part in esc.split("\\n"):
            buf += part + "\\n"
            if len(buf) > 80:
                chunks.append(buf)
                buf = ""
        if buf:
            chunks.append(buf)
        if not chunks:
            chunks = [esc]
        for i, chunk in enumerate(chunks):
            comma = "" if i + 1 < len(chunks) else ";"
            lines.append(f'\t"{chunk}"{comma}')
        lines.append("")

    lines.append("static const help_topic kTopics[] = {")
    for t in topics:
        lines.append(
            f'\t{{ "{t["name"]}", {KIND_NAME[KIND_MAP[t["kind"]]]}, kText_{t["_ident"]} }},'
        )
    lines.append("};")
    lines.append("")
    lines.append("static const struct {")
    lines.append("	const char *alias;")
    lines.append("	const char *canon;")
    lines.append("} kAlias[] = {")
    if aliases:
        for alias, canon in aliases:
            lines.append(f'\t{{ "{alias}", "{canon}" }},')
    else:
        lines.append('\t{ "", "" },')
    lines.append("};")
    lines.append("")
    lines.append(
        """
static const char *resolve_alias(const char *q)
{
	int i, n = (int)(sizeof(kAlias) / sizeof(kAlias[0]));
	for (i = 0; i < n; i++)
		if (kAlias[i].alias[0] && mmb_keyword_eq(kAlias[i].alias, q))
			return kAlias[i].canon;
	return q;
}

static const help_topic *find_by_name(const char *q)
{
	int i, n = (int)(sizeof(kTopics) / sizeof(kTopics[0]));
	for (i = 0; i < n; i++)
		if (mmb_keyword_eq(kTopics[i].name, q))
			return &kTopics[i];
	return 0;
}

static void first_word(const char *q, char *dst, int dstsz)
{
	int n = 0;
	while (*q && *q != ' ' && n < dstsz - 1)
		dst[n++] = *q++;
	dst[n] = 0;
}

static const help_topic *find_topic(const char *q)
{
	const help_topic *t;
	char word[40];
	const char *canon;

	canon = resolve_alias(q);
	t = find_by_name(canon);
	if (t)
		return t;
	first_word(q, word, sizeof(word));
	if (word[0] && !mmb_keyword_eq(word, q))
	{
		canon = resolve_alias(word);
		t = find_by_name(canon);
		if (t)
			return t;
	}
	return 0;
}

int mmb_help_topic_count(void)
{
	return (int)(sizeof(kTopics) / sizeof(kTopics[0]));
}

const char *mmb_help_topic_name(int i)
{
	int n = mmb_help_topic_count();
	if (i < 0 || i >= n)
		return "";
	return kTopics[i].name;
}

const char *mmb_help_topic_text(int i)
{
	int n = mmb_help_topic_count();
	if (i < 0 || i >= n)
		return "";
	return kTopics[i].text;
}

int mmb_help_topic_kind(int i)
{
	int n = mmb_help_topic_count();
	if (i < 0 || i >= n)
		return 0;
	return kTopics[i].kind;
}

int mmb_help_lookup(const char *name)
{
	const help_topic *t;
	if (!name || !name[0])
		return -1;
	t = find_topic(name);
	if (!t)
		return -1;
	return (int)(t - kTopics);
}

int mmb_help_alias_count(void)
{
	int n = (int)(sizeof(kAlias) / sizeof(kAlias[0]));
	if (n == 1 && !kAlias[0].alias[0])
		return 0;
	return n;
}

const char *mmb_help_alias_name(int i)
{
	int n = mmb_help_alias_count();
	if (i < 0 || i >= n)
		return "";
	return kAlias[i].alias;
}

const char *mmb_help_alias_canon(int i)
{
	int n = mmb_help_alias_count();
	if (i < 0 || i >= n)
		return "";
	return kAlias[i].canon;
}
""".rstrip()
    )
    lines.append("")
    return "\n".join(lines) + "\n"


def write_if_changed(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.isfile(path) and open(path, encoding="utf-8").read() == text:
        return
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def main(argv: list[str] | None = None) -> int:
    out = DEFAULT_OUT
    args = argv if argv is not None else sys.argv[1:]
    if args:
        out = os.path.abspath(args[0])
    topics = load_topics()
    aliases = collect_aliases(topics)
    validate_links(topics, aliases)
    write_if_changed(out, generate_c(topics, aliases))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
