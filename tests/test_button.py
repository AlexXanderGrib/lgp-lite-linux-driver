"""Tests for userspace/lgplite-button.py."""
import importlib.util
import os
import unittest

_path = os.path.join(os.path.dirname(__file__), "..", "userspace", "lgplite-button.py")
_spec = importlib.util.spec_from_file_location("lgplite_button", _path)
button = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(button)


class EventTests(unittest.TestCase):
    def test_counts_presses_only(self):
        ev = button.INPUT_EVENT
        data = (ev.pack(5, 250000, 4, 4, 0)       # EV_MSC scancode
                + ev.pack(5, 250000, 1, 167, 1)   # KEY_RECORD press
                + ev.pack(5, 250000, 0, 0, 0)     # SYN
                + ev.pack(5, 250000, 1, 167, 0)   # release
                + ev.pack(7, 500000, 1, 148, 1))  # remapped key press
        self.assertEqual(button.count_presses(data), 2)
        self.assertEqual(button.count_presses(b""), 0)


class FakeLight(button.Light):
    """A Light whose control lives in memory instead of a video device."""

    def __init__(self, value=0):
        super().__init__("/dev/null")
        self.value = value
        self.writes = []

    def play(self, now):
        self.fd = -1  # pretend the device is open
        super().play(now)

    def _control(self, request, value=0):
        if request == button.VIDIOC_S_CTRL:
            self.value = value
            self.writes.append(value)
        return self.value

    def restore(self):
        self.fd = None
        super().restore()


class LightTests(unittest.TestCase):
    def test_click_spins_then_restores_previous_setting(self):
        light = FakeLight(value=2)  # e.g. Solid, chosen by the user
        light.play(10.0)
        self.assertEqual(light.value, button.LIGHT_FAST)
        self.assertAlmostEqual(light.timeout(10.5), button.LIGHT_SECONDS - 0.5)
        light.expire(10.0 + button.LIGHT_SECONDS)
        self.assertEqual(light.value, 2)
        self.assertIsNone(light.timeout(20.0))

    def test_click_during_spin_restarts_it_and_keeps_the_setting(self):
        light = FakeLight(value=0)
        light.play(10.0)
        light.play(10.5)
        self.assertEqual(light.writes, [button.LIGHT_FAST, button.LIGHT_FAST])
        light.expire(10.0 + button.LIGHT_SECONDS)  # the first spin's end
        self.assertEqual(light.value, button.LIGHT_FAST)
        light.expire(10.5 + button.LIGHT_SECONDS)
        self.assertEqual(light.value, 0)

    def test_restore_without_play_writes_nothing(self):
        light = FakeLight(value=1)
        light.restore()
        self.assertEqual(light.writes, [])


if __name__ == "__main__":
    unittest.main()
