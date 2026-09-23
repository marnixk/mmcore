"""Static contrast audit of the SETTINGS dialog role map (#611).

The dialog counterpart of ``tests/test_editor_theme_contrast.py``: parses the
``ST_*`` role macros out of ``mmbasic/src/cmd_settings.c``, resolves each
against every shipped theme's palette and asserts the foreground/background
pairs the dialog draws with meet WCAG AA. It also pins the role map itself, so
a regression back to editor-syntax colours (``edit_fg`` on ``dlg_bg`` or
``hot`` on ``edit_bg`` — the Turbo wash-out) fails here instead of only in the
framebuffer.

Every shipped theme is covered, so this is the machine-readable version of the
"walk every theme with SETTINGS open" checklist. The QEMU matrix in
``tests/test_settings.py`` checks the rendered pixels for the same themes.
"""

import re

from test_editor_theme_contrast import ROOT, _pals_and_themes, contrast

SETTINGS_C = ROOT / "mmbasic" / "src" / "cmd_settings.c"

# The dialog surface roles and the theme fields they must resolve to.
EXPECTED_MAP = {
    "ST_TITLE_FG": "menu_fg",
    "ST_TITLE_BG": "menu_bg",
    "ST_FG": "dlg_fg",
    "ST_BG": "dlg_bg",
    "ST_SEL_FG": "sel_fg",
    "ST_SEL_BG": "sel_bg",
    "ST_HOT": "hot",
    "ST_BRD_FG": "brd_fg",
    "ST_BRD_BG": "brd_bg",
    "ST_ERR_FG": "err_fg",
    "ST_ERR_BG": "err_bg",
}

# Editor-syntax roles that may only appear in the live PREVIEW swatch.
EDITOR_ONLY = ("ST_EDIT_FG", "ST_EDIT_BG", "ST_STR", "ST_NUM", "ST_CMT")

# (fg, bg) pairs the dialog composes across the hub, sections and hints.
TEXT_PAIRS = [
    ("ST_TITLE_FG", "ST_TITLE_BG"),
    ("ST_FG", "ST_BG"),
    ("ST_SEL_FG", "ST_SEL_BG"),
    ("ST_HOT", "ST_BG"),
    ("ST_ERR_FG", "ST_ERR_BG"),
]
DECOR_PAIRS = [
    ("ST_BRD_FG", "ST_BRD_BG"),
]
TEXT_MIN = 4.5
DECOR_MIN = 3.0
# A focused row must read as a distinct surface from the dialog body; dark
# themes rely on the selection hue more than on luminance, so only require a
# clear colour separation here.
SELECT_MIN = 1.2


def _role_map():
    src = SETTINGS_C.read_text()
    return dict(
        re.findall(r"#define\s+(ST_\w+)\s+\(\(int\)stth\(\)->(\w+)\)", src)
    )


def test_settings_role_map_uses_dialog_roles():
    roles = _role_map()
    for macro, field in EXPECTED_MAP.items():
        assert roles.get(macro) == field, (macro, roles.get(macro))


def test_settings_editor_roles_are_preview_only():
    src = SETTINGS_C.read_text()
    roles = _role_map()
    for macro in EDITOR_ONLY:
        assert roles.get(macro), macro
    # No dialog-facing macro may point at an editor-syntax field.
    for macro, field in roles.items():
        if macro in EDITOR_ONLY:
            continue
        assert not field.startswith(("edit_", "str_", "num_", "cmt_")), (
            macro,
            field,
        )
    # The live preview must actually use the editor roles it defines.
    assert "ST_EDIT_FG" in src and "ST_EDIT_BG" in src


def test_settings_text_pairs_meet_wcag_aa():
    themes, palette, rgb = _pals_and_themes()
    roles = _role_map()
    failures = []
    for name, theme in themes.items():
        pal = palette(theme)
        for fg, bg in TEXT_PAIRS:
            ratio = contrast(
                rgb(theme, pal, roles[fg]), rgb(theme, pal, roles[bg])
            )
            if ratio < TEXT_MIN:
                failures.append(f"{name}: {fg}/{bg} = {ratio:.2f}")
    assert not failures, "\n".join(failures)


def test_settings_border_meets_wcag():
    themes, palette, rgb = _pals_and_themes()
    roles = _role_map()
    failures = []
    for name, theme in themes.items():
        pal = palette(theme)
        for fg, bg in DECOR_PAIRS:
            ratio = contrast(
                rgb(theme, pal, roles[fg]), rgb(theme, pal, roles[bg])
            )
            if ratio < DECOR_MIN:
                failures.append(f"{name}: {fg}/{bg} = {ratio:.2f}")
    assert not failures, "\n".join(failures)


def test_settings_selection_surface_is_distinct():
    themes, palette, rgb = _pals_and_themes()
    roles = _role_map()
    failures = []
    for name, theme in themes.items():
        pal = palette(theme)
        sel_bg = rgb(theme, pal, roles["ST_SEL_BG"])
        body_bg = rgb(theme, pal, roles["ST_BG"])
        if sel_bg == body_bg:
            failures.append(f"{name}: sel_bg == dlg_bg")
            continue
        ratio = contrast(sel_bg, body_bg)
        if ratio < SELECT_MIN:
            failures.append(f"{name}: sel_bg/dlg_bg = {ratio:.2f}")
    assert not failures, "\n".join(failures)
