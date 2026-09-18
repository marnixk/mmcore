"""Static WCAG contrast audit of the EDIT theme palettes.

Parses ``k_themes`` and the per-theme ``pal_*`` tables out of
``mmbasic/src/editor.c``, resolves the effective palette (VGA when
``pal == 0``, as Turbo does), and asserts every text-bearing role pair meets
WCAG AA (>= 4.5:1) and borders meet >= 3:1. No QEMU required.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EDITOR_C = ROOT / "mmbasic" / "src" / "editor.c"

TUI_INDEX = {
    "TUI_BLACK": 0,
    "TUI_RED": 1,
    "TUI_GREEN": 2,
    "TUI_YELLOW": 3,
    "TUI_BLUE": 4,
    "TUI_MAGENTA": 5,
    "TUI_CYAN": 6,
    "TUI_WHITE": 7,
    "TUI_BRBLACK": 8,
    "TUI_BRRED": 9,
    "TUI_BRGREEN": 10,
    "TUI_BRYELLOW": 11,
    "TUI_BRBLUE": 12,
    "TUI_BRMAGENTA": 13,
    "TUI_BRCYAN": 14,
    "TUI_BRWHITE": 15,
}

VGA_PALETTE = [
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
]

FIELDS = [
    "menu_fg", "menu_bg", "hot", "sel_fg", "sel_bg", "edit_fg", "edit_bg",
    "mark_fg", "mark_bg", "str_fg", "num_fg", "cmt_fg", "brd_fg", "brd_bg",
    "tab_fg", "tab_bg", "tabcur_fg", "tabcur_bg", "dlg_fg", "dlg_bg",
    "sh_fg", "sh_bg", "list_bg", "err_fg", "err_bg", "pal",
]

TEXT_PAIRS = [
    ("menu_fg", "menu_bg"),
    ("hot", "menu_bg"),
    ("sel_fg", "sel_bg"),
    ("edit_fg", "edit_bg"),
    ("mark_fg", "mark_bg"),
    ("str_fg", "edit_bg"),
    ("num_fg", "edit_bg"),
    ("cmt_fg", "edit_bg"),
    ("tab_fg", "tab_bg"),
    ("tabcur_fg", "tabcur_bg"),
    ("dlg_fg", "dlg_bg"),
    ("dlg_fg", "list_bg"),
    ("err_fg", "err_bg"),
]

DECOR_PAIRS = [
    ("brd_fg", "brd_bg"),
]

TEXT_MIN = 4.5
DECOR_MIN = 3.0
# A list surface must stay legible under the single dialog foreground and also
# read as a distinct surface. For dark themes those two goals cap the
# list_bg/dlg_bg separation well below 3:1 (a bright list_bg would break
# dlg_fg/list_bg), so the list surface is only required to be clearly
# distinguishable.
LIST_MIN = 1.2


def _load():
    src = EDITOR_C.read_text()
    pals = {}
    for m in re.finditer(
        r"static const unsigned (pal_\w+)\[16\] = \{([^}]*)\};", src
    ):
        vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)u?", m.group(2))]
        assert len(vals) == 16, (m.group(1), len(vals))
        pals[m.group(1)] = vals

    table = src[src.index("static const mmb_ed_theme k_themes"):]
    table = table[: table.index("\n};")]
    themes = {}
    for m in re.finditer(r'\{\s*"(\w+)",(.*?)\}', table, re.S):
        name = m.group(1)
        toks = re.findall(r"TUI_[A-Z]+|pal_\w+|\b0\b", m.group(2))
        assert len(toks) == len(FIELDS), (name, len(toks), toks)
        themes[name] = dict(zip(FIELDS, toks))
    return pals, themes


def _channel(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def _luminance(rgb):
    r = (rgb >> 16) & 0xFF
    g = (rgb >> 8) & 0xFF
    b = rgb & 0xFF
    return 0.2126 * _channel(r) + 0.7152 * _channel(g) + 0.0722 * _channel(b)


def contrast(a, b):
    la, lb = _luminance(a), _luminance(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


def _pals_and_themes():
    pals, themes = _load()

    def palette(theme):
        p = theme["pal"]
        return VGA_PALETTE if p == "0" else pals[p]

    def rgb(theme, pal, field):
        return pal[TUI_INDEX[theme[field]]]

    return themes, palette, rgb


def test_theme_palettes_parse():
    pals, themes = _load()
    assert len(themes) == 10, sorted(themes)
    assert "Slate" in themes
    for name, theme in themes.items():
        if theme["pal"] != "0":
            assert theme["pal"] in pals, name


def test_all_text_role_pairs_meet_wcag_aa():
    themes, palette, rgb = _pals_and_themes()
    failures = []
    for name, theme in themes.items():
        pal = palette(theme)
        for fg, bg in TEXT_PAIRS:
            ratio = contrast(rgb(theme, pal, fg), rgb(theme, pal, bg))
            if ratio < TEXT_MIN:
                failures.append(f"{name}: {fg}/{bg} = {ratio:.2f}")
    assert not failures, "\n".join(failures)


def test_decorative_pairs_meet_wcag():
    themes, palette, rgb = _pals_and_themes()
    failures = []
    for name, theme in themes.items():
        pal = palette(theme)
        for fg, bg in DECOR_PAIRS:
            ratio = contrast(rgb(theme, pal, fg), rgb(theme, pal, bg))
            if ratio < DECOR_MIN:
                failures.append(f"{name}: {fg}/{bg} = {ratio:.2f}")
    assert not failures, "\n".join(failures)


def test_list_background_is_distinct_from_dialog_surface():
    themes, palette, rgb = _pals_and_themes()
    failures = []
    for name, theme in themes.items():
        pal = palette(theme)
        ratio = contrast(rgb(theme, pal, "list_bg"), rgb(theme, pal, "dlg_bg"))
        if ratio < LIST_MIN:
            failures.append(f"{name}: list_bg/dlg_bg = {ratio:.2f}")
    assert not failures, "\n".join(failures)


def test_solid_shadow_block_differs_from_dialog_surface():
    themes, palette, rgb = _pals_and_themes()
    for name, theme in themes.items():
        pal = palette(theme)
        sh_fg = rgb(theme, pal, "sh_fg")
        sh_bg = rgb(theme, pal, "sh_bg")
        if sh_fg == sh_bg:
            assert sh_bg != rgb(theme, pal, "dlg_bg"), name
        else:
            assert contrast(sh_fg, sh_bg) >= DECOR_MIN, name


def test_turbo_numbers_are_bright_cyan():
    themes, palette, rgb = _pals_and_themes()
    turbo = themes["Turbo"]
    assert turbo["num_fg"] == "TUI_BRCYAN"
    assert rgb(turbo, palette(turbo), "num_fg") == 0x55FFFF


def test_default_and_names_unchanged():
    pals, themes = _load()
    names = list(themes)
    assert names == [
        "Paper", "Cloud", "Snow", "Night", "Nord",
        "Slate", "Forest", "Violet", "Turbo", "Phosphor",
    ]
