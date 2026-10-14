---
name: github-release
description: Build hardware MMBasic images and publish a semantic GitHub Release that includes the compiled kernels, firmware, install-sdcard.sh, and (when run on macOS) a signed universal (arm64 + x86_64) app bundle. Use when the user asks to release, ship, publish, tag, cut a version, or run the github-release skill.
---

# GitHub semantic release

Publish a **new semantic version** of the MMBasic console so consumers can
download one zip and write a bootable SD card with `install-sdcard.sh`.

Do **not** guess the version and do **not** start the hardware build until the
user has confirmed it.

## 1. Resolve versions (always first)

From the repository root:

```bash
scripts/github-release.sh last-version
scripts/github-release.sh next-minor
scripts/github-release.sh next-patch
scripts/github-release.sh next-major
```

- `last-version` is the highest `vX.Y.Z` GitHub release tag (git tags are the fallback).
- **Default next version is a minor bump** (`next-minor`). Example: last `0.1.1` → default `0.2.0`.

## 2. Ask the user, then stop

Tell the user the last published version and the default next version. Ask them
to pick one of:

| Reply | Result |
| --- | --- |
| yes / default / minor / empty | `next-minor` (default) |
| patch | `next-patch` |
| major | `next-major` |
| `X.Y.Z` or `vX.Y.Z` | that exact version |

**Stop and wait for their answer.** Do not build, tag, or call `gh release`
before they reply.

Skip this question only when the **same user message that invoked this skill**
already named an explicit version (`0.2.0`, `v0.2.0`, `patch`, `major`, or
`minor`).

## 3. Preflight

After the version `VERSION` is confirmed:

- Working tree should be clean except `dist/` and expected untracked junk. If
  it is not, stop and ask before committing or tagging.
- Prefer publishing from the default branch (`master`). If HEAD is elsewhere,
  say so and ask before continuing.
- Confirm `scripts/install-sdcard.sh` exists and is executable.
- Do **not** compile `picomite-fork/`. Hardware images come from
  `scripts/package-release.sh` (local `mmbasic/` + Circle).

## 4. Test only what changed

The full QEMU suite takes tens of minutes, and much of it exercises code that
rarely changes. Before publishing, select the suites related to the diff since
the last release and run only those:

```bash
scripts/release-tests.sh plan     # what would run, and why (no build, no QEMU)
scripts/release-tests.sh run      # build + run the selected suites
```

`scripts/release_tests.py` holds the mapping (data-driven `RULES`: changed-path
glob → test modules), and `scripts/release-tests.sh` is the wrapper (it finds
the project venv). Examples of the mapping:

- `mmbasic/src/cmd_wordpad.c` → `tests/test_wordpad*.py`
- `mmbasic/src/paint_*.c` → `tests/test_paint*.py`
- `console/wlan*.c` → `tests/test_wlan_*.py`

If any changed path is broad or cannot be mapped, it falls back to the **full
suite** so a release can never ship untested. The full fallback covers the core
interpreter (`mmbasic/src/core.c`, `expr.c`, `tok.c`, `vars.c`, `util.c`,
`gfx.c`, any `mmbasic/src/*.h`), the build (`scripts/build.sh`, `Makefile`),
Circle, `patches/`, and any source file with no mapping.

`plan` prints every selected suite and the reason it was selected (the rule and
the changed path that matched); capture that in the release log. `run` starts
the selection through `scripts/test-watch.py` — poll it with `status --sleep 60`
for the required percentage progress (see the `test-suite-progress` skill). Do
**not** publish until the selected suites are green. The same selection is
written into the GitHub release notes under `## Tests` by
`release_notes` in `scripts/github-release.sh`.

## 5. Publish

```bash
scripts/github-release.sh publish VERSION
```

That script:

1. Builds hardware kernels (`RASPPI=3` and `RASPPI=4`, `QEMU=0`) and packs:
   - `dist/mmcore-console-rpi3-vVERSION.zip`
   - `dist/mmcore-console-pizero2-vVERSION.zip`
   - `dist/mmcore-console-pizero2w-vVERSION.zip`
   - `dist/mmcore-console-pi400-vVERSION.zip`
   Zero 2 / Zero 2 W reuse the Pi 3 kernel. Each zip includes `install-sdcard.sh`.
2. Restores the QEMU Pi 3 Circle config so pytest still works.
3. On macOS, runs `scripts/package-macos-app.sh` and packages the native SDL2
   binary as a signed `mmcore.app`, attached as `dist/mmcore-macos-universal.zip`.
   This is mandatory on macOS — there is no skip. The build sets
   `MMCORE_REQUIRE_NOTARY=1`: the bundle **must** be notarized + stapled, using
   the `mmcore-notary` keychain profile by default (override with
   `NOTARY_PROFILE`). If notarization or stapling fails the release aborts, so
   an unnotarized app cannot ship unnoticed.
   - If `notarytool` cannot read the keychain (a non-interactive/agent session
     fails with `User interaction is not allowed`, or the profile is missing),
     do **not** stop and do **not** skip the macOS build. Notarize with App Store
     Connect credentials directly by exporting `NOTARY_APPLE_ID`,
     `NOTARY_TEAM_ID`, and `NOTARY_PASSWORD`; the script then prefers them over
     the keychain profile. The credentials live in the user's shell history
     (`~/.zsh_history`) from the last `xcrun notarytool store-credentials
     mmcore-notary` run.
4. Creates annotated tag `vVERSION` and pushes it to `origin`.
5. Creates the GitHub release with the zips, the macOS app (when built), **and**
   a top-level `install-sdcard.sh` asset.

The release notes include an "Install" section, the Linux AppImage, a "Windows
native (x86_64)" section, and a "macOS native (universal: Apple Silicon + Intel)" section whenever
`dist/mmcore-macos-universal.zip` exists at publish time. They also name the
Linux KMS/DRM framebuffer build (`mmcore-fb-linux-x86_64.tar.gz`) and the
bootable USB/ISO (compressed `mmcore-fb-x86_64.iso.zst` plus `install-usb.sh`;
the raw `.iso` is not published), which CI attaches on the release event
(`.github/workflows/linux-framebuffer.yml` and
`.github/workflows/linux-iso.yml`); see
[`docs/framebuffer-and-iso.md`](../../../docs/framebuffer-and-iso.md).

A Windows zip is not built locally. The `.github/workflows/windows.yml`
workflow runs on `windows-latest` (MSYS2/MinGW) when the release is published
and attaches `mmcore-windows-x86_64.zip` to it (the release notes name the
asset regardless). Check the workflow run if the asset is missing:
`gh run list --workflow=windows.yml`. The attach step needs
`export PATH="/c/Program Files/GitHub CLI:$PATH"` because the MSYS2 shell does
not inherit the runner's PATH; if the workflow fails only at "Attach to
release" with `gh: command not found`, download its `mmcore-Windows` artifact
(`gh run download <run-id> -n mmcore-Windows`) and finish with
`gh release upload vVERSION <zip> --clobber`.

Do not force-push tags. If `vVERSION` already exists, stop.

## 6. Afterward

- Show the release URL from `gh release view vVERSION`.
- Do not commit `dist/` (it is gitignored).
- If pytest was left with a Pi 4 Circle config because packaging was interrupted,
  restore with: `(cd circle && ./configure -r 3 -p aarch64-none-elf- --qemu -f)`.

## Consumer install (for release notes)

```bash
unzip mmcore-console-rpi3-vVERSION.zip
sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX
```

`--model pi400` selects the Pi 400 / Pi 4 zip. `--model pizero2` and
`--model pizero2w` select the Zero 2 zips (same `kernel8.img` as Pi 3).
`--update` refreshes kernel and firmware without wiping user files on `C:`.

## macOS app (universal)

```bash
unzip mmcore-macos-universal.zip
open mmcore.app
```

Build it standalone with `scripts/package-macos-app.sh` (needs Homebrew SDL2);
see [`docs/native-desktop.md`](../../../docs/native-desktop.md#macos-app-bundle).
