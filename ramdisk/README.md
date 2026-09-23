# ramdisk/

Files under this folder are embedded into the kernel at build time and seeded
into the in-memory A: ramdisk at boot, preserving the tree. `ramdisk/lib/x.inc`
becomes `A:/lib/x.inc`, and so on.

- **Dotfiles are skipped.** Files and directories whose name starts with `.`
  (for example `.gitkeep`, `.DS_Store`) are not embedded.
- **RAM cost.** Seeded files are copied into the heap as ordinary writable A:
  files, so the whole tree consumes RAM. Keep it small.
- **Limits.** Each path component must be at most 79 characters (the
  `vfs_node.name[80]` limit) and the tree must fit the `VFS_MAX` node budget in
  `mmbasic/src/vfs.c`; a violation fails the build with a clear message.
- **Generated code.** `scripts/gen_ramdisk.py` emits
  `mmbasic/src/ramdisk_data.c` (git-ignored). `console/Makefile` reruns it when
  this folder or the generator changes. Empty or missing folders build fine.
- **Re-seeding.** Seeding runs on every boot after A: is cleared, so a reboot
  restores the kernel image's copy (overwriting edits to seeded paths).
- **Release builds** can omit a top-level folder with
  `RAMDISK_EXCLUDE=<name>` (see `scripts/package-release.sh`).

Starter content lives in `lib/` and `apps/`; the real catalogue is follow-up
work.

## Fonts

- **`fonts/`** — bitmap font JSON descriptors (see `fonts/fonts.inc` and
  `A:/fonts/<name>.json`).
- **`fonts/tdf/`** — curated TheDraw `.TDF` fonts from the [Roy/SAC TheDraw TDF
  collection](https://www.roysac.com/images/galleries/ZIP/ROYS-THEDRAW_TDF_FONTS_COLLECTION.ZIP)
  (30 files, ~153 KiB). This is a shortlist for ramdisk seeding, not the full
  zip; provenance and file list are in `fonts/tdf/README.md`. Runtime loading is
  tracked under issue #511; assets ship under #554.
