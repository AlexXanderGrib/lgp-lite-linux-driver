<img src="assets/icon.svg" width="64" height="64" alt="">

# lgp-lite-linux-driver

Linux drivers for the **AVerMedia Live Gamer Portable Lite (GL310)**, USB ID
`07ca:c835`. The card has no UVC interface and no upstream Linux support.

- **Native V4L2 webcam.** The `lgplite` kernel module exposes the card's
  hardware MJPEG encoder as an ordinary `/dev/video*` device for OBS, browsers,
  Discord, FFmpeg and other camera applications.
- **Programmable button.** The kernel module also reports the button on top
  of the card as a key, for a desktop shortcut or the included helper that
  runs a command on each click. The light around it turns solid while the
  card records, as with the vendor software, and can be set to other
  patterns.
- **Standalone recorder.** A dependency-free Python tool records the hardware
  encoder's MPEG-TS stream (H.264 video, AAC audio) through usbfs, with no
  kernel module.

This is an experimental driver written from reverse engineering and tested on
one GL310. See [Current limits](#current-limits) before relying on it.

| | Native webcam | Standalone recorder |
| --- | --- | --- |
| Output | MJPEG, video only | MPEG-TS file or pipe, H.264 + AAC |
| Sizes | 640×360, 960×540, 1280×720, 1920×1080 | Same as the HDMI input |
| Frame rates | 15 or 30 fps | Up to 30 fps at 1080p, 60 fps at 720p |
| Needs | Kernel headers, DKMS | Python 3 |

## Firmware

The card's encoder runs firmware that the host uploads each time capture
starts. That firmware is proprietary and is **not included**. Copy it from a
Windows installation of the AVerMedia software, or from an AVerMedia GL310
driver installer or ZIP that you already have:

```sh
tools/get-firmware.sh '/path/to/Windows/Program Files (x86)/AVerMedia'
# Or, with 7z installed:
tools/get-firmware.sh /path/to/GL310_driver.zip
```

The script checks both files against known SHA-256 hashes and writes them to
`firmware/`, which Git ignores. Driver packages 3.7.x.39 and 3.2802.x.40 contain
the supported images.

## Native V4L2 webcam

Requires Linux 6.2 or newer with kernel headers, DKMS, a compiler and make.
The module builds against 6.2, 6.17 and 7.0; it has been run on 7.0 only.

```sh
sudo bash tools/install-native.sh
v4l2-ctl --list-devices
```

The installer copies the firmware to `/lib/firmware/avermedia/`, registers the
module with DKMS so it is rebuilt on kernel updates, and loads it. The module
then loads by itself whenever the card is connected.

Select **AVerMedia GL310** in the application's camera list. In OBS, add a
**Video Capture Device (V4L2)** source and choose **Motion-JPEG**. For a direct
preview, substitute the card's actual node:

```sh
ffplay -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 /dev/video0
```

The default format is 1280×720 at 30 fps. The encoder scales the HDMI picture
down but not up, so the size and rate must not exceed those of the HDMI source.
JPEG quality is the `compression_quality` control, 1 to 100, and can be changed
while the card is not streaming:

```sh
v4l2-ctl -d /dev/video0 --set-ctrl=compression_quality=90
```

The native interface supplies **video only**; an ALSA capture device for HDMI
audio is not implemented. Only one application can stream from the card at a
time. Video device numbers can change after reconnecting, so select the card
by name.

### Button and light

The Windows software uses the button on top of the card to launch itself. The
module reports it as a key, `KEY_RECORD` (keysym `XF86AudioRecord`), on an
input device named **AVerMedia GL310 Button**, whether or not anything is
streaming. The card reports clicks only: there is no release, a long hold
reports nothing, and the second press of a double click is usually lost. The
driver polls the button every 125 ms while a program has the input device
open; desktop sessions keep it open.

To bind it in GNOME, open Settings → Keyboard → View and Customize Shortcuts →
Custom Shortcuts, add a shortcut with your command, and press the button when
asked for the shortcut. Other desktops and applications that accept hotkeys
work the same way.

The light ring around the button follows the **Button Light** control of the
video device. Its default, Automatic, runs the LEDs clockwise while the card
is idle and lights the ring solid while an application captures from it. The
other values select one pattern: Clockwise, Solid, Fade or Fast Clockwise.
Writing a value restarts its animation. The standalone recorder also lights
the ring solid while it records.

```sh
v4l2-ctl -d /dev/video0 --set-ctrl=button_light=2   # solid
v4l2-ctl -d /dev/video0 --set-ctrl=button_light=0   # automatic
```

`userspace/lgplite-button.py` runs a command for each click without a desktop
shortcut, and answers each click with about two turns of Fast Clockwise
before the light returns to its previous pattern (`--no-light` turns this
off). It needs read access to the input device. This udev rule grants that,
and the recorder's USB access, to the user at the local desktop; reconnect
the card after installing it:

```sh
sudo install -m 0644 udev/70-avermedia-lgp-lite.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

```sh
python3 userspace/lgplite-button.py               # print each click
python3 userspace/lgplite-button.py --click obs   # start OBS
```

The helper waits while the card is unplugged. To start it with the desktop
session, create `~/.config/systemd/user/lgplite-button.service`, replacing
the path and command:

```ini
[Unit]
Description=Live Gamer Portable Lite button

[Service]
ExecStart=/usr/bin/python3 /path/to/lgp-lite-linux-driver/userspace/lgplite-button.py --click obs
Restart=on-failure

[Install]
WantedBy=graphical-session.target
```

```sh
systemctl --user enable --now lgplite-button.service
```

To report a different key, map the button's scancode 0 in a udev hwdb file
such as `/etc/udev/hwdb.d/70-lgplite-button.hwdb`, then run
`sudo systemd-hwdb update` and reconnect the card:

```
evdev:name:AVerMedia GL310 Button:*
 KEYBOARD_KEY_0=prog1
```

### Secure Boot

With Secure Boot enabled the kernel only loads signed modules. DKMS signs
`lgplite` with the machine owner key (MOK), which must be enrolled once. If
`modprobe lgplite` fails with `Key was rejected by service`, enroll the key
your distribution's DKMS uses and reboot. On Ubuntu:

```sh
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der
```

### Updating and removing

Close camera applications and run the installer again to update. Pass
`--no-load` to install for the next reboot without reloading the running
module. To remove the driver:

```sh
sudo modprobe -r lgplite
sudo dkms remove lgplite/0.1.0 --all
sudo rm -r /usr/src/lgplite-0.1.0
sudo rm /lib/firmware/avermedia/qpvidfwusb.bin /lib/firmware/avermedia/qpaudfwusb.bin
```

### Hardware checks

With `v4l-utils` and FFmpeg installed, an active HDMI source, and other camera
applications closed:

```sh
bash tools/validate-native.sh
sudo python3 tools/test-native-disconnect.py
```

The first runs `v4l2-compliance` including streaming, then captures and decodes
every advertised size and rate. The second unbinds the driver in the middle of
a capture while holding the video device open, and checks that capture starts
again after rebinding.

## Standalone recorder

The kernel module owns the USB interface while it is loaded. To use the
recorder, close camera applications and run `sudo modprobe -r lgplite`;
`sudo modprobe lgplite` restores the webcam afterwards.

Install the access rule once, then unplug and reconnect the card:

```sh
sudo install -m 0644 udev/70-avermedia-lgp-lite.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

The rule grants access to the user logged in at the local desktop. For a
remote or headless session, grant that account access to the USB device, or
run the tool as root.

Connect a source to **HDMI IN**, select stereo PCM audio on that source, then:

```sh
python3 userspace/lgplite.py signal
python3 userspace/lgplite.py capture recording.ts --seconds 30
ffplay recording.ts
```

Omit `--seconds` to record until Ctrl+C. Each capture loads and verifies the
firmware, detects the input timing, and stops the encoder on exit. Existing
output files are never overwritten; choose a new name for each recording.

```sh
# Video bitrate in kbit/s (default 8000, range 1000–20000).
python3 userspace/lgplite.py capture recording-12mbps.ts --bitrate 12000

# Live preview: MPEG-TS goes to stdout, status messages to stderr.
python3 userspace/lgplite.py capture - | ffplay -i pipe:0

# Remux without re-encoding.
ffmpeg -i recording.ts -c copy recording.mkv
```

`--fps` changes the output frame rate. The default is the input rate, capped
at 30 fps for 1080p and 60 fps for 720p. `--timeout` sets how long to wait
without video before failing (default 5 seconds). The recorder finds the
firmware in `firmware/`, in the directory given by `--firmware DIR` or
`LGPLITE_FIRMWARE`, in `~/.local/share/lgplite/`, or in
`/lib/firmware/avermedia/`.

## Current limits

- Tested on one GL310 (`07ca:c835`) with a 1080p60 source. The recorder also
  accepts the alternate ID `07ca:d835`, which is untested.
- Supported HDMI inputs are progressive 1080p at 24, 25, 30, 50 or 60 Hz and
  720p at 50 or 60 Hz. Only 1080p60 has been verified on hardware. Interlaced
  video, PC timings such as 1440p, fractional rates, and compressed HDMI audio
  are not supported.
- Capture uses the HDMI mode present when it starts; restart it after
  changing the source's mode. Suspend and resume during capture is untested.
- The webcam does not offer 24 or 25 fps: the firmware's timestamps were
  inconsistent at those rates.
- No HDMI audio in the native driver. MCU firmware updates and flash writes
  are not implemented.
- The button reports clicks only: no release, no long press, and usually no
  second press of a double click.
- Timestamps follow the encoder's clock, anchored to the host clock when
  capture starts. The two clocks can drift apart in very long sessions.

## Troubleshooting

- `Firmware avermedia/qpvidfwusb.bin is missing` in `dmesg`: run
  `tools/get-firmware.sh`, then the installer again.
- Starting capture fails with `Link has been severed`: no HDMI signal. With
  `Numerical result out of range`, the source's mode is unsupported or smaller
  than the requested size; `dmesg` names the detected timing.
- The card stops answering after an interrupted session: unplug it, wait five
  seconds, and reconnect it.
- The recorder reports that the device is in use: unload the kernel module
  first, as described above.

Recorder diagnostics:

```sh
python3 userspace/lgplite.py info
python3 userspace/lgplite.py -v boot
python3 userspace/lgplite.py reg 0x38
```

Verbose capture logs every firmware message. Register writes are available for
development (`reg ADDRESS VALUE`); normal capture needs none.

## Development

```sh
make -C kernel                          # build the module in place
python3 -m unittest discover -s tests   # userspace unit tests, no hardware
```

The protocol and how it was established are documented in
[docs/PROTOCOL.md](docs/PROTOCOL.md). Hardware test results are in
[docs/VALIDATION.md](docs/VALIDATION.md).

## License and disclaimer

GPL-2.0-only; see [LICENSE](LICENSE).

This project is not affiliated with or endorsed by AVerMedia. AVerMedia and
Live Gamer Portable are trademarks of their owner. The protocol was worked out
for interoperability by observing the device and analysing the vendor's
Windows driver; the repository contains no vendor code and no firmware. The
software comes with no warranty: it uploads firmware to and reconfigures your
capture card, so use it at your own risk.
