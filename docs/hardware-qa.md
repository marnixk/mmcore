# Hardware QA checklist (v0.197.0 – v0.199.0)

Manual tests to run on real Raspberry Pi hardware. Built from the three
parallel issue waves; each item links the issue(s) it came from. Desktop-only
items are marked and can be skipped on the Pi.

Releases: **v0.197.0** (wave 1) → **v0.198.0** (wave 2) → **v0.199.0** (wave 3).
Each ships the four board zips plus a Windows zip and a Linux AppImage. macOS
is intentionally omitted (notarization profile unavailable in the build
environment).

## Boot & sessions
- [ ] Boot screen shows the centred mmcore logo, `mmcore operating system - 2026 (c) Marnix Kok`, then the MMBasic/copyright lines. (#536)
- [ ] Virtual consoles: `Ctrl+Alt+1`…`4` (top-row or keypad) give independent interpreter, screen, input line and history; switching back preserves the previous screen. `Ctrl+Alt+F1`…`F4` no longer switches. (#510, #603)

## Clock / network
- [ ] `OPTION NTP ON`, `OPTION NTP SERVER "host[:port]"`, `NTP` one-shot, `OPTION TIMEZONE "Europe/Amsterdam"` (also `UTC+2`, `UTC-5:30`). Confirm `DATE$`/`TIME$`/`DATETIME$` shift and FAT timestamps use local time. (#524)

## Storage / USB
- [ ] Insert/remove a USB stick: hotplug notice and volume label; `EJECT "D:"` (or FILES → Command → Eject) flushes and unmounts; `DRIVE` drops it; re-insert works. (#518, #523)
- [ ] Screenshot: `F12` from any app (including TERM/EDIT) and `SCREENSHOT [path$]`; timestamped PNG on A:/USB; `?SCREENSHOT` on an unwritable/full drive. (#517, #522)

## TERM
- [ ] Scrollback + search: `OPTION TERM SCROLLBACK n`; `PgUp`/`PgDn`, `/` to search, `q` back to live. (#528)
- [ ] Session replay/diary: save a logged session and replay it. (#529)

## Writing
- [ ] WORDPAD lists: `Tab`/`Shift+Tab` nest/outdent, `Enter` auto-increments numbered lists, bullets work; `Ctrl+P` quick-open lists cwd `.MD`. (#514, #539, #556)
- [ ] Autosave / crash resume: mid-edit, kill power; relaunch EDIT or WORDPAD and take the recover prompt; Discard must not touch the real file. (#516, #521)
- [ ] EDITOR quick-open `Ctrl+P` finds cwd `.BAS`/`.INC` past the cap. (#571)
- [ ] `Ctrl+Z` undo is consistent in EDITOR, WORDPAD, PAINT and SPRITE. (#532)

## Creative apps
- [ ] PAINT: `PAINT [file$]`; keyboard tools and USB mouse (click/drag paint, right-click pick colour); save/reload PNG. (#512, #513)
- [ ] SPRITE editor: grid-constrained sprite/font editing. (#527)
- [ ] JUKE: `JUKE [path$]`, folder queue, visualiser, keeps playing off-screen; audio output correct. (#519)
- [ ] TDF.BAS: `#INCLUDE "A:/lib/TDF.BAS"`, then `TDF.Load` / `TDF.Print` / `TDF.Close`; fonts load from `A:/fonts/tdf/`. (#557)

## Apps / prompt
- [ ] App PATH: `OPTION PATH` (default `A:/APPS/`), type an app name at the prompt to run it, `APPS` launcher, `Ctrl+Space` picker, boot-to-app setting. (#515, #520)
- [ ] PACKAGE wizard: guided folder → `.APP`. (#526)

## UI consistency
- [ ] Theme picker: `SETTINGS` UI, `SETTINGS THEME name|n`, `OPTION THEME name`, `THEME("name")` function; applies to EDIT/FILES/WORDPAD/TERM/HELP/SPRITE/PACKAGE/APPS and persists in `C:/.mmbasic.ini`. (#509)
- [ ] Status-bar hotkey hints render a single `>`, e.g. `<Up/Down> Move`. (#569)

## Desktop-only (skip on Pi)
- [ ] Host clipboard bridge: copy out and `Ctrl+Shift+V` paste in the AppImage. (#525)
- [ ] SDL binaries are named `mmcore` / `mmcore.exe`. Windows Authenticode is wired in CI but stays unsigned until a signing certificate secret is configured, so SmartScreen may still warn. (#552, #553)

## Known caveats
- macOS bundle is not published (notarization profile unavailable in the build environment); same as v0.196.0/v0.197.0.
- One graphics test (`test_page1_alpha_composite_png`) is flaky only under parallel load; it passes in isolation.
