---
name: github-release
description: Build hardware MMBasic images and publish a semantic GitHub Release that includes the compiled kernels, firmware, install-sdcard.sh, and (when run on macOS) a signed arm64 app bundle. Use when the user asks to release, ship, publish, tag, cut a version, or run the github-release skill.
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

## 4. Publish

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
   binary as a signed `mmcore.app`, attached as `dist/mmcore-macos-arm64.zip`.
   Set `MMCORE_SKIP_MACOS=1` to skip it. The build sets
   `MMCORE_REQUIRE_NOTARY=1`: the bundle **must** be notarized + stapled, using
   the `mmcore-notary` keychain profile by default (override with
   `NOTARY_PROFILE`). If notarization or stapling fails the release aborts, so
   an unnotarized app cannot ship unnoticed.
4. Creates annotated tag `vVERSION` and pushes it to `origin`.
5. Creates the GitHub release with the zips, the macOS app (when built), **and**
   a top-level `install-sdcard.sh` asset.

The release notes include an "Install" section, the Linux AppImage, a "Windows
native (x86_64)" section, and a "macOS native (Apple Silicon)" section whenever
`dist/mmcore-macos-arm64.zip` exists at publish time.

A Windows zip is not built locally. The `.github/workflows/windows.yml`
workflow runs on `windows-latest` (MSYS2/MinGW) when the release is published
and attaches `mmcore-windows-x86_64.zip` to it (the release notes name the
asset regardless). Check the workflow run if the asset is missing:
`gh run list --workflow=windows.yml`.

Do not force-push tags. If `vVERSION` already exists, stop.

## 5. Afterward

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

## macOS app (Apple Silicon)

```bash
unzip mmcore-macos-arm64.zip
open mmcore.app
```

Build it standalone with `scripts/package-macos-app.sh` (needs Homebrew SDL2);
see [`docs/native-desktop.md`](../../../docs/native-desktop.md#macos-app-bundle).
