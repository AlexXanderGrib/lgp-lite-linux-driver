#!/usr/bin/env python3
"""Userspace driver for the AVerMedia Live Gamer Portable Lite (GL310, 07ca:c835).

Talks to the device through usbfs with nothing but the Python standard
library. See docs/PROTOCOL.md for the USB and encoder mailbox protocol.

The device is a QPixel QL201-family H.264 encoder SoC behind four bulk
endpoints.  It keeps no codec firmware in flash: the host uploads an audio and
a video image into RAM, starts the SoC's ARM core, and then drives the encoder
through a register mailbox.
"""
import argparse
import array
import contextlib
import ctypes
import errno
import fcntl
import glob
import hashlib
import os
import signal
import struct
import sys
import time

VID = 0x07CA
PIDS = (0xC835, 0xD835)

# usbfs ioctls (linux/usbdevice_fs.h)
USBDEVFS_CLAIMINTERFACE = 0x8004550F
USBDEVFS_RELEASEINTERFACE = 0x80045510

EP_CMD_OUT = 0x04    # commands to the device
EP_CMD_IN = 0x83     # command replies
EP_DATA_OUT = 0x02   # bulk payload to device memory ("DMA write")
EP_DATA_IN = 0x81    # bulk payload from device memory ("DMA read")

# Command opcodes understood by the SoC's USB gadget
OP_HCI_REG = 0x00
OP_REG = 0x01
OP_MEM = 0x02
OP_REG_MULTI = 0x03
OP_I2C_WRITE = 0x05
OP_RESET_ARM = 0x07
OP_I2C_READ = 0x08
OP_DMA = 0x09
OP_SWI2C_WRITE = 0x0B
OP_SWI2C_READ = 0x0C
OP_USB_SPEED = 0x14

I2C_STATUS_OK = 0x08

# SoC registers
REG_CTRL = 0x000          # bit 13 is cleared before firmware upload
REG_CHIP_VERSION = 0x038
REG_OUTPUT_SWITCH = 0x050  # bit 1: audio out disabled, bit 2: video out disabled
REG_GPIO_DIR = 0x610
REG_GPIO_VALUE = 0x614
REG_GPIO_INPUT = 0x618
REG_MAILBOX_STATUS = 0x6CC
REG_MEM_WINDOW = 0x81C     # 3 x (start, end, offset)
REG_MEM_WINDOW_CTRL = 0x840

MEM_PAGE_SIZE = 0x100000
DMA_CHUNK = 0x8000

# Firmware load addresses and the handshake words the driver sets before boot
AUDIO_FW_ADDR = 0x100000
AUDIO_FW_REGION = 0x70624   # region above the audio image is zero-filled up to here
VIDEO_FW_ADDR = 0x000000
MEM_FW_INT_MODE = 0x2F1090
MEM_FW_FIXED_MODE = 0x2F1094
MEM_QSOS_HEADER = 0x100     # "QSOS", u16, u16 version
QSOS2_STATE_BASE = 0x2F2000
QSOS3_STATE_BASE = 0x0F2000

VIDEO_FW_NAME = "qpvidfwusb.bin"
AUDIO_FW_NAME = "qpaudfwusb.bin"
FIRMWARE_SHA256 = {
    VIDEO_FW_NAME: "8da30f6dc2d4fee5aeb3e2e06df5581db4c01d146c50c21f6a5d4824025a47d1",
    AUDIO_FW_NAME: "0e8ea66b6a3ddb68f9a283f10155df90ce8d6f46599ed0ecf6f308d6c90210f4",
}

REG_HOST_MESSAGE = 0x6B0
REG_HOST_STATUS = 0x6C8
REG_ARM_PARAM1 = 0x6F8
REG_ARM_MESSAGE = 0x6FC
REG_INTERRUPT = 0x800
MCU_ADDR = 0x15  # The INF's USBMcuAddr 0x2b is an 8-bit I2C address.
TS_BUFFER_TYPE = 0x83
SOC_MEMORY_SIZE = 64 * 1024 * 1024

# width, height, horizontal total, vertical total, pixel clock kHz, fps,
# encoder input control. Recovered from the Windows driver's HDMI mode table.
# Only 1080p60 input / 1080p30 output has been tested on hardware so far.
HDMI_MODES = (
    (1920, 1080, 2750, 1125, 74000, 24, 0x0C580609),
    (1920, 1080, 2640, 1125, 74000, 25, 0x0CD90609),
    (1920, 1080, 2200, 1125, 74000, 30, 0x0F5E0609),
    (1920, 1080, 2640, 1125, 148000, 50, 0x0CF20609),
    (1920, 1080, 2200, 1125, 148500, 60, 0x0F7C0609),
    (1280, 720, 1650, 750, 74000, 60, 0x3E7C0609),
    (1280, 720, 1980, 750, 74000, 50, 0x39720609),
)


class DeviceError(Exception):
    pass


class _BulkTransfer(ctypes.Structure):
    _fields_ = [("ep", ctypes.c_uint), ("len", ctypes.c_uint),
                ("timeout", ctypes.c_uint), ("data", ctypes.c_void_p)]


USBDEVFS_BULK = 0xC0005502 | (ctypes.sizeof(_BulkTransfer) << 16)


def decode_signal(status, timing, polarity, audio):
    """Decode MCU registers 0x1c, 0x1d..0x23, 0x26 and 0x24."""
    ht = timing[0] | (timing[1] & 0x0F) << 8
    width = timing[2] | (timing[1] & 0xF0) << 4
    vt = timing[3] | (timing[4] & 0x0F) << 8
    height = timing[5] | (timing[4] & 0xF0) << 4
    clock = 3456000 // timing[6] if timing[6] else 0
    mode = next((m for m in HDMI_MODES
                 if m[0] == width and m[1] == height
                 and abs(m[2] - ht) < 16 and abs(m[3] - vt) < 31
                 and abs(m[4] - clock) * 100 < m[4] * 6
                 and polarity & 3 == 3), None)
    return dict(locked=bool(status & 4), width=width, height=height,
                htotal=ht, vtotal=vt, pixel_clock_khz=clock,
                fps=mode[5] if mode else None,
                input_control=mode[6] if mode else None,
                audio_present=bool(status & 0x10),
                sample_rate={0: 44100, 2: 48000, 3: 32000}.get(audio & 0xF))


def encoder_registers(info, bitrate=8000, fps=None):
    """Build the Artesa encoder register bank for an identified HDMI mode."""
    if not info["locked"]:
        raise DeviceError("no stable HDMI signal; turn on the HDMI source")
    if info["input_control"] is None:
        raise DeviceError("unsupported HDMI timing: %dx%d (totals %dx%d, clock %d kHz); "
                          "use 720p50/60 or 1080p24/25/30/50/60"
                          % (info["width"], info["height"], info["htotal"],
                             info["vtotal"], info["pixel_clock_khz"]))
    limit = min(info["fps"], 30 if info["width"] == 1920 else 60)
    fps = limit if fps is None else fps
    if not 1 <= fps <= limit:
        raise DeviceError("output frame rate must be between 1 and %d for this input" % limit)
    if not 1000 <= bitrate <= 20000:
        raise DeviceError("video bitrate must be between 1000 and 20000 kbit/s")
    if info["sample_rate"] is None:
        raise DeviceError("unsupported HDMI audio sample rate; use PCM at 32, 44.1 or 48 kHz")
    sample = {44100: 0, 48000: 1, 32000: 2}[info["sample_rate"]]
    resolution = info["height"] << 16 | info["width"]
    return {
        0x6F8: 0x2101C219,  # MPEG-TS, video + audio, H.264 High
        0x6F4: resolution,
        0x6F0: (info["input_control"] & ~(0x3F << 23)) | fps << 23,
        0x6EC: 0x00780000 | bitrate,
        0x6E8: 0x1F4007D0,  # VBR limits (CBR is selected above)
        0x6E4: 0x00002000,
        0x6E0: 0xF1990000 | fps,  # one-second GOP
        0x6D8: 0x10,
        0x6DC: resolution,
        0x6D4: (0x21161080 & ~0xF000) | sample << 12,
        0x6D0: 0x520840F4,
    }, fps


def swap_words(data):
    """The encoder stores transport-stream bytes reversed in each 32-bit word."""
    words = array.array("I", data)
    words.byteswap()
    return words.tobytes()


class TransportStream:
    """Keep packet boundaries across firmware buffers (which split packets)."""

    def __init__(self):
        self.tail = b""
        self.offset = 0

    def feed(self, data):
        data = self.tail + data
        end = len(data) - len(data) % 188
        if any(data[i] != 0x47 for i in range(0, end, 188)):
            raise DeviceError("MPEG-TS sync lost after %d bytes" % self.offset)
        self.tail = data[end:]
        self.offset += end
        return data[:end]


def find_device():
    """Return the usbfs node of the first supported device."""
    for path in sorted(glob.glob("/sys/bus/usb/devices/*")):
        try:
            with open(path + "/idVendor") as f:
                vid = int(f.read(), 16)
            with open(path + "/idProduct") as f:
                pid = int(f.read(), 16)
            if vid != VID or pid not in PIDS:
                continue
            with open(path + "/busnum") as f:
                bus = int(f.read())
            with open(path + "/devnum") as f:
                dev = int(f.read())
        except (OSError, ValueError):
            continue
        return "/dev/bus/usb/%03d/%03d" % (bus, dev)
    raise DeviceError("no Live Gamer Portable Lite (07ca:c835) found on USB")


class Usb:
    """Synchronous bulk transfers on interface 0 via usbfs."""

    def __init__(self, node=None):
        self.node = node or find_device()
        try:
            self.fd = os.open(self.node, os.O_RDWR)
        except PermissionError:
            raise DeviceError(
                "no permission to open %s; install udev/70-avermedia-lgp-lite.rules "
                "(see README) or run as root" % self.node)
        try:
            fcntl.ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", 0))
        except OSError as e:
            os.close(self.fd)
            if e.errno == errno.EBUSY:
                raise DeviceError("the device is in use by another program or a kernel driver")
            raise

    def close(self):
        if self.fd is not None:
            try:
                fcntl.ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", 0))
            except OSError:
                pass
            os.close(self.fd)
            self.fd = None

    def write(self, ep, data, timeout_ms=2000):
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        xfer = _BulkTransfer(ep, len(data), timeout_ms, ctypes.cast(buf, ctypes.c_void_p))
        n = fcntl.ioctl(self.fd, USBDEVFS_BULK, xfer)
        if n != len(data):
            raise DeviceError("short write on endpoint %#x: %d of %d bytes" % (ep, n, len(data)))

    def read(self, ep, length, timeout_ms=2000):
        buf = ctypes.create_string_buffer(length)
        xfer = _BulkTransfer(ep, length, timeout_ms, ctypes.cast(buf, ctypes.c_void_p))
        n = fcntl.ioctl(self.fd, USBDEVFS_BULK, xfer)
        return buf.raw[:n]


class Hci:
    """The SoC's host command interface: registers, memory, I2C, bulk memory transfers."""

    def __init__(self, usb):
        self.usb = usb

    def _cmd(self, out, reply_len=0):
        self.usb.write(EP_CMD_OUT, out)
        if not reply_len:
            return b""
        reply = self.usb.read(EP_CMD_IN, reply_len)
        if len(reply) != reply_len:
            raise DeviceError("command %#04x: expected %d reply bytes, got %d"
                              % (out[0], reply_len, len(reply)))
        return reply

    # -- registers ---------------------------------------------------------
    def reg_read(self, addr, count=1):
        reply = self._cmd(struct.pack("<BBHI", OP_REG, 0, count, addr), 4 * count)
        vals = struct.unpack("<%dI" % count, reply)
        return vals[0] if count == 1 else list(vals)

    def reg_write(self, addr, value):
        self._cmd(struct.pack("<BBHII", OP_REG, 1, 1, addr, value & 0xFFFFFFFF))

    # -- single words of SoC memory (byte addresses) --------------------------
    def mem_read(self, addr):
        word = addr >> 2
        return struct.unpack("<I", self._cmd(struct.pack("<BBHII", OP_MEM, 0, 4, word, word), 4))[0]

    def mem_write(self, addr, value):
        word = addr >> 2
        self._cmd(struct.pack("<BBHIII", OP_MEM, 1, 4, word, word, value & 0xFFFFFFFF))

    # -- bulk memory transfers ------------------------------------------------
    def dma_write(self, addr, data):
        """Write `data` (multiple of 4 bytes) to SoC memory at byte address `addr`."""
        if addr & 3 or len(data) & 3:
            raise ValueError("bulk transfers are word aligned")
        self._cmd(struct.pack("<BBHIII", OP_DMA, 1, 8, 0, addr >> 2, len(data) >> 2), 1)
        self.usb.write(EP_DATA_OUT, data, timeout_ms=5000)

    def dma_read(self, addr, length, timeout_ms=5000):
        if addr & 3 or length & 3:
            raise ValueError("bulk transfers are word aligned")
        self._cmd(struct.pack("<BBHIII", OP_DMA, 0, 8, 0, addr >> 2, length >> 2), 1)
        data = self.usb.read(EP_DATA_IN, length, timeout_ms=timeout_ms)
        if len(data) != length:
            raise DeviceError("bulk read at %#x: expected %d bytes, got %d" % (addr, length, len(data)))
        return data

    # -- I2C (hardware bus of the SoC, and the bit-banged "software" bus) ------
    def _i2c_write(self, op, dev, data):
        status = self._cmd(struct.pack("<BBHI", op, 1, len(data), dev) + bytes(data), 1)[0]
        if status != I2C_STATUS_OK:
            raise DeviceError("I2C write to %#04x failed, status %#04x" % (dev, status))

    def _i2c_read(self, op, dev, length, write=b""):
        out = struct.pack("<BBHI", op, len(write), length, dev) + bytes(write)
        reply = self._cmd(out, length + 1)
        if reply[length] != I2C_STATUS_OK:
            raise DeviceError("I2C read from %#04x failed, status %#04x" % (dev, reply[length]))
        return reply[:length]

    def i2c_write(self, dev, data):
        self._i2c_write(OP_I2C_WRITE, dev, data)

    def i2c_read(self, dev, length, write=b""):
        return self._i2c_read(OP_I2C_READ, dev, length, write)

    def swi2c_write(self, dev, data):
        self._i2c_write(OP_SWI2C_WRITE, dev, data)

    def swi2c_read(self, dev, length, write=b""):
        return self._i2c_read(OP_SWI2C_READ, dev, length, write)

    # -- ARM core ----------------------------------------------------------
    def reset_arm(self, run):
        self._cmd(struct.pack("<BBHI", OP_RESET_ARM, 1 if run else 0, 0, 0))
        self.reg_write(REG_MAILBOX_STATUS, 0)

    def usb_speed(self):
        return self._cmd(struct.pack("<BBHI", OP_USB_SPEED, 0, 1, 0), 1)[0]


def find_firmware(directory=None):
    """Locate the two firmware images; returns (video_path, audio_path)."""
    candidates = [directory] if directory else [
        os.environ.get("LGPLITE_FIRMWARE"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware"),
        os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")), "lgplite"),
        "/lib/firmware/avermedia",
    ]
    for d in candidates:
        if not d:
            continue
        vid, aud = os.path.join(d, VIDEO_FW_NAME), os.path.join(d, AUDIO_FW_NAME)
        if os.path.isfile(vid) and os.path.isfile(aud):
            return vid, aud
    raise DeviceError(
        "firmware not found (%s and %s). Run tools/get-firmware.sh first; looked in: %s"
        % (VIDEO_FW_NAME, AUDIO_FW_NAME, ", ".join(os.path.normpath(d) for d in candidates if d)))


def _pad4(data):
    return data + b"\0" * (-len(data) % 4)


class Device:
    def __init__(self, node=None, verbose=False):
        self.usb = Usb(node)
        self.hci = Hci(self.usb)
        self.verbose = verbose
        self.streaming = False

    def close(self):
        self.usb.close()

    def log(self, msg):
        if self.verbose:
            print(msg, file=sys.stderr, flush=True)

    # -- bring-up --------------------------------------------------------------
    def map_memory_windows(self):
        """Program the three 1 MiB host windows onto SoC memory (direct access mode)."""
        h = self.hci
        for i in range(3):
            offset = i * MEM_PAGE_SIZE
            start = offset + 0x4000
            h.reg_write(REG_MEM_WINDOW + i * 0xC, start)
            h.reg_write(REG_MEM_WINDOW + i * 0xC + 4, start - 1 + MEM_PAGE_SIZE)
            h.reg_write(REG_MEM_WINDOW + i * 0xC + 8, offset)
        h.reg_write(REG_MEM_WINDOW_CTRL, 0x70003124)
        h.reg_write(REG_MEM_WINDOW_CTRL, 0x90003124)

    def set_outputs(self, audio_out=True, video_out=False):
        h = self.hci
        v = h.reg_read(REG_OUTPUT_SWITCH)
        h.reg_write(REG_OUTPUT_SWITCH, v & ~2 if audio_out else v | 2)
        v = h.reg_read(REG_OUTPUT_SWITCH)
        h.reg_write(REG_OUTPUT_SWITCH, v & ~4 if video_out else v | 4)

    def _upload(self, addr, image, what, verify=True):
        image = _pad4(image)
        for off in range(0, len(image), DMA_CHUNK):
            chunk = image[off:off + DMA_CHUNK]
            self.hci.dma_write(addr + off, chunk)
            if verify:
                time.sleep(0.001)
                if self.hci.dma_read(addr + off, len(chunk)) != chunk:
                    raise DeviceError("%s firmware verify failed at offset %#x" % (what, off))
        self.log("%s firmware: %d bytes at %#x%s" % (what, len(image), addr, ", verified" if verify else ""))

    def boot(self, firmware_dir=None, verify=True):
        """Upload both firmware images and start the ARM core."""
        vid_path, aud_path = find_firmware(firmware_dir)
        with open(vid_path, "rb") as f:
            video = f.read()
        with open(aud_path, "rb") as f:
            audio = f.read()
        for path, data in ((vid_path, video), (aud_path, audio)):
            if hashlib.sha256(data).hexdigest() != FIRMWARE_SHA256[os.path.basename(path)]:
                raise DeviceError("unrecognized firmware: %s; use tools/get-firmware.sh" % path)
        if video[0x100:0x104] != b"QSOS":
            raise DeviceError("%s does not look like a QSOS firmware image" % vid_path)
        qsos_version = struct.unpack_from("<H", video, 0x106)[0]
        state_base = QSOS2_STATE_BASE if qsos_version < 3 else QSOS3_STATE_BASE

        h = self.hci
        h.reset_arm(False)
        self.map_memory_windows()
        self.set_outputs(audio_out=True, video_out=False)
        h.reg_write(REG_GPIO_DIR, 0)
        h.reg_write(REG_GPIO_VALUE, 0)
        time.sleep(0.05)
        h.reg_write(REG_CTRL, h.reg_read(REG_CTRL) & ~0x2000)

        # Audio image, with the rest of its region cleared first.
        aligned = len(audio) & ~3
        fill = max(AUDIO_FW_REGION - aligned, 0)
        zeros = bytes(DMA_CHUNK)
        off = aligned
        while fill:
            n = min(fill, DMA_CHUNK)
            h.dma_write(AUDIO_FW_ADDR + off, zeros[:n + (-n % 4)])
            off += n
            fill -= n
        self._upload(AUDIO_FW_ADDR, audio, "audio", verify)
        time.sleep(0.001)

        # Video image (the ARM program proper).
        h.mem_write(MEM_FW_FIXED_MODE, 1)
        h.mem_write(MEM_FW_INT_MODE, 0)
        h.mem_write(state_base + 4, 0)
        h.reg_write(REG_MAILBOX_STATUS, 0)
        self._upload(VIDEO_FW_ADDR, video, "video", verify)

        time.sleep(0.001)
        h.reset_arm(True)
        time.sleep(0.15)

        magic = struct.pack("<II", h.mem_read(MEM_QSOS_HEADER), h.mem_read(MEM_QSOS_HEADER + 4))
        if magic[:4] != b"QSOS":
            raise DeviceError("firmware header not readable after boot: %r" % magic)
        mode = h.mem_read(state_base + 0x10)
        # Reading the uploaded header alone cannot prove that ARM is running.
        self.send(0)  # GetEncoderVersion, consumed without a reply on this FW
        self.wait_mailbox()
        self.log("firmware running: QSOS version %d, USB default mode %#x" % (qsos_version, mode))
        return mode

    # -- firmware mailbox ----------------------------------------------------
    def wait_mailbox(self, timeout=1.0):
        deadline = time.monotonic() + timeout
        while self.hci.reg_read(REG_MAILBOX_STATUS) & 1:
            if time.monotonic() >= deadline:
                raise DeviceError("firmware mailbox timed out; re-run capture to reload firmware")
            time.sleep(0.001)

    def send(self, code, params=(), status=0):
        self.wait_mailbox()
        for i, value in enumerate(params):
            self.hci.reg_write(REG_ARM_PARAM1 - 4 * i, value)
        # High bits preserve the firmware message ID on acknowledgments.
        self.hci.reg_write(REG_MAILBOX_STATUS, (status & 0xFFFF0000) | 1)
        self.hci.reg_write(REG_ARM_MESSAGE, code)

    def receive(self):
        # Firmware sets mailbox busy before all parameters are published. The
        # interrupt is the publication barrier; polling only HOST_STATUS can
        # read a previous address with a new length and silently duplicate data.
        irq = self.hci.reg_read(REG_INTERRUPT)
        if not irq & 0x10000:
            return None
        msg = self.hci.reg_read(REG_HOST_MESSAGE, 7)
        code, *_, status = msg
        self.hci.reg_write(REG_INTERRUPT, (irq & ~0x70000) | 0x10000)
        if not status & 1:
            return None
        self.log("ARM message: " + " ".join("%08x" % v for v in msg))
        if status & 0x100:
            self.send((code & 0xFFFF0000) | (0x31 if code & 0xFFFF < 0x80 else 0xA2),
                      status=status)
        self.hci.reg_write(REG_HOST_STATUS, status & ~1)
        return msg

    def complete_buffer(self, msg, words):
        code, kind, _, _, _, pts, status = msg
        self.send((code & 0xFFFF0000) | 0x30,
                  (kind & 0xFFFF, words, pts & 0x7FFFFFFF,
                   (pts >> 31) & 1, 0, kind >> 24), status)

    def read_buffer(self, msg):
        code, kind, addr, _, words, _, _ = msg
        if code & 0xFFFF not in (0x40, 0x41):
            return b""
        if kind & 0xFFFF != TS_BUFFER_TYPE:
            self.complete_buffer(msg, 0)
            raise DeviceError("unexpected encoder buffer type %#x" % kind)
        length = words * 4
        addr *= 4
        if length > 4 * 1024 * 1024 or addr + length > SOC_MEMORY_SIZE:
            raise DeviceError("invalid encoder buffer: address %#x, length %d" % (addr, length))
        data = b"".join(self.hci.dma_read(addr + off, min(DMA_CHUNK, length - off))
                        for off in range(0, length, DMA_CHUNK))
        self.complete_buffer(msg, words)
        return swap_words(data)

    # -- HDMI input and capture ----------------------------------------------
    def mcu_read(self, reg, length=1):
        # GPIO13 is MCU ready (input, active high). Match the driver's retries.
        for _ in range(3):
            if self.hci.reg_read(REG_GPIO_INPUT) & (1 << 13):
                break
            time.sleep(0.03)
        return self.hci.swi2c_read(MCU_ADDR, length, bytes([reg]))

    def signal_info(self):
        for _ in range(10):
            if self.mcu_read(0x1B)[0] == 0:
                break
            time.sleep(0.03)
        else:
            raise DeviceError("HDMI signal did not stabilize")
        time.sleep(0.005)
        status = self.mcu_read(0x1C)[0]
        timing = self.mcu_read(0x1D, 7)
        polarity = self.mcu_read(0x26)[0]
        audio = self.mcu_read(0x24)[0]
        # MCU register 0x37 bit 1 marks compressed audio, unsupported here.
        if status & 0x10 and self.mcu_read(0x37)[0] & 2:
            raise DeviceError("compressed HDMI audio is unsupported; set the source to stereo PCM")
        return decode_signal(status, timing, polarity, audio)

    def start_capture(self, info, bitrate=8000, fps=None):
        regs, fps = encoder_registers(info, bitrate, fps)
        self.send(0xF1, (0x80000011,))  # system open: encode H.264 + AAC
        self.send(0xF2, (0x01000100,))  # VIU/AIU inputs, host video/audio outputs
        self.send(0x10, (0x14, 1, 19000))  # large compressed buffer
        self.send(0x10, (0x16, 1, 1))  # AV discard control, preserve sync
        self.send(0x10, (0x17, 1))  # software PTS
        self.send(0x10, (2, 0xF1F1F1DA, 0xB6F1F1B6))  # VIU sync codes
        self.wait_mailbox()
        for addr, value in regs.items():
            self.hci.reg_write(addr, value)
        self.streaming = True
        self.send(1)
        return fps

    def stop_capture(self):
        if not self.streaming:
            return
        try:
            self.send(2, (0,))
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                msg = self.receive()
                if msg is None:
                    time.sleep(0.001)
                    continue
                code, p1, *_ = msg
                if code & 0xFFFF in (0x40, 0x41):
                    self.complete_buffer(msg, 0)
                elif code & 0xFFFF == 0x50 and p1 & 0x7FFFFFFF == 2:
                    self.wait_mailbox()
                    return
            raise DeviceError("encoder did not confirm stop within 2 seconds")
        finally:
            self.streaming = False

    def capture(self, output, seconds=None, idle_timeout=5.0, stop_requested=lambda: False):
        started = last_data = time.monotonic()
        size = blocks = 0
        ts = TransportStream()
        while not stop_requested() and (seconds is None or time.monotonic() - started < seconds):
            msg = self.receive()
            if msg is not None:
                if msg[0] & 0xFFFF == 0x51:
                    raise DeviceError("encoder reported error %#x" % msg[1])
                data = self.read_buffer(msg)
                if data:
                    data = ts.feed(data)
                    output.write(data)
                    size += len(data)
                    blocks += 1
                    last_data = time.monotonic()
            else:
                time.sleep(0.001)
            if time.monotonic() - last_data > idle_timeout:
                raise DeviceError("no video data for %.1f seconds; check the HDMI source" % idle_timeout)
        if not size and not stop_requested():
            raise DeviceError("capture ended without receiving any video data")
        return size, blocks, time.monotonic() - started

    def info(self):
        h = self.hci
        return {
            "node": self.usb.node,
            "chip_version": h.reg_read(REG_CHIP_VERSION),
            "ctrl": h.reg_read(REG_CTRL),
            "output_switch": h.reg_read(REG_OUTPUT_SWITCH),
            "gpio_dir": h.reg_read(REG_GPIO_DIR),
            "gpio_value": h.reg_read(REG_GPIO_VALUE),
            "gpio_input": h.reg_read(REG_GPIO_INPUT),
            "mailbox_status": h.reg_read(REG_MAILBOX_STATUS),
        }


def cmd_info(dev, args):
    for key, val in dev.info().items():
        print("%-16s %s" % (key, val if isinstance(val, str) else "0x%08x" % val))


def cmd_boot(dev, args):
    t = time.monotonic()
    dev.boot(args.firmware, verify=not args.no_verify)
    print("firmware uploaded and started in %.1f s" % (time.monotonic() - t))


def format_signal(info):
    audio = "%s Hz HDMI audio" % info["sample_rate"] if info["audio_present"] else "no HDMI audio"
    return "%dx%d, %s fps input, %s" % (
        info["width"], info["height"], info["fps"] or "unknown",
        audio)


def cmd_signal(dev, args):
    info = dev.signal_info()
    print(("locked: " if info["locked"] else "no signal: ") + format_signal(info))


def cmd_capture(dev, args):
    # Validate settings before resetting the device or creating an output file.
    info = dev.signal_info()
    _, fps = encoder_registers(info, args.bitrate, args.fps)
    stopped = False

    def request_stop(signum, frame):
        nonlocal stopped
        stopped = True

    handlers = {s: signal.signal(s, request_stop) for s in (signal.SIGINT, signal.SIGTERM)}
    try:
        # Exclusive creation avoids accidentally destroying an existing recording.
        output = contextlib.nullcontext(sys.stdout.buffer) if args.output == "-" else open(args.output, "xb")
        with output as stream:
            print("HDMI: %s; recording at %d fps, %d kbit/s" %
                  (format_signal(info), fps, args.bitrate), file=sys.stderr)
            dev.boot(args.firmware)
            try:
                dev.start_capture(info, args.bitrate, args.fps)
                size, blocks, elapsed = dev.capture(stream, args.seconds, args.timeout, lambda: stopped)
                stream.flush()
            finally:
                # Preserve the original error if e.g. unplugging also breaks stop.
                if sys.exc_info()[0] is not None:
                    try:
                        dev.stop_capture()
                    except (DeviceError, OSError) as e:
                        print("lgplite: cleanup: %s" % e, file=sys.stderr)
                else:
                    dev.stop_capture()
        print("Captured %d bytes in %.1f s (%d buffers)" % (size, elapsed, blocks), file=sys.stderr)
    finally:
        for s, handler in handlers.items():
            signal.signal(s, handler)


def cmd_reg(dev, args):
    addr = int(args.addr, 0)
    if args.value is not None:
        dev.hci.reg_write(addr, int(args.value, 0))
    else:
        for i, v in enumerate(dev.hci.reg_read(addr, args.count) if args.count > 1
                              else [dev.hci.reg_read(addr)]):
            print("0x%04x: 0x%08x" % (addr + 4 * i, v))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="show basic device registers").set_defaults(fn=cmd_info)
    sub.add_parser("signal", help="show detected HDMI video and audio").set_defaults(fn=cmd_signal)

    b = sub.add_parser("boot", help="upload firmware and start the SoC")
    b.add_argument("--firmware", metavar="DIR", help="directory with the two .bin files")
    b.add_argument("--no-verify", action="store_true", help="skip read-back verification")
    b.set_defaults(fn=cmd_boot)

    c = sub.add_parser("capture", help="record an MPEG-TS stream (H.264 + AAC)")
    c.add_argument("output", help="new .ts file, or - for stdout; existing files are preserved")
    c.add_argument("--firmware", metavar="DIR", help="directory with the two .bin files")
    c.add_argument("--seconds", type=float, help="record for this many seconds; default: until Ctrl+C")
    c.add_argument("--bitrate", type=int, default=8000, help="video kbit/s, 1000..20000 (default: 8000)")
    c.add_argument("--fps", type=int, help="output fps; default: input rate, capped at 30 for 1080p")
    c.add_argument("--timeout", type=float, default=5.0, help="seconds without video before failing (default: 5)")
    c.set_defaults(fn=cmd_capture)

    r = sub.add_parser("reg", help="read or write a SoC register (debugging)")
    r.add_argument("addr")
    r.add_argument("value", nargs="?")
    r.add_argument("-n", "--count", type=int, default=1)
    r.set_defaults(fn=cmd_reg)

    args = p.parse_args()
    if args.cmd == "capture":
        if args.seconds is not None and (not 0 < args.seconds < float("inf")):
            p.error("--seconds must be a finite positive number")
        if not 0 < args.timeout < float("inf"):
            p.error("--timeout must be a finite positive number")
        if args.fps is not None and args.fps <= 0:
            p.error("--fps must be positive")
        if not 1000 <= args.bitrate <= 20000:
            p.error("--bitrate must be between 1000 and 20000 kbit/s")
    if args.cmd == "reg" and not 1 <= args.count <= 32:
        p.error("--count must be between 1 and 32")
    dev = None
    try:
        dev = Device(verbose=args.verbose)
        args.fn(dev, args)
    except DeviceError as e:
        sys.exit("lgplite: %s" % e)
    except BrokenPipeError:
        # A player closing its stdin is a normal end to a live preview.
        with open(os.devnull, "wb") as sink:
            os.dup2(sink.fileno(), sys.stdout.fileno())
    except OSError as e:
        sys.exit("lgplite: %s" % e)
    finally:
        if dev is not None:
            dev.close()


if __name__ == "__main__":
    main()
