# Hardware validation

Tested on 2026-11-15 with a GL310 / C835, USB 07ca:c835, chip revision
0x00010020. HDMI input was another PC producing progressive 1920×1080 at
60 Hz with stereo PCM audio at 48 kHz.

## Verified recording

Command:

```sh
python3 userspace/lgplite.py capture captures/verified-1080p30.ts --seconds 45
```

The local sample is ignored by Git, along with firmware and other recordings.

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

The wall-clock capture limit includes encoder startup latency, so the media
duration is slightly shorter than the requested limit.

## Other checks

- Both firmware images passed read-back comparison after every RAM upload.
- A second recording at 12,000 kbit/s captured 6,057,172 bytes in four seconds.
- Repeated captures reload firmware, stop normally, and release the USB interface.
- Closing a stdout consumer early stops the encoder and exits without a traceback.
- SIGINT and SIGTERM stop capture and release the device.
- Attempting to record over an existing file fails while preserving its size
  and modification time.
- Invalid durations, timeouts, frame rates, and bitrates fail before USB access.
- Fourteen regression tests pass, including split TS packets, DMA chunking,
  invalid DMA addresses, HDMI mode detection, and ignoring a mailbox that is
  busy but has not raised its publication interrupt.
- Python syntax, shell syntax, and `git diff --check` pass.

The Windows system partition used to locate the proprietary firmware was
unmounted after extraction. No firmware blobs or Windows binaries are included
in the source tree tracked by Git.

## Limits of these checks

This does not establish long-term capture stability, support for other devices,
or correct behavior for every timing row. Physical unplug during capture,
fractional frame rates, interlaced input, and the optional v4l2loopback bridge
were not tested. See the README for the current support limits.

## Native V4L2 webcam, 2026-11-16

Tested on the same GL310 with 1080p60 HDMI input, running Linux
7.0.0-43-generic. DKMS signs `lgplite.ko` with the machine's already-enrolled
MOK, and the module loads under Secure Boot. Its USB alias loads it automatically
on boot and reconnection. Native video requires no userspace bridge.

| Check | Result |
| --- | --- |
| Basic V4L2 compliance | 48 passed, zero failures or warnings |
| Streaming and all advertised formats | 63 total checks passed, zero failures or warnings |
| 640×480 MJPEG at 30 fps | 120 frames, dimensions/count checked, every frame decoded |
| 1280×720 MJPEG at 30 fps | 120 frames, dimensions/count checked, every frame decoded |
| 1920×1080 MJPEG at 30 fps | 120 frames, dimensions/count checked, every frame decoded |
| Native FFmpeg 1080p30 run | 900 frames in 30 seconds; zero duplicates/drops; empty error log |
| Lower frame rate | 640×480 at 15 fps captures and decodes without timestamp errors |
| Driver removal during capture | Capture exits; old open fd returns ENODEV and signals error/hangup |
| Rebind with the old fd open | New node registers and responds; closing the old fd is safe |
| Capture after rebind | Starts again and decodes 30 frames without errors |
| Module build | Linux 6.17.0-41 and 7.0.0-43, `W=1`, no C compiler warnings |

The capture/decode checks are reproducible with `bash tools/validate-native.sh`.
Final size/rate samples and logs are under `captures/native-rz2bD9/`; the longer
run's progress/error logs and successful disconnect test are under
`captures/native-lyrNgZ/`. These local artifacts are ignored by Git.

Streaming compliance includes read(), mmap, select, epoll, REQBUFS, and
CREATE_BUFS. DMA-BUF export is checked, but streaming import was not tested
against a separate allocator. USERPTR is not advertised. The explicit
disconnect/rebind check is `sudo python3 tools/test-native-disconnect.py`;
it targets only the single verified GL310 and requires other camera consumers
to be closed. This emulates driver unbinding rather than cutting USB power.

### Corrections established by hardware tests

- Controls retain a separate mutex from the video ioctl/queue mutex. The
  original recursive-lock bug was corrected before this validation run.
- Firmware messages split JPEGs when the circular stream buffer wraps.
  Partial frames are retained across messages and published only after EOI.
- Mailbox PTS is in 22.5 kHz units. Using this counter instead of USB receipt
  times eliminates the duplicate DTS warnings observed in a native FFmpeg run.
- `soft_unbind` keeps USB endpoints usable until the driver joins its worker
  and stops the encoder. Disabling transfers too early left the board needing
  a physical USB reconnect; the final unbind/rebind/capture test passes.
- 24/25 fps output produced inconsistent firmware timestamps and is omitted
  from native enumeration. The native driver advertises 15 and 30 fps.

Physical reconnects during development also exercised automatic module binding.
Long-term stability, physical cable removal in the middle of a capture, other
HDMI source timings, and application-specific integration still need broader
testing. No ALSA microphone interface is implemented.
