# Vendored libsmb2

`mmbasic/third_party/libsmb2/` is a vendored copy of
[libsmb2](https://github.com/sahlberg/libsmb2), the C SMB2/3 client library
used by the `OPTION NETWORK DRIVE` (Z:) backend in `mmbasic/src/netfs_smb2.c`.

- **Pinned version:** `v6.0.0` (tag), the release current as of 2026-10-08.
- **Upstream commit:** `fc710a3` is the head the v6.0.0 tag points at.
- **Vendored files:** `include/` and `lib/` (the client library sources),
  plus `COPYING` and `LICENCE-LGPL-2.1.txt`.
- **Licence:** the library (`lib/`, `include/`) is **LGPL-2.1-or-later**. See
  `LICENCE-LGPL-2.1.txt`. mmcore ships the full source of this tree, so the
  LGPL relinking obligation is met by public source availability; the release
  credits must carry the LGPL notice. (Not legal advice.)
- **Local patches:** `patches/0001-mingw-w64-modern.patch` (applied) makes the
  vendored tree build with modern MinGW-w64: `compat.h` no longer redeclares
  `gethostname`/`snprintf` for MinGW (winsock2.h provides them now) and
  `asprintf.h` skips its inline `asprintf`/`vasprintf` when the MinGW CRT
  already declares them. Both are backports of upstream master fixes. Apply
  from the repo root with `git apply`. List every patch here so the
  modifications stay public.

## Build shim

`config.h` in this directory is **mmcore-authored**, not upstream. Upstream
generates it with CMake/autoconf for each target; mmcore builds the library
from a plain `native/Makefile`, so this file supplies the `HAVE_*` feature
macros for the POSIX desktop targets (macOS and Linux). It is compiled with
`-DHAVE_CONFIG_H` and this directory on the include path.

The MinGW/Winsock target uses `include/win/config.h` (also mmcore-authored)
instead, selected by putting `include/win/` ahead of this directory on the
include path. It enables only the standard C headers MinGW-w64 ships and lets
`<compat.h>` supply the Winsock socket shims, mirroring how upstream's CMake
Windows path relies on `compat.h`.

A future bare-metal Circle port must not rely on this POSIX shim: it should
supply its own `config.h` (the upstream `include/esp/` or `include/picow/`
configs are the model) and a socket/libc shim, exactly as the design analysis
for #1128 describes. The Circle adapter is out of scope for this vendoring.

## Updating

Re-download the tag tarball, replace `lib/` and `include/`, re-check `config.h`
against `cmake/config.h.cmake`, and update the pinned version above.
