#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Hardware checks for the native driver: V4L2 compliance including streaming,
# then capture and decode at every advertised size and rate.
#
# Needs v4l-utils and FFmpeg, an active HDMI source, and no other program
# using the card. Results are kept under captures/, which Git ignores.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
device=${1:-}
if [[ -z $device ]]; then
    for name_file in /sys/class/video4linux/video*/name; do
        [[ -r $name_file ]] || continue
        read -r camera_name < "$name_file"
        if [[ $camera_name == 'AVerMedia GL310' ]]; then
            device=/dev/$(basename -- "$(dirname -- "$name_file")")
            break
        fi
    done
fi
if [[ -z $device || ! -c $device ]]; then
    echo 'GL310 V4L2 node not found. Check USB connection and modprobe lgplite.' >&2
    exit 1
fi
mkdir -p "$project_dir/captures"
results=$(mktemp -d "$project_dir/captures/native-XXXXXX")
echo "Testing $device; results: $results"
timeout --kill-after=3s 20s v4l2-ctl -d "$device" --all --list-formats-ext \
    > "$results/device.txt" 2>&1
cat "$results/device.txt"
status=0
# -s streams with every I/O method, -f repeats that for every size and rate.
if ! timeout --kill-after=3s 300s v4l2-compliance -d "$device" -s -f \
    > "$results/compliance.txt" 2>&1; then
    status=1
fi
tail -n 40 "$results/compliance.txt"

mapfile -t sizes < <(v4l2-ctl -d "$device" --list-framesizes=MJPG |
    sed -n 's/.*Discrete \([0-9]*\)x\([0-9]*\).*/\1 \2/p')
(( ${#sizes[@]} )) || { echo 'The driver advertises no frame sizes.' >&2; exit 1; }
for dimensions in "${sizes[@]}"; do
    read -r width height <<< "$dimensions"
    name=${width}x${height}
    output=$results/$name.mjpeg
    timeout --kill-after=3s 30s v4l2-ctl -d "$device" \
        --set-fmt-video="width=$width,height=$height,pixelformat=MJPG" \
        --set-parm=30 --stream-mmap=4 --stream-count=120 --stream-to="$output"
    ffmpeg -v error -f mjpeg -i "$output" -f null - 2> "$results/$name-decode.txt"
    test ! -s "$results/$name-decode.txt"
    ffprobe -v error -f mjpeg -count_frames -select_streams v:0 \
        -show_entries stream=codec_name,width,height,nb_read_frames \
        -of default=noprint_wrappers=1 "$output" > "$results/$name-probe.txt"
    cat "$results/$name-probe.txt"
    grep -qx 'codec_name=mjpeg' "$results/$name-probe.txt"
    grep -qx "width=$width" "$results/$name-probe.txt"
    grep -qx "height=$height" "$results/$name-probe.txt"
    grep -qx 'nb_read_frames=120' "$results/$name-probe.txt"
    # Timestamps come from the encoder clock; FFmpeg reports any that repeat
    # or run backwards.
    for fps in 15 30; do
        timeout --kill-after=3s 20s ffmpeg -nostdin -v error -f v4l2 \
            -input_format mjpeg -video_size "$name" -framerate "$fps" \
            -i "$device" -frames:v 60 -f null - 2> "$results/$name-live-${fps}fps.txt"
        test ! -s "$results/$name-live-${fps}fps.txt"
    done
done
# Leave the driver's default format selected.
v4l2-ctl -d "$device" \
    --set-fmt-video=width=1280,height=720,pixelformat=MJPG --set-parm=30
echo "Capture checks completed; compliance status=$status. Results: $results"
exit "$status"
