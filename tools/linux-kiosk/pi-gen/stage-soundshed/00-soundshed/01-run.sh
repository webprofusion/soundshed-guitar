#!/bin/bash -e
# Runs on the build host with ROOTFS_DIR set. build-image.sh has put the kiosk files in
# files/kiosk and the staged Linux distribution in files/app; the packages are already
# installed from 00-packages-nr.

stage_tmp="/tmp/soundshed-kiosk-install"

rm -rf "${ROOTFS_DIR}${stage_tmp}"
mkdir -p "${ROOTFS_DIR}${stage_tmp}"
cp -R files/kiosk "${ROOTFS_DIR}${stage_tmp}/kiosk"
cp -R files/app "${ROOTFS_DIR}${stage_tmp}/app"

on_chroot << EOF
bash "${stage_tmp}/kiosk/install.sh" --dist "${stage_tmp}/app" --no-apt
rm -rf "${stage_tmp}"
EOF
