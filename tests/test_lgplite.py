"""Regression tests for timing detection and firmware stream boundaries."""
import io
import random
import unittest
from unittest.mock import Mock

from userspace.lgplite import (
    Device, DeviceError, TransportStream, decode_signal, encoder_registers, swap_words,
)


class SignalTests(unittest.TestCase):
    def signal(self, **changes):
        # Actual MCU response from the connected GL310, 1080p60 / PCM 48 kHz.
        info = decode_signal(0xB7, bytes.fromhex("98 78 80 65 44 38 17"), 3, 2)
        info.update(changes)
        return info

    def test_live_1080p60_detection_and_30fps_limit(self):
        info = self.signal()
        self.assertEqual((info["width"], info["height"], info["fps"], info["sample_rate"]),
                         (1920, 1080, 60, 48000))
        regs, fps = encoder_registers(info)
        self.assertEqual(fps, 30)
        self.assertEqual(regs[0x6F4], 0x04380780)
        self.assertEqual(regs[0x6F0], 0x0F7C0609)
        self.assertEqual(regs[0x6EC] & 0xFFFF, 8000)
        with self.assertRaisesRegex(DeviceError, "between 1 and 30"):
            encoder_registers(info, fps=60)

    def test_720p60_keeps_full_frame_rate(self):
        info = decode_signal(4, bytes.fromhex("72 56 00 ee 22 d0 2e"), 3, 2)
        self.assertEqual(info["fps"], 60)
        self.assertEqual(encoder_registers(info)[1], 60)

    def test_1080p50_defaults_to_30_output(self):
        info = decode_signal(4, bytes.fromhex("50 7a 80 65 44 38 17"), 3, 2)
        self.assertEqual(info["fps"], 50)
        self.assertEqual(encoder_registers(info)[1], 30)

    def test_reject_unlocked_or_unknown_input(self):
        for info in (self.signal(locked=False), self.signal(input_control=None)):
            with self.subTest(info=info), self.assertRaises(DeviceError):
                encoder_registers(info)

    def test_audio_clock_fields(self):
        for rate, field in ((44100, 0), (48000, 1), (32000, 2)):
            with self.subTest(rate=rate):
                regs, _ = encoder_registers(self.signal(sample_rate=rate))
                self.assertEqual((regs[0x6D4] >> 12) & 15, field)
        with self.assertRaisesRegex(DeviceError, "sample rate"):
            encoder_registers(self.signal(sample_rate=None))

    def test_reject_bitrate_outside_device_range(self):
        for rate in (-1, 999, 20001):
            with self.subTest(rate=rate), self.assertRaises(DeviceError):
                encoder_registers(self.signal(), bitrate=rate)


class StreamTests(unittest.TestCase):
    def packets(self, count):
        return b"".join(b"\x47\x1f\xff\x10" + bytes([i % 256]) * 184 for i in range(count))

    def test_word_order_matches_live_transport_stream_header(self):
        self.assertEqual(swap_words(bytes.fromhex("30 00 40 47")), bytes.fromhex("47 40 00 30"))

    def test_packets_split_across_arbitrary_dma_and_firmware_buffers(self):
        # Firmware boundaries can split a TS packet, even in the middle of its header.
        expected = self.packets(2000)
        wire = swap_words(expected)
        rng = random.Random(835)
        ts = TransportStream()
        out = io.BytesIO()
        offset = 0
        while offset < len(wire):
            length = rng.randrange(1, 8193) * 4
            out.write(ts.feed(swap_words(wire[offset:offset + length])))
            offset += length
        self.assertEqual(out.getvalue(), expected)
        self.assertEqual(ts.tail, b"")

    def test_partial_final_packet_is_held_back(self):
        ts = TransportStream()
        data = self.packets(3)
        self.assertEqual(ts.feed(data[:-4]), data[:376])
        self.assertEqual(ts.feed(data[-4:]), data[376:])

    def test_corrupt_packet_is_reported(self):
        ts = TransportStream()
        with self.assertRaisesRegex(DeviceError, "sync lost"):
            ts.feed(b"\0" + self.packets(1)[1:])

    def test_invalid_dma_address_is_rejected_before_usb_read(self):
        dev = Device.__new__(Device)
        dev.hci = Mock()
        with self.assertRaisesRegex(DeviceError, "invalid encoder buffer"):
            dev.read_buffer([0x40, 0x83, 0x1000000, 0, 47, 0, 1])
        dev.hci.dma_read.assert_not_called()

    def test_busy_mailbox_is_not_a_published_message(self):
        # Real firmware can set HOST_STATUS before publishing its new parameters.
        # It raises IRQ bit 16 only when the full message is ready to read.
        dev = Device.__new__(Device)
        dev.hci = Mock()
        dev.hci.reg_read.side_effect = lambda reg: {0x800: 0, 0x6C8: 1}[reg]
        self.assertIsNone(dev.receive())
        self.assertEqual([c.args[0] for c in dev.hci.reg_read.call_args_list], [0x800])
        dev.hci.reg_write.assert_not_called()

    def test_large_ts_buffer_spans_multiple_dma_reads(self):
        dev = Device.__new__(Device)
        dev.hci = Mock()
        dev.complete_buffer = Mock()
        packet = self.packets(400)
        wire = swap_words(packet)
        base = 0x62BB00 * 4
        dev.hci.dma_read.side_effect = lambda addr, size: wire[addr - base:addr - base + size]
        msg = [0x40, 0x83, base // 4, 0, len(packet) // 4, 0xAAAAAAAA, 1]
        self.assertEqual(dev.read_buffer(msg), packet)
        self.assertGreater(dev.hci.dma_read.call_count, 1)
        dev.complete_buffer.assert_called_once_with(msg, len(packet) // 4)

    def test_stop_timeout_does_not_leave_streaming_flag_set(self):
        dev = Device.__new__(Device)
        dev.streaming = True
        dev.send = Mock(side_effect=DeviceError("mailbox timeout"))
        with self.assertRaises(DeviceError):
            dev.stop_capture()
        self.assertFalse(dev.streaming)


if __name__ == "__main__":
    unittest.main()
