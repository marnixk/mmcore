# Linux native (SDL2) backend

A native build of the same MMBasic interpreter that runs on the Raspberry Pi,
using SDL2 for the window, keyboard, and audio. It is a second backend behind
the existing `mmb_platform` contract, not a fork: language, graphics, commands,
and tests live in `mmbasic/`.

## Build

Requirements: a C toolchain, `make`, `pkg-config`, and SDL2 development files
(`libsdl2-dev` on Debian/Ubuntu, `sdl2` from Homebrew on macOS), plus Python 3
for the generated ramdisk/help/version sources.

```bash
scripts/build-linux.sh
```

Produces:

- `linux/mmbasic` — headless stdio build (no SDL), useful for tests/automation.
- `linux/mmbasic-sdl` — SDL2 windowed build.

Build with `CC`/`CFLAGS` overrides if needed:
`scripts/build-linux.sh CC=gcc CFLAGS="-O0 -g"`.

## Run

```bash
./linux/mmbasic-sdl
```

A window opens; the interpreter REPL is shown in it. Type at the terminal (the
REPL reads stdin) — or with SDL keyboard input once focused. Output appears in
the window and on stdout (the serial mirror).

- `Alt+Enter` at the prompt toggles fullscreen on the primary display.
- `EDIT`, `FILES`, `WORDPAD`, `HELP`, `AFK`, and (with a network) `TERM` /
  `CONNECT` take over the screen; the REPL returns on exit.

Headless smoke test helpers: `MMB_SDL_DUMP=out.ppm` writes the framebuffer on
exit; `SDL_VIDEODRIVER=dummy` runs without a display.

## Storage

Physical drives map to host directories under `MMB_DRIVE_ROOT` (default
`~/.mmbasic`): `C:` → `$MMB_DRIVE_ROOT/C`, `D:` → `.../D`, up to `H:`. `A:` is
the in-RAM ramdisk seeded from `ramdisk/` at boot. Settings live in
`C:/.mmbasic.ini` (falling back to `A:`).

Bind a host directory to `D:` at startup with `--drive DIR`:

```bash
./linux/mmbasic-sdl --drive /media/usb
./linux/mmbasic --drive /media/usb 'DIR "D:/"'
```

The path is used as-is (absolute or relative), so the D: drive in MMBasic is
that folder. `--drive-root DIR` sets the base for the other drives. Run
`--help` for the full usage. Path lookups are case-insensitive to match FatFs,
even though the host filesystem may not be.

## Network

TCP client and server are implemented over BSD sockets (`linux/net_posix.c`)
with the same non-blocking contracts as the Circle transport, so `OPEN
"TCP:host:port"`, `CONNECT`, `TERM`, and the FTP server work. `IPCONFIG`
reports the active interface via `getifaddrs`. Wi-Fi radio scan/join is out of
scope on Linux; those options report unavailable.

## AppImage

`.github/workflows/linux-appimage.yml` builds `linux/mmbasic-sdl` on
`ubuntu-22.04` and packages it with `linuxdeploy` + `appimagetool`, then
attaches it to the rolling `linux-native` pre-release:

```
https://github.com/marnixk/mmcore/releases/download/linux-native/MMBasic-x86_64.AppImage
```

Build it locally on Linux with `scripts/package-linux-appimage.sh` (result in
`dist/`).

## Differences from the Pi build

- No Wi-Fi radio management (TCP over the host's existing connection only).
- Native pixels are RGB555 (green at bit 6) exactly like Circle's `COLOR16`;
  SDL presents through an RGB565 conversion.
- `wait_vsync`/page-flip are software (single framebuffer, SDL vsync pacing).
- The Circle `kernel.cpp` front end is not yet rewired onto the shared
  `mmbasic/src/frontend.c` REPL.
