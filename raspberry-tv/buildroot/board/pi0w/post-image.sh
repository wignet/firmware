#!/bin/bash
# The SD card image: boot | rootfs A | rootfs B (reserved) | data.
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# The shared firmware and config.txt at the top; slot p2's kernel, device
# tree, overlays and cmdline.txt in p2/ (os_prefix=p2/), the same files as
# its rootfs's /boot.  p3/ appears with the first over-the-air update.
rm -rf "${BINARIES_DIR}/p2"
cp -r "${TARGET_DIR}/boot" "${BINARIES_DIR}/p2"
rm "${BINARIES_DIR}/p2/config.txt"
cp "${TARGET_DIR}/boot/config.txt" "${BINARIES_DIR}/config.txt"
FILES=( rpi-firmware/bootcode.bin rpi-firmware/start.elf rpi-firmware/fixup.dat config.txt p2 )
BOOT_FILES=$(printf '\\t\\t\\t"%s",\\n' "${FILES[@]}")
sed "s|#BOOT_FILES#|${BOOT_FILES}|" "${BOARD_DIR}/genimage.cfg" >"${GENIMAGE_CFG}"

# An empty rootpath: data.ext4 is created empty, rootfs.ext4 is already built.
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"
rm -rf "${GENIMAGE_TMP}"
genimage \
	--rootpath "${ROOTPATH_TMP}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${GENIMAGE_CFG}"
