#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Run after loading the corrected module, with other camera consumers closed.
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
if ! timeout --kill-after=3s 90s v4l2-compliance -d "$device" \
    > "$results/compliance.txt" 2>&1; then
    status=1
fi
cat "$results/compliance.txt"
for dimensions in '640 480' '1280 720' '1920 1080'; do
    read -r width height <<< "$dimensions"
    output="$results/${width}x${height}.mjpeg"
    timeout --kill-after=3s 30s v4l2-ctl -d "$device" \
        --set-fmt-video="width=$width,height=$height,pixelformat=MJPG" \
        --set-parm=30 --stream-mmap=4 --stream-count=120 --stream-to="$output"
    ffmpeg -v error -f mjpeg -i "$output" -f null - \
        2> "$results/${width}x${height}-decode.txt"
    test ! -s "$results/${width}x${height}-decode.txt"
    ffprobe -v error -f mjpeg -count_frames -select_streams v:0 \
        -show_entries stream=codec_name,width,height,nb_read_frames \
        -of default=noprint_wrappers=1 "$output" \
        > "$results/${width}x${height}-probe.txt"
    cat "$results/${width}x${height}-probe.txt"
    rg -qx 'codec_name=mjpeg' "$results/${width}x${height}-probe.txt"
    rg -qx "width=$width" "$results/${width}x${height}-probe.txt"
    rg -qx "height=$height" "$results/${width}x${height}-probe.txt"
    rg -qx 'nb_read_frames=120' "$results/${width}x${height}-probe.txt"
done
for fps in 15 30; do
    timeout --kill-after=3s 20s ffmpeg -nostdin -v error -f v4l2 \
        -input_format mjpeg -video_size 640x480 -framerate "$fps" \
        -i "$device" -frames:v 60 -f null - \
        2> "$results/live-${fps}fps.txt"
    test ! -s "$results/live-${fps}fps.txt"
done
# Leave the default webcam format selected after successful checks.
v4l2-ctl -d "$device" \
    --set-fmt-video=width=640,height=480,pixelformat=MJPG --set-parm=30
echo "Capture checks completed; compliance status=$status. Results: $results"
exit "$status"
