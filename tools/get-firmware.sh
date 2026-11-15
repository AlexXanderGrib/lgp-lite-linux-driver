#!/bin/sh
# Collect the two firmware images the Live Gamer Portable Lite needs.
#
# The device has no firmware of its own in flash for the codec SoC: the host
# uploads a video image (qpvidfwusb.bin) and an audio image (qpaudfwusb.bin)
# on every plug-in.  Both are proprietary AVerMedia files and are not shipped
# with this repository; this script copies them out of something you already
# have and checks them against known-good hashes.
#
# usage: get-firmware.sh [-o OUTDIR] SOURCE
#
#   SOURCE may be
#     - a directory that contains the two .bin files somewhere below it, e.g.
#       a mounted Windows install ("/mnt/win/Program Files (x86)/AVerMedia")
#     - an AVerMedia driver installer (.exe) or the downloaded .zip that
#       contains the installers (needs 7z)
#
#   OUTDIR defaults to ./firmware
set -eu

VID_NAME=qpvidfwusb.bin
AUD_NAME=qpaudfwusb.bin
# Shipped unchanged in driver packages 3.7.x.39 (2015) and 3.2802.x.40 (2020).
VID_SHA=8da30f6dc2d4fee5aeb3e2e06df5581db4c01d146c50c21f6a5d4824025a47d1
AUD_SHA=0e8ea66b6a3ddb68f9a283f10155df90ce8d6f46599ed0ecf6f308d6c90210f4

out=firmware
while getopts o:h opt; do
    case $opt in
        o) out=$OPTARG ;;
        *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { echo "usage: $0 [-o OUTDIR] SOURCE" >&2; exit 2; }
src=$1

tmp=
cleanup() { [ -n "$tmp" ] && rm -rf "$tmp"; return 0; }
trap cleanup EXIT

sha() { sha256sum "$1" | cut -d' ' -f1; }

# Print the first file below $1 named $2 whose hash is $3.
find_good() {
    find "$1" -type f -iname "$2" 2>/dev/null | while IFS= read -r f; do
        if [ "$(sha "$f")" = "$3" ]; then
            printf '%s\n' "$f"
            break
        fi
    done
}

unpack() {
    command -v 7z >/dev/null || { echo "7z is needed to unpack $1 (package: 7zip)" >&2; exit 1; }
    mkdir -p "$2"
    7z x -y -bso0 -bsp0 -o"$2" "$1" >/dev/null
}

if [ -f "$src" ]; then
    tmp=$(mktemp -d)
    unpack "$src" "$tmp/a"
    # The .zip holds one NSIS installer per Windows version; the firmware is
    # identical in all of them, so unpack them until one yields both files.
    n=0
    find "$tmp/a" -type f -iname '*_Install.exe' | sort | while IFS= read -r exe; do
        n=$((n + 1))
        unpack "$exe" "$tmp/b$n"
        if [ -n "$(find_good "$tmp/b$n" "$VID_NAME" "$VID_SHA")" ]; then
            break
        fi
    done
    src=$tmp
elif [ ! -d "$src" ]; then
    echo "$src: no such file or directory" >&2
    exit 1
fi

vid=$(find_good "$src" "$VID_NAME" "$VID_SHA")
aud=$(find_good "$src" "$AUD_NAME" "$AUD_SHA")

if [ -z "$vid" ] || [ -z "$aud" ]; then
    echo "Could not find both firmware files with the expected contents below $1." >&2
    [ -z "$vid" ] && echo "  missing or unrecognised: $VID_NAME" >&2
    [ -z "$aud" ] && echo "  missing or unrecognised: $AUD_NAME" >&2
    echo "Files with these names that were seen:" >&2
    find "$src" -type f \( -iname "$VID_NAME" -o -iname "$AUD_NAME" \) -exec sha256sum {} \; >&2 || true
    exit 1
fi

mkdir -p "$out"
install -m 0644 "$vid" "$out/$VID_NAME"
install -m 0644 "$aud" "$out/$AUD_NAME"
echo "Firmware written to $out/:"
ls -l "$out/$VID_NAME" "$out/$AUD_NAME"
