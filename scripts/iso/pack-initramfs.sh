#!/bin/sh
# Build the small live initramfs: busybox, the musl loader, and the module
# closure needed to mount the squashfs root. The full rootfs is not packed.
#
#   pack-initramfs.sh ROOTFS OUT
#   pack-initramfs.sh --select-modules modules.dep   # print selected dep lines
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
INIT_SCRIPT="${SCRIPT_DIR}/live-init"

# Names are normalised (hyphen == underscore) against modules.dep basenames.
# uas is omitted on purpose: see live-init.
SEEDS="squashfs overlay isofs vfat fat ext4 nls_cp437 nls_iso8859_1 nls_utf8 nls_ascii \
usb-storage usbcore usb-common \
xhci-pci xhci-hcd ehci-pci ehci-hcd uhci-hcd ohci-pci ohci-hcd \
sd_mod sr_mod cdrom scsi_mod \
ata_piix ata_generic ahci libahci libata \
virtio_pci virtio_blk virtio_scsi virtio virtio_ring \
mmc_block mmc_core sdhci sdhci-pci \
loop"

die() {
	printf 'pack-initramfs: %s\n' "$*" >&2
	exit 1
}

select_modules() {
	depfile="$1"
	[ -f "$depfile" ] || die "no modules.dep at ${depfile}"
	awk -v seeds="$SEEDS" '
	function base(path,    n, a, name) {
		n = split(path, a, "/")
		name = a[n]
		sub(/\.ko(\..*)?$/, "", name)
		gsub(/-/, "_", name)
		return name
	}
	BEGIN {
		nseeds = split(seeds, s, " ")
		for (i = 1; i <= nseeds; i++) {
			key = s[i]
			gsub(/-/, "_", key)
			if (key != "") want[key] = 1
		}
	}
	{
		mod = $0
		sub(/:.*/, "", mod)
		gsub(/^[ \t]+|[ \t]+$/, "", mod)
		if (mod == "") next
		key = base(mod)
		path[key] = mod
		line[key] = $0
	}
	END {
		for (k in want) queue[k] = 1
		changed = 1
		while (changed) {
			changed = 0
			for (k in queue) {
				if (seen[k]) continue
				if (!(k in path)) continue
				seen[k] = 1
				ln = line[k]
				sub(/^[^:]+:[ \t]*/, "", ln)
				n = split(ln, deps, /[ \t]+/)
				for (i = 1; i <= n; i++) {
					if (deps[i] == "") continue
					dk = base(deps[i])
					if (dk == "uas") continue
					if (!queue[dk]) {
						queue[dk] = 1
						changed = 1
					}
				}
			}
		}
		for (k in seen) {
			ln = line[k]
			mod = path[k]
			sub(/^[^:]+:[ \t]*/, "", ln)
			n = split(ln, deps, /[ \t]+/)
			out = mod ":"
			for (i = 1; i <= n; i++) {
				if (deps[i] == "") continue
				dk = base(deps[i])
				if (seen[dk]) out = out " " deps[i]
			}
			print out
		}
	}
	' "$depfile"
}

if [ "${1:-}" = "--select-modules" ]; then
	[ -n "${2:-}" ] || die "--select-modules needs a modules.dep path"
	select_modules "$2"
	exit 0
fi

[ $# -eq 2 ] || die "usage: pack-initramfs.sh ROOTFS OUT"
ROOTFS="$1"
OUT="$2"
[ -d "$ROOTFS" ] || die "rootfs not found: ${ROOTFS}"
[ -f "$INIT_SCRIPT" ] || die "live init not found: ${INIT_SCRIPT}"
[ -x "${ROOTFS}/bin/busybox" ] || [ -f "${ROOTFS}/bin/busybox" ] \
	|| die "busybox missing in ${ROOTFS}/bin"

kver_dir=$(echo "${ROOTFS}"/lib/modules/*)
[ -d "$kver_dir" ] || die "no /lib/modules in the rootfs"
kver=$(basename "$kver_dir")

loader=$(echo "${ROOTFS}"/lib/ld-musl-*.so.1)
[ -f "$loader" ] || die "musl loader missing in ${ROOTFS}/lib"

stage=$(mktemp -d)
trap 'rm -rf "$stage" "${stage}.cpio"' EXIT

mkdir -p "${stage}/bin" "${stage}/lib/modules/${kver}" "${stage}/etc/modprobe.d"
cp -L "${ROOTFS}/bin/busybox" "${stage}/bin/busybox"
chmod 0755 "${stage}/bin/busybox"
cp -L "$loader" "${stage}/lib/$(basename "$loader")"
cp "$INIT_SCRIPT" "${stage}/init"
chmod 0755 "${stage}/init"
printf 'blacklist uas\n' > "${stage}/etc/modprobe.d/blacklist-uas.conf"

if [ -f "${kver_dir}/modules.builtin" ]; then
	cp "${kver_dir}/modules.builtin" "${stage}/lib/modules/${kver}/modules.builtin"
fi

dep_out="${stage}/lib/modules/${kver}/modules.dep"
select_modules "${kver_dir}/modules.dep" > "$dep_out"
grep -q squashfs "$dep_out" || die "squashfs was not selected from modules.dep"
while IFS= read -r dep_line; do
	rel=${dep_line%%:*}
	case "$(basename "$rel")" in
		uas.ko*) die "uas was selected into the initramfs" ;;
	esac
done < "$dep_out"

while IFS= read -r dep_line; do
	[ -n "$dep_line" ] || continue
	rel=${dep_line%%:*}
	rel=$(printf '%s' "$rel" | sed 's/^[ \t]*//;s/[ \t]*$//')
	[ -n "$rel" ] || continue
	[ -f "${kver_dir}/${rel}" ] || die "missing module ${rel}"
	mkdir -p "${stage}/lib/modules/${kver}/$(dirname "$rel")"
	cp "${kver_dir}/${rel}" "${stage}/lib/modules/${kver}/${rel}"
done < "${stage}/lib/modules/${kver}/modules.dep"

mkdir -p "$(dirname "$OUT")"
(
	cd "$stage"
	find . -print0 | cpio --null -o -H newc
) > "${stage}.cpio"
gzip -9 -c "${stage}.cpio" > "$OUT"
rm -f "${stage}.cpio"
[ -s "$OUT" ] || die "empty initramfs"
