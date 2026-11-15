# GL310 / C835 USB protocol

This documents the implemented subset of the AVerMedia Live Gamer Portable
Lite protocol. Hardware: USB 07ca:c835, vendor-specific interface 0, QPixel
QL201/PL330 family encoder (chip revision register 0x38 reads 0x00010020),
an HDMI receiver, and a board MCU. The HDMI receiver is managed by the MCU;
the host does not need a full receiver register initialization sequence.

## Sources and verification

The main reference is AVerMedia's April 2015 `AVer330USB.sys`, version
3.7.64.39, interpreted with function names/types aligned from the unstripped
PDB supplied with the installed Windows driver. The supplied PDB does not
exactly match the binary; mappings were checked against function sizes,
debug strings, and live hardware. The 2020 3.2802.x.40 package supplies the
same two firmware images.

The sibling C875 trace in
[Trouffman/octv_gears_lgp](https://github.com/Trouffman/octv_gears_lgp/blob/master/capture_sequence)
provided an independent cross-check for USB framing, boot order, and encoder
settings. That repository's abandoned kernel driver was not used as a working
driver. C875 board configuration must not be blindly replayed on a C835.

All register addresses below are **byte addresses**. DMA commands carry
**32-bit word addresses and word counts**. Register and command values are
little-endian. Stream byte order is a separate issue, described below.

## USB transport

| Endpoint | Direction | Purpose |
| --- | --- | --- |
| 0x04 | OUT | Commands |
| 0x83 | IN | Command replies |
| 0x02 | OUT | DMA payload upload |
| 0x81 | IN | DMA payload download |

Commands begin with `u8 opcode, u8 flag, u16 count, u32 address`.
Commands and their replies must be serialized. All transfers are bulk;
there is no UVC interface and no isochronous video endpoint.

| Operation | Command layout after packing little-endian | Reply |
| --- | --- | --- |
| Read registers | `01 00 count16 addr32` | `count * 4` bytes |
| Write register | `01 01 01 00 addr32 value32` | none |
| Read memory word | `02 00 04 00 wordaddr32 wordaddr32` | 4 bytes |
| Write memory word | `02 01 04 00 wordaddr32 wordaddr32 value32` | none |
| DMA write | `09 01 08 00 00000000 wordaddr32 words32` | 1 byte on 0x83, then upload on 0x02 |
| DMA read | `09 00 08 00 00000000 wordaddr32 words32` | 1 byte on 0x83, then read on 0x81 |
| ARM reset/run | `07 run8 00 00 00000000` | none |
| Software I2C write | `0b 01 count16 dev32 data...` | 1 status byte |
| Software I2C write/read | `0c write_count8 read_count16 dev32 write_data...` | read bytes, then status byte |

I2C status `0x08` means success. Hardware I2C uses opcodes 0x05 and 0x08
with the same layouts. The MCU uses **software I2C**, address **0x15**. The
INF encodes this as 8-bit address 0x2b; shift it right once for the USB command.
`MCU<ProjectC875>::ReadRegister` and `WriteRegister` also handle the C835.

Source functions: `CUsbCntl_RegisterReadEx`, `CUsbCntl_RegisterWrite`,
`CUsbCntl_MemoryRead`, `CUsbCntl_MemoryWrite`, `CUsbCntl_StartDMARead`,
`CUsbCntl_StartDMAWrite`, `CUsbCntl_SWI2CWriteThenRead`, `CUsbCntl_ResetArm`.
`USB_ResetArm` additionally clears register 0x6cc after reset/run.

## Firmware boot

Supported firmware is hash-checked before touching device RAM:

| File | Bytes | SHA-256 |
| --- | --- | --- |
| qpvidfwusb.bin | 454064 | `8da30f6dc2d4fee5aeb3e2e06df5581db4c01d146c50c21f6a5d4824025a47d1` |
| qpaudfwusb.bin | 363832 | `0e8ea66b6a3ddb68f9a283f10155df90ce8d6f46599ed0ecf6f308d6c90210f4` |

1. Stop ARM (`07 00 ...`) and clear register 0x6cc.
2. Set three host memory windows. For window `i=0..2`, write
   `0x81c + 12*i = 0x4000 + i*0x100000`,
   `0x820 + 12*i = 0x103fff + i*0x100000`,
   `0x824 + 12*i = i*0x100000`.
   Write register 0x840 with 0x70003124 and then 0x90003124.
3. Register 0x50: clear bit 1 (audio output enabled), set bit 2 (video output
   disabled). Write GPIO direction 0x610=0 and GPIO value 0x614=0. Wait 50 ms.
4. Clear bit 13 of register 0. USB boot ROM has already initialized SDRAM;
   `CQLCodec_InitializeMemory` is a no-op in this USB/direct Artesa configuration.
5. Zero-fill memory from `0x100000 + (audio_size & ~3)` through 0x170624,
   then upload audio to byte address 0x100000 in chunks of at most 0x8000 bytes.
   Read each chunk back and compare without byte swapping.
6. Write memory words 0x2f1094=1 (fixed mode), 0x2f1090=0 (interrupt mode),
   and `state_base+4=0`. Clear register 0x6cc.
   `state_base=0x2f2000` for this QSOS2 firmware. The API also defines 0x0f2000
   for QSOS3; that firmware version is not enabled by the current hash whitelist.
7. Upload video to byte address 0, with the same chunk/read-back procedure.
8. Run ARM (`07 01 ...`), clear 0x6cc, and wait 150 ms.
9. The header at memory 0x100 contains `QSOS` and a u16 version at 0x106.
   Verify ARM is actually running by sending a harmless mailbox command 0
   and waiting for the firmware to clear the mailbox busy bit. A readable
   uploaded header alone is insufficient evidence of execution.

Sources: `QPHCI_ReInit`, `CQLCodec_FWDownloadAll`, `CQLCodec_FWDownload`,
`CQLCodec_AOSwitch`, `CQLCodec_VOSwitch`, `CQLCodec_SetGPIODefaults`.
These are RAM operations; no flash updater protocol is implemented.

## HDMI status through the MCU

Before an MCU operation, GPIO input register 0x618 bit 13 indicates ready.
The Windows driver retries busy checks up to three times with 30 ms delays.
Poll MCU register 0x1b for 0 (stable), up to ten times, then wait 5 ms.

| MCU register | Meaning |
| --- | --- |
| 0x1c bit 2 | HDMI sync detected |
| 0x1c bit 4 | HDMI audio present |
| 0x1d..0x23 | Seven bytes of video timing |
| 0x24 low nibble | Audio rate: 0=44.1 kHz, 2=48 kHz, 3=32 kHz |
| 0x26 bits 0..1 | H/V sync polarities |
| 0x37 bit 1 | Compressed audio flag |

For timing bytes `b[0..6]`:

```
htotal = b[0] | ((b[1] & 0x0f) << 8)
width  = b[2] | ((b[1] & 0xf0) << 4)
vtotal = b[3] | ((b[4] & 0x0f) << 8)
height = b[5] | ((b[4] & 0xf0) << 4)
pixel_clock_kHz = 3456000 / b[6]
```

Match against known timing rows, with the original driver's tolerances
(horizontal total <16 pixels, vertical total <31 lines, clock error <6%).
The supported progressive modes and input-control words are in `HDMI_MODES`
in the implementation. Example from this GL310: `98 78 80 65 44 38 17`
decodes to 1920×1080, totals 2200×1125, approximately 150 MHz, matching 1080p60.

Sources: `InterfaceNUC100::getHdmiVideo_6604` (timing table at Ghidra address
0x000fa740 in the analyzed binary), `getHdmiAudio_6604`,
`MCU<ProjectC875>::IsCompressedAudio`, `MCU<ProjectC875>::bIsBusy`.

## Mailbox

Host → ARM:

- Poll register **0x6cc bit 0** until clear, with a bounded timeout.
- Write parameters to **0x6f8, 0x6f4, 0x6f0, 0x6ec, 0x6e8, 0x6e4**.
- Write **0x6cc = (message_status & 0xffff0000) | 1**.
- Write the command to **0x6fc** last. The firmware clears busy after consuming it.
- Commands include the task/channel in bits 16..31; capture uses task 0.
  For this USB/direct path, `SetInterrupt` is unsupported; the firmware polls.

ARM → host:

- Poll **0x800 bit 16** before reading a message. **0x6c8 bit 0 alone is not
  sufficient**: it can become set while parameters still contain the previous
  request. Reading at this point caused occasional duplicate data, nonmonotonic
  timestamps, and mixed address/length pairs on the real device.
- Read seven words from **0x6b0**: command, param1..param5, status.
- Clear the ARM interrupt flag through register 0x800 (write-one-to-clear bit
  16, preserving its other control bits). Ignore a stale IRQ if status bit 0 is clear.
- If status bit 8 requests an acknowledgment, send command 0x31 for encoder
  messages (command <0x80), otherwise 0xa2, preserving high command and status bits.
- Clear bit 0 of 0x6c8 after accepting the message.
- Accepting a notification and returning its data buffer are distinct actions.

Sources: `QPFWAPI_SendMessageToARM`, `QPFWAPI_GetARMMessage`,
`QPFWAPI_AckARMMessage`, `QPFWAPI_MailboxReady`, `DM_ClearInterrupt`, and
`CQLCodec_InterruptHandlerUSB` (the Windows driver gates message reads on IRQ).

## Encoder start

Open system command **0xf1**, param1=0x80000011 (H.264 + AAC encode).
Link command **0xf2**, param1=0x01000100 (VIU/AIU input, host outputs).
Miscellaneous command **0x10** selects its subcommand in param1; other
parameters follow in descending register order. The implemented setup uses
large compressed buffers (subcommand 0x14) and A/V discard control (0x16).
Software timestamps use subcommand 0x17, param2=1.
VIU sync codes use subcommand 2, parameters 0xf1f1f1da and 0xb6f1f1b6.

Write the encoder bank before sending start command **1**:

| Register | Setting / default |
| --- | --- |
| 0x6f8 | System control 0x2101c219: MPEG-TS H.264 video + audio |
| 0x6f4 | Input resolution: `(height << 16) | width` |
| 0x6f0 | Input control from HDMI timing table; output fps in bits 23..28 |
| 0x6ec | Rate control: 0x00780000 OR video kbit/s |
| 0x6e8 | VBR limits 0x1f4007d0 (VBR disabled) |
| 0x6e4 | Filter control 0x00002000 |
| 0x6e0 | GOP/loop filter 0xf1990000 OR output fps |
| 0x6d8 | Block transfer size 0x10 |
| 0x6dc | Output resolution (same as input) |
| 0x6d4 | Audio control 0x21161080 for AAC, stereo, 128 kbit/s, 48 kHz |
| 0x6d0 | AAC extension 0x520840f4 |

Audio rate bits 12..15 are 0=44.1 kHz, 1=48 kHz, 2=32 kHz. The output rate
is capped at 30 fps for 1080p. The firmware reports H.264 High, level 4.1 for
the tested 1080p30 configuration.

Sources: `CQLCodec_LoadDefaultSettings`, `CQLCodec_UpdateEncoderConfig`,
`CQLCodec_UpdateMiscConfig`, `QPFWCODECAPI_SystemOpen`, `SystemLink`,
`QPFWENCAPI_StartEncoder`, and the INF's `HKR,Encoder` defaults.

## Read and return stream buffers

Command **0x40** announces data; **0x41** announces the last buffer.
`CEncoderTask_ProcessArmMessage` identifies the fields:

| Field | Meaning for MPEG-TS |
| --- | --- |
| param1 low 16 bits | Buffer type **0x83** |
| param1 top byte | Firmware buffer token; preserve on completion |
| param2 | Buffer address, **in 32-bit words** |
| param3 | Frame flags, unused by this TS path |
| param4 | Buffer length, **in 32-bit words** |
| param5 low 31 bits | PTS divided by four (22.5 kHz); high bit is a validity flag |

Read `param4 * 4` bytes from `param2 * 4`, in DMA chunks. The encoder stores
stream bytes in reversed 32-bit words: raw `30 00 40 47` becomes `47 40 00 30`.
Swap every word in **stream data only**; do not swap firmware or command data.

**A firmware buffer can end in the middle of a 188-byte TS packet.** Carry
the partial packet into the next buffer before checking sync bytes or writing
complete packets. Discard an incomplete final packet when capture stops.
This differs from the initial buffers observed on the device, which happened
to be packet-aligned.

Return the buffer through command **0x30**, preserving the original command's
high bits and status high bits. Parameters are:
`type, consumed_words, pts_low31, pts_valid, 0, firmware_token`.
Do this after the DMA read. Sources: `CTask_BuildIoBlock`,
`CTask_CompleteArm`, `CTask_ReturnArmBuffer`.

To stop, send command **2**, param1=0 (do not wait for GOP). Continue accepting
pending buffer messages and return them with consumed_words=0. Wait for command
**0x50**, param1 low 31 bits=2, which confirms encoder stop. Command **0x51**
reports an encoder error. Always use bounded stop and idle timeouts.

## Native MJPEG webcam output

The encoder can return complete JPEG frames instead of MPEG-TS. This allows
native V4L2 capture using `V4L2_PIX_FMT_MJPEG` without a software decoder.
After the ordinary firmware boot, send:

| Command | Parameters | Purpose |
| --- | --- | --- |
| 0xf1 | 0x80000002 | Open MJPEG video encoding without audio |
| 0xf2 | 0x00000100 | Route VIU input to host video output |
| 0x11 | 0, 0, 0 | Encoder mode defaults |
| 0x10 | 11, quality | JPEG quality index, 0 through 15 |
| 0x10 | 12, 0 | Default JPEG frame buffering |

Write the ordinary register bank with system control `0x6f8 = 0x2101c20e`.
`CTaskEncode::enableMJPEG` sets the low 12 bits to `0x20e` and the system
function's low byte to 2. Input resolution at `0x6f4` stays at the source's
dimensions. Output resolution at `0x6dc` requests hardware downscaling;
1920×1080 input was verified at 640×360, 960×540 and 1280×720 output. The
scaler stretches the whole picture to the requested size (640×480 output of a
16:9 input is distorted, not cropped). Then send start command 1.

Messages 0x40/0x41 still report type **0x83**. After reversing each 32-bit
word's bytes, JPEGs start with `ff d8` and end with `ff d9`, with possible
trailing alignment padding. **A firmware buffer can split a JPEG when the
circular stream buffer wraps.** Return each firmware buffer promptly with
command 0x30 and retain partial JPEG bytes across messages. Publish a V4L2
buffer only after finding EOI, excluding up to seven alignment bytes from
`bytesused`. Data that does not begin with SOI is discarded until the next
frame start. The mailbox IRQ publication rule also applies here.

MJPEG timestamps come from the first fragment's param5, with software PTS
enabled through command `0x10`, parameters `0x17, 1`. The numeric counter
advances even when the firmware leaves the validity bit clear in MJPEG mode.
Mailbox PTS uses **22,500 ticks per second**; the Windows driver's
`CTask_ProcessDataStreaming` shifts it left by two to obtain 90 kHz timestamps.
At 30 fps, hardware messages advanced by 750 ticks; at 15 fps, by 1,500 ticks.
Anchor this counter to the host monotonic clock once per capture and unwrap
its low 31 bits. Timestamping USB arrival instead produces jitter, especially
when a JPEG crosses a buffer boundary, and can cause duplicate DTS after
FFmpeg rounds timestamps to the output time base.

On a driver **unbind**, stop/join the capture worker and send the normal encoder
stop sequence while the USB interface is still usable. Marking the device
disconnected before joining can abandon a command/reply or DMA transfer and
leave the board unresponsive until USB is physically reconnected. Physical
USB removal is different: the USB core has already marked the device absent.
Set `usb_driver.soft_unbind = 1` so the USB core keeps endpoints enabled until
the driver's disconnect callback completes. Before starting a new session,
drain pending command replies and DMA payloads with short, bounded IN transfers.

Sources: `CTaskEncode::enableMJPEG`, `QPFWENCAPI_SetMJPEGQuality`,
`QPFWENCAPI_SetMJPEGFrameBuffer`, and direct hardware captures of valid 1080p
and 640×480 JPEG frames.

## Remaining work

Additional timing modes, automatic input-change recovery, ALSA audio, and
testing on more devices remain open. The proprietary files and analyzed
Windows binaries are excluded from the repository.
