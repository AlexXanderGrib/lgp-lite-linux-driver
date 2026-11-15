<img src="assets/icon.svg" width="64" height="64" alt="">

# lgp-lite-linux-driver

Experimental Linux drivers for the **AVerMedia Live Gamer Portable Lite /
GL310**, USB **07ca:c835**:

- **Native V4L2 webcam:** the `lgplite` kernel module exposes hardware MJPEG
  through `/dev/video*` for webcam applications. No userspace bridge is required.
- **Standalone recorder:** a dependency-free Python driver records the hardware
  encoder's MPEG-TS stream with H.264 video and AAC audio through Linux usbfs.

**Verified on a real GL310:** native MJPEG capture at 640×480, 1280×720, and
1920×1080 at 30 fps; 15 fps at 640×480; all 63 V4L2 compliance and streaming
checks, including every advertised size/rate; and capture recovery after
driver unbind/rebind. A 30-second
native 1080p30 run decoded all 900 frames with no errors or timestamp warnings.
The module is installed through DKMS with Secure Boot signing. The Python
H.264/AAC recorder also passed decode and timestamp checks on a 45-second recording.

## Setup

The firmware is proprietary and is **not included**. Copy it from your Windows
installation or an AVerMedia GL310 driver installer/ZIP you already have:

```sh
tools/get-firmware.sh '/path/to/Windows/Program Files (x86)/AVerMedia'
# Or, with 7z installed:
tools/get-firmware.sh /path/to/GL310_driver.zip
```

The script verifies both SHA-256 hashes and creates `firmware/`, ignored by Git.
Driver packages 3.7.x.39 and 3.2802.x.40 contain the supported images. You can
also pass `--firmware DIR`, set `LGPLITE_FIRMWARE`, or put the images under
`~/.local/share/lgplite/` or `/lib/firmware/avermedia/`.

## Native V4L2 webcam

Install your distribution's kernel headers, DKMS, compiler, and make, then:

```sh
make -C kernel
sudo bash tools/install-native.sh
v4l2-ctl --list-devices
```

The installer copies the verified firmware to `/lib/firmware/avermedia/`,
registers the driver with DKMS for future kernel updates, and loads it.
With Secure Boot, DKMS signs using the distribution's module-signing key;
that key must already be enrolled. The development PC's existing key is enrolled.
The USB device ID lets the module load automatically on boot and reconnection.

Select **AVerMedia GL310** in the application's camera list. In OBS, add a
**Video Capture Device (V4L2)** source and choose **Motion-JPEG**. The initial
format is 640×480 at 30 fps. Advertised sizes are 640×480, 1280×720, and
1920×1080; advertised rates are 15 and 30 fps. Output dimensions and
frame rate must not exceed the HDMI source. All three sizes were tested at
30 fps; lower rates were tested at 640×480. The JPEG quality control is set
before starting capture.
24/25 fps output is not advertised because its firmware timestamps were
inconsistent in native application tests.

For a direct preview, substitute the actual GL310 node:

```sh
ffplay -f v4l2 -input_format mjpeg -video_size 640x480 -framerate 30 /dev/video0
```

The native interface supplies **video only**. HDMI audio is supported by the
standalone MPEG-TS recorder, but an ALSA microphone interface is not implemented.
Only one application can stream from the card at a time. Close camera consumers
before installing an updated module. `--no-load` installs for the next reboot
without attempting to reload the running module.

With `v4l-utils`, FFmpeg, and ripgrep installed, run the native checks with
other camera consumers closed:

```sh
bash tools/validate-native.sh
```

An additional hardware test exercises driver removal during capture, with an
open video handle, and verifies that capture starts again after rebinding:

```sh
sudo python3 tools/test-native-disconnect.py
```

Video device numbers can change after reconnecting. Select the card by its
**AVerMedia GL310** name, or find its current node with `v4l2-ctl --list-devices`.

To remove the native driver (leave other webcam drivers loaded):

```sh
sudo modprobe -r lgplite
sudo dkms remove lgplite/0.1.0 --all
sudo rm -r /usr/src/lgplite-0.1.0
```

## Standalone recorder setup

The native driver owns the USB interface while loaded. To use the standalone
recorder, first close webcam applications and run `sudo modprobe -r lgplite`.
After recording, `sudo modprobe lgplite` restores the native webcam.

Install USB access rules once, then unplug and reconnect the GL310:

```sh
sudo install -m 0644 udev/70-avermedia-lgp-lite.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

The rule grants access to the active local desktop user. For a remote/headless
session, configure USB permissions for that account or run the tool as root.
The rule is already installed on the development machine used for this project.

## Capture

Connect a source to **HDMI IN**, select stereo PCM audio on that source, then:

```sh
python3 userspace/lgplite.py signal
python3 userspace/lgplite.py capture recording.ts --seconds 30
ffplay recording.ts
```

Omit `--seconds` to record until Ctrl+C. Each capture loads and verifies firmware,
detects the input timing, and stops the encoder on exit. Existing output files
are preserved; choose a new name for each recording.

```sh
# Video bitrate in kbit/s (default 8000, range 1000–20000).
python3 userspace/lgplite.py capture recording-12mbps.ts --bitrate 12000

# Live preview: MPEG-TS goes to stdout, status messages to stderr.
python3 userspace/lgplite.py capture - | ffplay -i pipe:0

# Remux without re-encoding.
ffmpeg -i recording.ts -c copy recording.mkv
```

`--fps` changes the output frame rate. The default is the input rate, capped
at 30 fps for 1080p and 60 fps for 720p. `--timeout` controls how long to wait
without video before failing (default 5 seconds).

## Current limits

- Tested on GL310/C835 only. The native module binds only `07ca:c835`.
  The standalone recorder also recognizes the untested alternate `07ca:d835` ID.
- Mode tables cover progressive 1080p24/25/30/50/60 and 720p50/60. Only
  1080p60 input has been verified on hardware. Native output tests are listed
  above; the standalone H.264/AAC recorder was tested at 1080p30. Rates are nominal;
  fractional source rates have not been tested.
- Interlaced video, custom PC timings, compressed HDMI audio, and live input
  changes are unsupported. Restart capture after changing the HDMI mode.
- Capture reads the board's current HDMI input. MCU firmware updates, flash
  writes, input switching, and automatic hotplug recovery are not implemented.
- Only one application can claim the USB interface at a time.

## Diagnostics and tests

```sh
python3 userspace/lgplite.py info
python3 userspace/lgplite.py -v boot
python3 userspace/lgplite.py reg 0x38
python3 -m unittest discover -s tests -v
ffprobe -v error -show_streams recording.ts
ffmpeg -v error -i recording.ts -f null -
```

Verbose capture logs every firmware message and can be large. Register writes
are available for development (`reg ADDRESS VALUE`); normal capture needs no
manual register commands. If a run is interrupted by unplugging or a USB error,
reconnect the device and start a new capture.

The protocol and reverse-engineering sources are documented in
[docs/PROTOCOL.md](docs/PROTOCOL.md). Hardware test results are in
[docs/VALIDATION.md](docs/VALIDATION.md).
