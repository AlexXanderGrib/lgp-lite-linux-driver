# Hardware validation

All tests used one GL310 / C835, USB 07ca:c835, chip revision 0x00010020. The
HDMI input was another PC producing progressive 1920×1080 at 60 Hz with stereo
PCM audio at 48 kHz. Firmware, recordings and test logs are not part of the
repository.

## Native V4L2 webcam, 2026-11-16

Linux 7.0.0-43-generic (Ubuntu 26.04), Secure Boot enabled, module installed
through DKMS and signed with an enrolled machine owner key. v4l-utils 1.32.0.

| Check | Result |
| --- | --- |
| `v4l2-compliance -s -f` | 65 checks passed, no failures, no warnings |
| 640×360 MJPEG at 30 fps | 120 frames captured, every frame decoded |
| 960×540 MJPEG at 30 fps | 120 frames captured, every frame decoded |
| 1280×720 MJPEG at 30 fps | 120 frames captured, every frame decoded |
| 1920×1080 MJPEG at 30 fps | 120 frames captured, every frame decoded |
| Live FFmpeg capture, each size at 15 and 30 fps | 60 frames each, empty error log |
| 30-second 1080p30 FFmpeg run | 900 frames, no warnings, no duplicates or drops |
| JPEG quality control | 45, 132 and 222 kB per 720p frame at quality 10, 75 and 100; rejected with EBUSY while streaming |
| Driver unbind during capture | Capture ends; the open descriptor returns ENODEV and signals error/hangup |
| Rebind with the old descriptor open | A new node registers and responds; closing the old one is safe |
| Capture after rebind | Starts again and decodes 30 frames without errors |
| Capture thread CPU use | About 3% of one core at 1080p30 |
| Module build | 6.2.0-23, 6.17.0-41 and 7.0.0-43 headers with `W=1`, no compiler warnings |

The compliance and capture rows are reproduced by `bash tools/validate-native.sh`,
the unbind and rebind rows by `sudo python3 tools/test-native-disconnect.py`.
The compliance run covers read(), mmap, select, epoll, REQBUFS and CREATE_BUFS
for every advertised size and rate. DMA-BUF export is checked, but importing
buffers from another allocator was not. USERPTR is not offered.

The unbind test detaches the driver through sysfs. It does not cut USB power,
so it does not prove behaviour when the cable is pulled in the middle of a
capture. Plugging the card in again was exercised repeatedly during
development and the module bound each time.

### Findings that shaped the driver

- Firmware messages split a JPEG when the circular stream buffer wraps.
  Partial frames are kept across messages and published only after the end
  marker.
- Mailbox timestamps count at 22.5 kHz. Using them instead of USB arrival
  times removed the duplicate-timestamp warnings seen in early FFmpeg runs.
- `soft_unbind` keeps the USB endpoints usable until the driver has joined its
  worker and stopped the encoder. Disabling transfers earlier left the card
  unresponsive until it was physically reconnected.
- Output at 24 and 25 fps produced inconsistent firmware timestamps, so the
  driver advertises 15 and 30 fps only.
- The scaler stretches the picture to the requested size. 640×480 distorted a
  16:9 source, so the driver advertises 16:9 sizes only.

## Standalone recorder, 2026-11-15

```sh
python3 userspace/lgplite.py capture verified-1080p30.ts --seconds 45
```

| Measurement | Result |
| --- | --- |
| Video | H.264 High, 1920×1080, nominal 30 fps |
| Audio | AAC LC, stereo, 48 kHz |
| Configured video bitrate | 8000 kbit/s |
| Measured total TS bitrate | 8,225,282 bit/s |
| Duration in file | 44.935233 seconds |
| File size | 46,200,624 bytes |
| Decoded video frames | 1,348 |
| Decoded audio frames | 2,105 |
| Full video + audio decode | FFmpeg returned 0 with empty error output |
| DTS audit | Strictly increasing for both streams across the entire file |

The wall-clock limit includes encoder startup, so the media duration is
slightly shorter than requested.

Other recorder checks:

- Both firmware images passed read-back comparison after every upload.
- A second recording at 12,000 kbit/s captured 6,057,172 bytes in four seconds.
- Repeated captures reload firmware, stop normally, and release the USB
  interface.
- Closing a stdout consumer early stops the encoder and exits without a
  traceback. SIGINT and SIGTERM stop capture and release the device.
- Recording over an existing file fails and leaves that file untouched.
- Invalid durations, timeouts, frame rates and bitrates fail before any USB
  access.
- Fourteen unit tests pass, covering split TS packets, DMA chunking, invalid
  DMA addresses, HDMI mode detection, and a mailbox that is busy but has not
  raised its publication interrupt.

## What these checks do not establish

Long-term capture stability, other units of the card, other HDMI modes than
1080p60, physical removal during capture, suspend and resume, and behaviour on
kernels older than 7.0 beyond compiling. See the README for the support limits.
