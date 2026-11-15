#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

load_module=yes
if [[ ${1:-} == --no-load && $# == 1 ]]; then
    load_module=no
elif (( $# )); then
    echo 'Usage: install-native.sh [--no-load]' >&2
    exit 2
fi

if (( EUID != 0 )); then
    echo "Run with sudo: sudo bash tools/install-native.sh" >&2
    exit 1
fi
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kernel_version=$(uname -r)
module_version=0.1.0
for tool in dkms make sha256sum modprobe; do
    command -v "$tool" >/dev/null
done
test -d "/lib/modules/$kernel_version/build"
(
    cd "$project_dir/firmware"
    sha256sum --check <<'HASHES'
8da30f6dc2d4fee5aeb3e2e06df5581db4c01d146c50c21f6a5d4824025a47d1  qpvidfwusb.bin
0e8ea66b6a3ddb68f9a283f10155df90ce8d6f46599ed0ecf6f308d6c90210f4  qpaudfwusb.bin
HASHES
)

install -d /lib/firmware/avermedia "/usr/src/lgplite-$module_version"
install -m 0644 "$project_dir/firmware/qpvidfwusb.bin" \
    "$project_dir/firmware/qpaudfwusb.bin" /lib/firmware/avermedia/
if [[ -n $(dkms status -m lgplite -v "$module_version") ]]; then
    dkms remove -m lgplite -v "$module_version" --all
fi
for source in Makefile Kbuild dkms.conf lgplite.h lgplite-hw.c lgplite-video.c; do
    install -m 0644 "$project_dir/kernel/$source" "/usr/src/lgplite-$module_version/"
done
dkms add -m lgplite -v "$module_version"
dkms build -m lgplite -v "$module_version" -k "$kernel_version"
dkms install -m lgplite -v "$module_version" -k "$kernel_version"
# DKMS signs with the distribution's MOK when Secure Boot is enabled.
# Reload only this driver, after the new module is successfully installed.
if [[ $load_module == no ]]; then
    echo 'Installed native GL310 webcam driver for the next load/reboot.'
    exit 0
fi
if [[ -d /sys/module/lgplite ]]; then
    modprobe -r lgplite
fi
modprobe lgplite
udevadm settle
echo 'Installed native GL310 webcam driver. Find it with: v4l2-ctl --list-devices'
