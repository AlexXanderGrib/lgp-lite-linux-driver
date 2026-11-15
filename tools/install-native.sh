#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install the lgplite kernel module through DKMS, with its firmware.
set -euo pipefail

die() {
    echo "install-native.sh: $*" >&2
    exit 1
}

load_module=yes
if [[ ${1:-} == --no-load && $# == 1 ]]; then
    load_module=no
elif (( $# )); then
    echo 'Usage: sudo bash tools/install-native.sh [--no-load]' >&2
    exit 2
fi
(( EUID == 0 )) || die 'run with sudo: sudo bash tools/install-native.sh'

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kernel_version=$(uname -r)
module_version=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' "$project_dir/kernel/dkms.conf")
[[ -n $module_version ]] || die 'cannot read the version from kernel/dkms.conf'
source_dir=/usr/src/lgplite-$module_version

for tool in dkms make sha256sum modprobe; do
    command -v "$tool" >/dev/null || die "$tool is required but not installed"
done
[[ -d /lib/modules/$kernel_version/build ]] ||
    die "kernel headers for $kernel_version are not installed"
[[ -f $project_dir/firmware/qpvidfwusb.bin && -f $project_dir/firmware/qpaudfwusb.bin ]] ||
    die 'firmware/ is missing; run tools/get-firmware.sh first (see README)'
(
    cd "$project_dir/firmware"
    sha256sum --check --quiet <<'HASHES'
8da30f6dc2d4fee5aeb3e2e06df5581db4c01d146c50c21f6a5d4824025a47d1  qpvidfwusb.bin
0e8ea66b6a3ddb68f9a283f10155df90ce8d6f46599ed0ecf6f308d6c90210f4  qpaudfwusb.bin
HASHES
) || die 'firmware/ does not hold the supported images; rerun tools/get-firmware.sh'

install -d /lib/firmware/avermedia
install -m 0644 "$project_dir/firmware/qpvidfwusb.bin" \
    "$project_dir/firmware/qpaudfwusb.bin" /lib/firmware/avermedia/
if [[ -n $(dkms status -m lgplite -v "$module_version") ]]; then
    dkms remove -m lgplite -v "$module_version" --all
fi
rm -rf -- "$source_dir"
install -d "$source_dir"
for source in Makefile Kbuild dkms.conf lgplite.h lgplite-hw.c lgplite-video.c; do
    install -m 0644 "$project_dir/kernel/$source" "$source_dir/"
done
dkms add -m lgplite -v "$module_version"
dkms build -m lgplite -v "$module_version" -k "$kernel_version"
# With Secure Boot, DKMS signs the module with the machine owner key.
dkms install -m lgplite -v "$module_version" -k "$kernel_version"

if [[ $load_module == no ]]; then
    echo 'Installed the GL310 webcam driver for the next load or reboot.'
    exit 0
fi
# Reload only this driver, now that the new module is in place.
if [[ -d /sys/module/lgplite ]]; then
    modprobe -r lgplite ||
        die 'the old module is in use; close camera applications and rerun'
fi
modprobe lgplite || die 'the module did not load. Under Secure Boot the DKMS
signing key must be enrolled first: see "Secure Boot" in the README.'
udevadm settle
echo 'Installed the GL310 webcam driver. Find it with: v4l2-ctl --list-devices'
