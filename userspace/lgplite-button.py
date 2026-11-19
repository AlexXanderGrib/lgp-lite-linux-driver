#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a command when the Live Gamer Portable Lite button is clicked.

The lgplite kernel module reports the button as a key on the input device
"AVerMedia GL310 Button". This tool waits for that device and runs a shell
command for each click; without one it prints the clicks. The light around
the button answers each click with two fast turns, then returns to its
previous pattern. The tool keeps running across reconnects.
"""
import argparse
import errno
import fcntl
import glob
import os
import select
import signal
import struct
import subprocess
import sys
import time

DEVICE_NAME = "AVerMedia GL310 Button"
INPUT_EVENT = struct.Struct("llHHi")  # struct input_event
EV_KEY = 1
RESCAN_SECONDS = 2

# The driver's "Button Light" control on the card's video device.
V4L2_CONTROL = struct.Struct("Ii")  # struct v4l2_control
VIDIOC_G_CTRL = 0xC008561B
VIDIOC_S_CTRL = 0xC008561C
LIGHT_CONTROL = 0x00982800
LIGHT_FAST = 4  # "Fast Clockwise"
LIGHT_SECONDS = 0.9  # about two turns


class Light:
    """Plays the fast pattern on the light ring, then restores the setting.

    The video device is open only while the pattern plays: an open descriptor
    would keep the lgplite module from being unloaded.
    """

    def __init__(self, path):
        self.path = path
        self.fd = None
        self.saved = None
        self.until = None

    def _control(self, request, value=0):
        data = bytearray(V4L2_CONTROL.pack(LIGHT_CONTROL, value))
        fcntl.ioctl(self.fd, request, data)
        return V4L2_CONTROL.unpack(data)[1]

    def play(self, now):
        if self.fd is None:
            self.fd = os.open(self.path, os.O_RDWR)
        if self.saved is None:
            self.saved = self._control(VIDIOC_G_CTRL)
        # Setting the same pattern again restarts it.
        self._control(VIDIOC_S_CTRL, LIGHT_FAST)
        self.until = now + LIGHT_SECONDS

    def timeout(self, now):
        return None if self.until is None else max(0.0, self.until - now)

    def expire(self, now):
        if self.until is not None and now >= self.until:
            self.restore()

    def restore(self):
        try:
            if self.saved is not None:
                self._control(VIDIOC_S_CTRL, self.saved)
        finally:
            self.saved = self.until = None
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None


def find_light(event_path):
    """Returns the Light of the card whose button is `event_path`, or None."""
    name = os.path.basename(event_path)
    card = os.path.realpath("/sys/class/input/%s/device/device" % name)
    for node in glob.glob("/sys/class/video4linux/video*"):
        if os.path.realpath(node + "/device") == card:
            return Light("/dev/" + os.path.basename(node))
    return None


def find_device():
    for path in sorted(glob.glob("/sys/class/input/event*/device/name")):
        try:
            with open(path) as f:
                if f.read().strip() == DEVICE_NAME:
                    return "/dev/input/" + path.split("/")[4]
        except OSError:
            continue
    return None


def count_presses(data):
    """Counts the key presses in raw input events."""
    presses = 0
    for off in range(0, len(data) - INPUT_EVENT.size + 1, INPUT_EVENT.size):
        _, _, kind, _, value = INPUT_EVENT.unpack_from(data, off)
        presses += kind == EV_KEY and value == 1
    return presses


def run(command):
    if command is None:
        print("click", flush=True)
        return
    # The outer shell exits at once, so no child is left to reap and the
    # command keeps running if this tool stops.
    try:
        subprocess.run(["/bin/sh", "-c", '(eval "$1") &', "lgplite-button", command],
                       start_new_session=True, check=False)
    except OSError as e:
        print("lgplite-button: cannot run %r: %s" % (command, e), file=sys.stderr)


def light_up(light):
    """Plays the light; returns it, or None once it has failed."""
    try:
        light.play(time.monotonic())
        return light
    except OSError as e:
        print("lgplite-button: cannot set the light on %s: %s" % (light.path, e),
              file=sys.stderr, flush=True)
        try:
            light.restore()
        except OSError:
            pass
        return None


def watch(path, command, light):
    """Handles clicks until the device goes away."""
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        print("lgplite-button: listening on " + path, file=sys.stderr, flush=True)
        while True:
            ready, _, _ = select.select([fd], [], [], light and light.timeout(time.monotonic()))
            if light:
                try:
                    light.expire(time.monotonic())
                except OSError:
                    light = None
            if not ready:
                continue
            for _ in range(count_presses(os.read(fd, INPUT_EVENT.size * 64))):
                if light:
                    light = light_up(light)
                run(command)
    except OSError as e:
        if e.errno != errno.ENODEV:
            raise
    finally:
        if light:
            try:
                light.restore()
            except OSError:
                pass
        os.close(fd)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--click", metavar="COMMAND",
                   help="shell command to run for each click")
    p.add_argument("--no-light", action="store_true",
                   help="leave the light around the button alone")
    args = p.parse_args()
    # Stopping the service while the light plays still restores it.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    message = None
    while True:
        path = find_device()
        new = None
        try:
            if path is None:
                new = ("waiting for the %s input device; is the card connected "
                       "and the lgplite module loaded?" % DEVICE_NAME)
            else:
                message = None
                light = None if args.no_light else find_light(path)
                watch(path, args.click, light)
        except PermissionError:
            # Also seen briefly after a reconnect, before udev sets access.
            new = ("no permission to read %s; install udev/70-avermedia-lgp-lite.rules "
                   "(see README)" % path)
        except OSError as e:
            # The device went away while it was being opened.
            if e.errno not in (errno.ENOENT, errno.ENODEV):
                raise
        if new and new != message:
            print("lgplite-button: " + new, file=sys.stderr, flush=True)
            message = new
        time.sleep(RESCAN_SECONDS)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
