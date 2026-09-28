# Hardware QA checklist (v0.197.0 – v0.199.0)

Manual tests to run on real Raspberry Pi hardware. Built from the three
parallel issue waves; each item links the issue(s) it came from. Desktop-only
items are marked and can be skipped on the Pi.

Releases: **v0.197.0** (wave 1) → **v0.198.0** (wave 2) → **v0.199.0** (wave 3).
Each ships the four board zips plus a Windows zip and a Linux AppImage. Those
three versions omitted macOS (the notarization profile was unavailable in the
build environment at the time); from v0.214.0 releases also ship a notarized
universal macOS app.

## Boot & sessions
- [ ] Boot screen shows the centred mmcore logo, `mmcore operating system - <version> - 2026 (c) Marnix Kok`, a blank line, then `MMBasic` with the `Copyright 2011-2026 Geoff Graham` and `Copyright 2016-2026 Peter Mather` notices, a blank line, and the `Type HELP ME for a short introduction.` prompt. (#536, #730)
- [ ] Virtual consoles: `Ctrl+Alt+1`…`4` (top-row or keypad) give independent interpreter, screen, input line and history; switching back preserves the previous screen. On the bare-metal Pi `Ctrl+Alt+F1`…`F12` do not switch (there is no Linux VT); on the framebuffer ISO they switch the Linux VT (tty1 = mmcore, tty2 = shell). (#510, #603, #903)

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
- [ ] TDF: native `TDF LOAD` / `TDF PRINT` / `TDF CLOSE` (plus `TDF USE` slots and the `TDF.*` accessors); fonts load from `A:/fonts/tdf/` (`mono/`/`color/`/`deco/`), multi-variation files expose `TDF.VARIANTS%` / the LOAD variant argument. (#557, #866, #867)

## Apps / prompt
- [ ] App PATH: `OPTION PATH` (default `A:/APPS/`), type an app name at the prompt to run it, `APPS` launcher, `Ctrl+Space` picker, boot-to-app setting. (#515, #520)
- [ ] PACKAGE wizard: guided folder → `.APP`. (#526)

## UI consistency
- [ ] Theme picker: `SETTINGS` UI, `SETTINGS THEME name|n`, `OPTION THEME name`, `THEME("name")` function; sets the system theme for FILES/WORDPAD/TERM/HELP/SPRITE/PACKAGE/APPS and persists in `C:/.mmbasic.ini`. `OPTION EDIT THEME name|n|SYSTEM` overrides EDIT only (EDIT Theme menu has a System entry); editor follows the system theme by default. (#509)
- [ ] Status-bar hotkey hints render a single `>`, e.g. `<Up/Down> Move`. (#569)

## Desktop-only (skip on Pi)
- [ ] Host clipboard bridge: copy out and `Ctrl+Shift+V` paste in the AppImage. (#525)
- [ ] SDL binaries are named `mmcore` / `mmcore.exe`. Windows Authenticode is wired in CI but stays unsigned until a signing certificate secret is configured, so SmartScreen may still warn. (#552, #553)

## Linux framebuffer (KMS/DRM) — `mmcore-fb`
- [ ] From a text VT (not a desktop session) run `./native/mmcore-fb` (or `SDL_VIDEODRIVER=kmsdrm ./native/mmcore-fb`): the interpreter fills the display from the first frame with no X11/Wayland. (#828, #829)
- [ ] There is no system cursor: PAINT's tool cursor and the `MOUSE ON` software cursor are the pointer; motion and clicks track. (#829)
- [ ] With no usable video driver the build exits non-zero with a clear stderr message rather than a blank frame. (#829)

## Live USB / ISO (x86_64)
- [ ] Boot `mmcore-fb-x86_64.iso` (USB written with `install-usb.sh`, or QEMU): the display reaches the mmcore prompt fullscreen; `Ctrl+Alt+F2` gives a shell on tty2 and `Ctrl+Alt+F1` returns to mmcore with the screen intact; serial ttyS0 (115200) gives a root shell. Keys typed at the mmcore prompt do not leak into tty1, and `Ctrl+C` still means BREAK, not SIGINT. (#833, #834, #903)
- [ ] `sudo ./install-usb.sh --iso mmcore-fb-x86_64.iso /dev/sdX`, boot, create a `.BAS` file on `C:`, reboot: the file is still there. (#835)
- [ ] Ethernet: plug in and confirm `IPCONFIG` shows an address. Wi-Fi: `OPTION WIFI COUNTRY`, `OPTION WIFI "ssid","psk"`, then `IPCONFIG`. (#836)

## Known caveats
- macOS was omitted from v0.196.0–v0.199.0 (the notarization profile was
  unavailable in the build environment); releases now ship a notarized universal
  `mmcore.app`.
- One graphics test (`test_page1_alpha_composite_png`) is flaky only under parallel load; it passes in isolation.
