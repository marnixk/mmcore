# Native desktop (SDL2) backend

A native build of the same MMBasic interpreter that runs on the Raspberry Pi,
for Linux, macOS, and Windows, using SDL2 for the window, keyboard, and audio.
It is a second backend behind the existing `mmb_platform` contract, not a fork:
language, graphics, commands, and tests live in `mmbasic/`.

## Build

Requirements: a C toolchain, `make`, `pkg-config`, and SDL2 development files
(`libsdl2-dev` on Debian/Ubuntu, `sdl2` from Homebrew on macOS), plus Python 3
for the generated ramdisk/help/version sources.

```bash
scripts/build-native.sh
```

Produces:

- `native/mmbasic` — headless stdio build (no SDL), useful for tests/automation.
- `native/mmcore` — SDL2 windowed build (the one shipped in desktop downloads).

Build with `CC`/`CFLAGS` overrides if needed:
`scripts/build-native.sh CC=gcc CFLAGS="-O0 -g"`.

## Windows

The same native backend builds for Windows x86_64 with MinGW-w64. In the MSYS2
MINGW64 shell:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make zip
scripts/build-windows.sh
```

That produces `native/mmbasic.exe` (headless) and `native/mmcore.exe`
(windowed, the one shipped in releases). A cross compiler works too:
`SDL2_ROOT=/path/to/SDL2-devel-*-mingw/x86_64-w64-mingw32 scripts/build-windows.sh`.
Winsock replaces the POSIX sockets backend and Wi-Fi radio features report
unavailable, as on Linux.

Package a distributable zip (the executable plus the `SDL2.dll` and
`libwinpthread-1.dll` it loads) with:

```bash
scripts/package-windows.sh
# -> dist/mmcore-windows-x86_64.zip
```

The `Windows build` GitHub Actions workflow runs this on `windows-latest` and
attaches `mmcore-windows-x86_64.zip` to every published GitHub release. The
persistent `C:` drive is `%USERPROFILE%\.mmbasic\C`.

### Windows Authenticode signing

Release Windows builds are **Authenticode-signed** so SmartScreen can build a
reputation for the publisher; locally they are unsigned by default. Signing is
performed by `scripts/sign-windows-exe.sh`, which `scripts/package-windows.sh`
calls right after staging `mmcore.exe`:

- `MMCORE_REQUIRE_WIN_SIGN=1` — release gate. A missing signing tool, missing
  certificate, or failed `signtool verify /pa` is fatal, so an unsigned zip
  cannot ship unnoticed (mirrors macOS `MMCORE_REQUIRE_NOTARY`). The
  `.github/workflows/windows.yml` release path sets this automatically.
- `MMCORE_SKIP_WIN_SIGN=1` — local escape hatch; ships unsigned. Conflicts with
  the release gate.
- Certificate selection: `WIN_SIGN_PFX`/`WIN_SIGN_PASSWORD` (a PKCS#12 file),
  `WIN_SIGN_PFX_BASE64` (the same file base64-encoded, for CI secrets),
  `WIN_SIGN_THUMBPRINT`, or `WIN_SIGN_SUBJECT` (Windows certificate store).
  `WIN_SIGN_COMMAND` runs a custom signer (DigiCert KeyLocker, Azure Trusted
  Signing) with the file as `$1`.
- Tool: `signtool` (Windows SDK) is preferred, with `osslsigncode` as the
  cross-platform fallback; override with `SIGNTOOL`/`WIN_SIGN_TOOL`.
- Every signature is timestamped (`WIN_SIGN_TIMESTAMP`, default
  `http://timestamp.digicert.com`) so it stays valid after the certificate
  expires.

In CI, store the certificate as GitHub Actions secrets: `WIN_SIGN_PFX_BASE64`,
`WIN_SIGN_PASSWORD`, and optionally `WIN_SIGN_TIMESTAMP` or `WIN_SIGN_COMMAND`.
Opening a signed but brand-new release can still show a SmartScreen prompt until
reputation accumulates; a certificate removes the “unknown publisher” path.
Users can confirm the signature with
`Get-AuthenticodeSignature .\mmcore.exe`.

## Run

```bash
./native/mmcore
```

A window opens; the interpreter REPL is shown in it. Type at the terminal (the
REPL reads stdin) — or with SDL keyboard input once focused. Output is rendered
in the window; the serial stream is only mirrored to stdout when it is not a
TTY (pipes, automation, `MMB_SDL_DUMP`) or when `MMB_SDL_SERIAL=1` is set, so an
interactive terminal is not spammed with a second copy.

- `Alt+Enter` at the prompt toggles fullscreen on the primary display. The
  graphics mode is integer-scaled and centred into the window/display, so the
  picture stays crisp with black bars filling any leftover area.
- `MM.RUNTIME$` is `linux`, `mac`, or `windows`; on the Pi build it is `pi`.
  `MM.HOST.HRES` / `MM.HOST.VRES` report the host window/display size, which
  can be larger than the mode the program selected.
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
./native/mmcore /path/to/SantaCatch.app   # mount read-only as B:, run MAIN.BAS, then exit
./native/mmcore --term bbs.example.net    # sealed TERM session
```

A positional argument that names an existing `.app` file mounts its host
directory on a physical drive, runs the package with the same B: read-only
semantics as REPL `RUN "name.app"`, and then exits. `--term [host[:port]]`
starts TERM (default port 23; no host opens the disconnected menu) and exits
when the session ends. Both modes are *sealed*: they never drop to the `> `
prompt and swallow BREAK / Ctrl+C. Pass `--repl` (or `--stay`) to return to the
interactive REPL instead, and a bare launch still opens the normal REPL. The
headless `native/mmbasic` accepts the same arguments.

## Storage

`A:` is the in-RAM ramdisk seeded from `ramdisk/` at boot. `C:` is the
persistent drive: it always maps to `$MMB_DRIVE_ROOT/C` (default
`~/.mmbasic/C`) and holds settings in `C:/.mmbasic.ini` (falling back to `A:`).
`D:` is not created by default; mount a host directory on it at startup with
`--drive DIR`:

```bash
./native/mmcore --drive /media/usb
./native/mmbasic --drive /media/usb 'DIR "D:/"'
```

The path is used as-is (absolute or relative), so the D: drive in MMBasic is
that folder. `--drive-root DIR` sets the base for `C:`. `E:`–`H:` are reserved
but never auto-created and are not exposed on the command line. Run `--help`
for the full usage. Path lookups are case-insensitive to match FatFs, even
though the host filesystem may not be.

## Network

TCP client and server are implemented over BSD sockets (`native/net_posix.c`)
with the same non-blocking contracts as the Circle transport, so `OPEN
"TCP:host:port"`, `CONNECT`, `TERM`, and the FTP server work. `IPCONFIG`
reports the active interface via `getifaddrs`. Wi-Fi radio scan/join is out of
scope on Linux; those options report unavailable.

## Native desktop vs Pi

| Area | Native (Linux/AppImage) | Pi (Circle) |
| --- | --- | --- |
| Graphics | same language + software rasterisers; RGB555 stored, RGB565 SDL present | RGB555 HDMI-native; `SetArea` DMA present and double-buffered VSync flip on Pi ≤ 4 |
| Audio | SDL2 (`PLAY TONE`/`MP3`/`MOD`/`XM`) | Circle HDMI / PWM audio |
| Filesystems | `A:` ramdisk; `C:` persistent host directory under `MMB_DRIVE_ROOT`; `--drive` mounts `D:` | `A:` ramdisk; `C:` SD card; `D:`– USB mass storage |
| TCP / `TERM` / `CONNECT` / FTP server | BSD sockets with the same non-blocking contract | Circle WLAN / Ethernet stack |
| Wi-Fi radio scan/join | unavailable (uses the host's network) | `OPTION WIFI` / `OPTIONS WIFI`, `OPTION ETHERNET` |
| Full-screen TUIs | `EDIT`/`FILES`/`WORDPAD`/`HELP`/`AFK`/`TERM` into the SDL framebuffer | same code, HDMI |
| vsync / page flip | software (single framebuffer, SDL vsync pacing) | hardware DMA, VSync flip |
| App-VM CLI | `.app` and `--term` sealed launches (#490/#491) | n/a (boots to the REPL) |

Native pixels are RGB555 (green at bit 6) exactly like Circle's `COLOR16`, so
the colour model matches; only the SDL present converts to RGB565. The Circle
`kernel.cpp` front end is not yet rewired onto the shared
`mmbasic/src/frontend.c` REPL.

## Development loop

Use the native build as the quick edit → build → test loop; escalate to QEMU
only when the change touches bare-metal specifics (Circle drivers, HDMI/DMA,
WLAN, SD-card FAT).

```bash
scripts/build-native.sh                                    # build both native binaries
.venv/bin/python -m pytest tests/test_linux_native.py     # REPL, storage, SDL, TUI, TCP
.venv/bin/python -m pytest tests/test_linux_net_srv.py    # POSIX TCP server (host loopback)
scripts/native-perf-check.sh                        # Pi-zero-overhead guard for the hot paths
```

`tests/test_linux_native.py` builds `native/mmbasic` itself and needs only a host
`cc`/`make`; SDL tests skip when `native/mmcore` was not built (no SDL2 dev
headers), and golden-image tests skip without ImageMagick. Escalate with
`scripts/build.sh` (needs the `aarch64-none-elf` cross-toolchain) followed by a
full `.venv/bin/python -m pytest`; the QEMU suite has no toolchain guard, so it
hard-fails rather than skips when the cross compiler or `qemu-system-aarch64` is
missing.

## AppImage

`.github/workflows/linux-appimage.yml` builds `native/mmcore` on
`ubuntu-22.04` and packages it with `linuxdeploy` + `appimagetool`, then
attaches it to the rolling `linux-native` pre-release. The AppImage is **Linux
x86_64 only**; `scripts/build-native.sh` itself also builds on macOS.

```
https://github.com/marnixk/mmcore/releases/download/linux-native/mmcore-x86_64.AppImage
```

Build it locally on Linux with `scripts/package-linux-appimage.sh` (result in
`dist/`).

## macOS app bundle

On an Apple Silicon Mac, `scripts/package-macos-app.sh` builds the same SDL2
binary and packages it as a self-contained `mmcore.app`:

```bash
brew install sdl2
scripts/package-macos-app.sh          # dist/mmcore.app + dist/mmcore-macos-arm64.zip
open dist/mmcore.app
```

SDL2 is the only non-system dependency; it is copied into
`Contents/Frameworks/` and the executable's install names are rewritten to load
it from `@rpath`, so the bundle runs on Macs without Homebrew.

The bundle is signed automatically: a keychain `Developer ID Application`
certificate is preferred, then `Apple Development` (fine on the build Mac), then
ad-hoc. Override with `SIGN_IDENTITY`, or `MMCORE_SKIP_SIGN=1` to leave it
unsigned (arm64 will refuse to launch an unsigned/modified binary). To ship to
other people without Gatekeeper warnings you need a Developer ID certificate
plus notarization:

```bash
SIGN_IDENTITY="Developer ID Application: ..." \
  NOTARY_PROFILE=mmcore-notary scripts/package-macos-app.sh
```

`NOTARY_PROFILE` is a `notarytool` keychain profile created with
`xcrun notarytool store-credentials`. Passing it notarizes and staples the
bundle; `scripts/github-release.sh publish` sets `MMCORE_REQUIRE_NOTARY=1`, which
makes notarization mandatory and defaults the profile to `mmcore-notary`, so a
release aborts if notarization or stapling fails. An unnotarized app therefore
cannot ship unnoticed. For a local build without a Developer ID, sign ad-hoc
(the default) and leave `NOTARY_PROFILE` unset; the mandatory default only
applies to release builds. `scripts/github-release.sh publish` builds and
attaches the zip on macOS automatically (skip with `MMCORE_SKIP_MACOS=1`).

