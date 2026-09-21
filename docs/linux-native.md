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
REPL reads stdin) — or with SDL keyboard input once focused. Output is rendered
in the window; the serial stream is only mirrored to stdout when it is not a
TTY (pipes, automation, `MMB_SDL_DUMP`) or when `MMB_SDL_SERIAL=1` is set, so an
interactive terminal is not spammed with a second copy.

- `Alt+Enter` at the prompt toggles fullscreen on the primary display.
- `EDIT`, `FILES`, `WORDPAD`, `HELP`, `AFK`, and (with a network) `TERM` /
  `CONNECT` take over the screen; the REPL returns on exit.
- Launched from a desktop entry (`Terminal=false`), stdin is `/dev/null`, which
  is ignored rather than treated as end-of-input.

Headless smoke test helpers: `MMB_SDL_DUMP=out.ppm` writes the framebuffer on
exit; `SDL_VIDEODRIVER=dummy` runs without a display.

### App-VM launch

The native binaries also act as a simple VM for packaged `.APP` files and for
TERM sessions, so the AppImage is a portable app runner:

```bash
./linux/mmbasic-sdl /path/to/SantaCatch.app   # mount read-only as B:, run MAIN.BAS, then exit
./linux/mmbasic-sdl --term bbs.example.net    # sealed TERM session
```

A positional argument that names an existing `.app` file mounts its host
directory on a physical drive, runs the package with the same B: read-only
semantics as REPL `RUN "name.app"`, and then exits. `--term [host[:port]]`
starts TERM (default port 23; no host opens the disconnected menu) and exits
when the session ends. Both modes are *sealed*: they never drop to the `> `
prompt and swallow BREAK / Ctrl+C. Pass `--repl` (or `--stay`) to return to the
interactive REPL instead, and a bare launch still opens the normal REPL. The
headless `linux/mmbasic` accepts the same arguments.

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
https://github.com/marnixk/mmcore/releases/download/linux-native/mmcore-x86_64.AppImage
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
