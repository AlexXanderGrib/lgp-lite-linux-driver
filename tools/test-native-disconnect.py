#!/usr/bin/env python3
"""Hardware test: unbind/rebind only lgplite during capture with an open fd.

Run as root with webcam consumers closed. Requires ffmpeg and an active HDMI
source. This emulates driver disconnection; it does not power-cycle USB.
"""
import errno
import fcntl
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time


def main():
    if os.geteuid():
        raise SystemExit("Run with sudo: sudo python3 tools/test-native-disconnect.py")
    driver = Path("/sys/bus/usb/drivers/lgplite")
    interfaces = [p for p in driver.glob("*:*") if p.is_symlink()]
    if len(interfaces) != 1:
        raise SystemExit("Expected exactly one bound lgplite USB interface")
    interface = interfaces[0].resolve()
    usb = interface.parent
    assert (usb / "idVendor").read_text().strip() == "07ca"
    assert (usb / "idProduct").read_text().strip() == "c835"
    name = interface.name
    node = next((interface / "video4linux").glob("video*"))
    fd = os.open("/dev/" + node.name, os.O_RDWR | os.O_NONBLOCK)
    capture = None
    detached = False
    try:
        with tempfile.TemporaryDirectory(prefix="lgplite-disconnect-") as temp:
            progress = Path(temp) / "progress.txt"
            with open(Path(temp) / "ffmpeg.log", "w+") as log:
                capture = subprocess.Popen([
                    "ffmpeg", "-nostdin", "-v", "error", "-f", "v4l2",
                    "-input_format", "mjpeg", "-video_size", "640x480",
                    "-framerate", "30", "-i", "/dev/" + node.name,
                    "-progress", str(progress), "-f", "null", "-",
                ], stdout=subprocess.DEVNULL, stderr=log)
                deadline = time.monotonic() + 10
                while True:
                    text = progress.read_text() if progress.exists() else ""
                    frames = [int(v.partition("=")[2]) for v in text.splitlines()
                              if v.startswith("frame=")]
                    if frames and max(frames) >= 15:
                        break
                    if capture.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Capture did not produce 15 frames before disconnect")
                    time.sleep(0.05)
                print("Capture active; unbinding", name, flush=True)
                (driver / "unbind").write_text(name)
                detached = True
                capture.wait(timeout=10)
                print("Capture exited after disconnection:", capture.returncode, flush=True)
                try:
                    fcntl.ioctl(fd, 0x80685600, bytearray(104))  # VIDIOC_QUERYCAP
                except OSError as error:
                    assert error.errno == errno.ENODEV, error
                else:
                    raise AssertionError("Disconnected fd still accepts QUERYCAP")
                poll = select.poll()
                poll.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
                events = poll.poll(1000)
                assert events and events[0][1] & (select.POLLERR | select.POLLHUP)
                # Rebind while the old video fd still holds its device reference.
                (driver / "bind").write_text(name)
                detached = False
                new_node = next((interface / "video4linux").glob("video*"))
                print("Rebound as /dev/" + new_node.name, flush=True)
                with open("/dev/" + new_node.name, "rb", buffering=0) as new_fd:
                    cap = bytearray(104)
                    fcntl.ioctl(new_fd, 0x80685600, cap)
                    assert cap.split(b"\0", 1)[0] == b"lgplite"
                print("Old fd rejected; new node responds; closing old reference", flush=True)
                os.close(fd)
                fd = None
                result = subprocess.run([
                    "ffmpeg", "-nostdin", "-v", "error", "-f", "v4l2",
                    "-input_format", "mjpeg", "-video_size", "640x480",
                    "-framerate", "30", "-i", "/dev/" + new_node.name,
                    "-frames:v", "30", "-f", "null", "-",
                ], check=True, timeout=15, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, text=True)
                assert not result.stderr, result.stderr
                print("Capture after reconnect passed", flush=True)
    finally:
        if capture is not None and capture.poll() is None:
            capture.terminate()
            try:
                capture.wait(timeout=5)
            except subprocess.TimeoutExpired:
                capture.kill()
                capture.wait(timeout=5)
        if fd is not None:
            os.close(fd)
        if detached:
            (driver / "bind").write_text(name)


if __name__ == "__main__":
    main()
