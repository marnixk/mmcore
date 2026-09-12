"""QEMU test harness for the raspberrypi-mmbasic bare-metal console.

The harness boots a Circle-based ``kernel8.img`` under ``qemu-system-aarch64``
(emulating a Raspberry Pi 3) and drives it the way a real user would:

* it injects keystrokes over the emulated PL011 serial console,
* it reads the console's serial output back (exact, deterministic),
* it captures the emulated HDMI framebuffer via the QEMU monitor and reads the
  on-screen text with OCR.

This is the foundation for a large regression suite: once the MMBasic core
from ``picomite-fork`` is ported onto Circle, each language feature can be
verified by typing a program and asserting on the console output.

QEMU ``raspi3b`` has no onboard NIC. Optional ``usb-net`` (SLIRP user
networking) attaches Circle's USB CDC Ethernet driver; see
``qemu_usb_net_args()``. Default tests omit it so USB enumerate/DHCP does
not slow the suite.
"""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import tempfile
import time


class HarnessError(RuntimeError):
    pass


def qemu_usb_net_args(hostfwd: str | None = None) -> list[str]:
    """QEMU flags for Circle USB CDC Ethernet (``-device usb-net``).

    ``hostfwd`` is a SLIRP rule such as ``tcp::8080-:80`` (host→guest).
    Guest→host uses ``10.0.2.2``. DHCP typically assigns ``10.0.2.15``.
    """
    netdev = "user,id=net0"
    if hostfwd:
        netdev += f",hostfwd={hostfwd}"
    return ["-netdev", netdev, "-device", "usb-net,netdev=net0"]


class MMBasicConsole:
    """Controls one QEMU instance running the bare-metal console."""

    def __init__(
        self,
        kernel: str,
        machine: str = "raspi3b",
        qemu: str = "qemu-system-aarch64",
        boot_timeout: float = 25.0,
        ready_marker: bytes = b"get started",
        prompt: bytes = b"> ",
        extra_qemu: list[str] | None = None,
    ) -> None:
        if not os.path.isfile(kernel):
            raise HarnessError(f"kernel image not found: {kernel}")
        self.kernel = os.path.abspath(kernel)
        self.machine = machine
        self.qemu = qemu
        self.boot_timeout = boot_timeout
        self.ready_marker = ready_marker
        self.prompt = prompt
        self.extra_qemu = extra_qemu or []
        self.boot_log = b""

        self._tmp = tempfile.mkdtemp(prefix="mmb-harness-")
        self._ser_path = os.path.join(self._tmp, "serial.sock")
        self._mon_path = os.path.join(self._tmp, "monitor.sock")
        self._proc: subprocess.Popen | None = None
        self._ser: socket.socket | None = None
        self._mon: socket.socket | None = None

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> "MMBasicConsole":
        cmd = [
            self.qemu,
            "-M", self.machine,
            "-kernel", self.kernel,
            "-display", "none",
            "-serial", f"unix:{self._ser_path},server,nowait",
            "-monitor", f"unix:{self._mon_path},server,nowait",
        ]
        cmd.extend(self.extra_qemu)
        self._proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        self._ser = self._connect(self._ser_path)
        self._mon = self._connect(self._mon_path)
        self._ser.settimeout(0.4)

        # wait for the firmware to announce it is ready, then the prompt
        deadline = time.time() + self.boot_timeout
        seen = b""
        got_marker = False
        while time.time() < deadline:
            chunk = self._recv(self._ser)
            if chunk:
                seen += chunk
                if self.ready_marker in seen:
                    got_marker = True
                if got_marker and self.prompt in seen:
                    self.boot_log = seen
                    return self
            else:
                time.sleep(0.05)
        self.stop()
        raise HarnessError(
            f"console did not become ready within {self.boot_timeout}s; "
            f"saw: {seen!r}"
        )

    def _connect(self, path: str) -> socket.socket:
        deadline = time.time() + 10
        while time.time() < deadline:
            if os.path.exists(path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(path)
                    return s
                except OSError:
                    pass
            time.sleep(0.05)
        raise HarnessError(f"timed out connecting to {path}")

    def stop(self) -> None:
        try:
            if self._mon is not None:
                try:
                    self._mon.sendall(b"quit\n")
                    time.sleep(0.2)
                except OSError:
                    pass
                self._mon.close()
        finally:
            self._mon = None
        if self._ser is not None:
            self._ser.close()
            self._ser = None
        if self._proc is not None:
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None
        shutil.rmtree(self._tmp, ignore_errors=True)

    def __enter__(self) -> "MMBasicConsole":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # -- serial I/O --------------------------------------------------------
    @staticmethod
    def _recv(sock: socket.socket) -> bytes:
        try:
            return sock.recv(4096)
        except socket.timeout:
            return b""
        except OSError:
            return b""

    def drain(self, quiet: float = 0.2, timeout: float = 12.0) -> bytes:
        """Read until the line is quiet, or until timeout if output never stops."""
        assert self._ser is not None
        buf = b""
        last = time.time()
        start = last
        while time.time() - last < quiet:
            if time.time() - start >= timeout:
                break
            chunk = self._recv(self._ser)
            if chunk:
                buf += chunk
                last = time.time()
        return buf

    def send_line(self, text: str, timeout: float = 3.0) -> str:
        """Type ``text`` + Enter and return the response text (between the
        echoed command and the next prompt)."""
        assert self._ser is not None
        self.drain(quiet=0.1, timeout=0.4)
        self._ser.sendall(text.encode() + b"\r")

        deadline = time.time() + timeout
        buf = b""
        # the response is complete once the next prompt is echoed back
        while time.time() < deadline:
            chunk = self._recv(self._ser)
            if chunk:
                buf += chunk
                if buf.count(self.prompt) >= 1 and buf.rstrip().endswith(
                    self.prompt.rstrip()
                ):
                    break
            else:
                if buf and self.prompt in buf:
                    break
        return self._extract_response(buf, text)

    def send_keys(self, data: bytes, timeout: float = 3.0) -> str:
        """Send raw keystrokes (no automatic Enter) and return text up to the next prompt."""
        assert self._ser is not None
        self.drain(quiet=0.1, timeout=0.4)
        self._ser.sendall(data)

        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            chunk = self._recv(self._ser)
            if chunk:
                buf += chunk
                if buf.count(self.prompt) >= 1 and buf.rstrip().endswith(
                    self.prompt.rstrip()
                ):
                    break
            else:
                if buf and self.prompt in buf:
                    break
        return self._extract_response(buf, "")

    def _extract_response(self, raw: bytes, echoed: str) -> str:
        text = raw.decode(errors="replace")
        # strip the echoed command and surrounding prompt/whitespace
        lines = [ln.strip() for ln in text.replace("\r", "\n").split("\n")]
        lines = [ln for ln in lines if ln and ln != echoed.strip()]
        cleaned = [ln for ln in lines if not self._is_prompt_line(ln)]
        return "\n".join(cleaned).strip()

    @staticmethod
    def _is_prompt_line(ln: str) -> bool:
        if ln == ">":
            return True
        # OPTION PROMPT CWD: A:/> or A:/DIR>
        if ln.endswith(">") and len(ln) >= 3 and ln[1] == ":":
            return True
        return False

    # -- screen (framebuffer) ---------------------------------------------
    def _monitor_cmd(self, cmd: str) -> None:
        assert self._mon is not None
        try:
            self._mon.settimeout(0.3)
            self._mon.recv(4096)
        except OSError:
            pass
        self._mon.sendall(cmd.encode() + b"\n")
        time.sleep(0.5)

    def screendump(self, dest_ppm: str | None = None) -> str:
        """Capture the emulated framebuffer to a .ppm file and return its path."""
        if dest_ppm is None:
            dest_ppm = os.path.join(self._tmp, f"fb-{time.time_ns()}.ppm")
        self._monitor_cmd(f"screendump {dest_ppm}")
        deadline = time.time() + 5
        while time.time() < deadline:
            if os.path.exists(dest_ppm) and os.path.getsize(dest_ppm) > 0:
                return dest_ppm
            time.sleep(0.1)
        raise HarnessError("screendump did not produce a file")

    def capture_png(self, dest_png: str | None = None) -> str:
        """Capture the framebuffer to a .png file and return its path."""
        ppm = self.screendump()
        png = dest_png or ppm.replace(".ppm", ".png")
        subprocess.run(["convert", ppm, png], check=True, capture_output=True)
        return png

    def screen_size(self) -> tuple[int, int]:
        """Return the emulated HDMI framebuffer (width, height) in pixels."""
        png = self.capture_png()
        out = subprocess.run(
            ["identify", "-format", "%w %h", png],
            check=True, capture_output=True, text=True,
        ).stdout.split()
        return int(out[0]), int(out[1])

    def screen_pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Return the (r, g, b) colour of the framebuffer pixel at (x, y)."""
        png = self.capture_png()
        out = subprocess.run(
            ["convert", png, "-format", f"%[pixel:p{{{x},{y}}}]", "info:"],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
        low = out.lower()
        if "(" in out and ")" in out:
            inner = out[out.find("(") + 1 : out.find(")")]
            parts = [p.strip() for p in inner.replace("%", "").split(",") if p.strip()]
            if len(parts) >= 3:
                r, g, b = (int(float(p)) for p in parts[:3])
                return r, g, b
            if len(parts) == 1 and (low.startswith("gray") or low.startswith("grey")):
                n = int(float(parts[0]))
                return (n, n, n)
        named = {
            "black": (0, 0, 0),
            "white": (255, 255, 255),
            "red": (255, 0, 0),
            "green": (0, 128, 0),
            "lime": (0, 255, 0),
            "blue": (0, 0, 255),
        }
        key = low.split("(")[0].split()[0]
        if key in named:
            return named[key]
        raise HarnessError(f"unparsed pixel colour: {out!r}")

    def image_diff_ratio(self, golden_png: str, fuzz: str = "12%") -> float:
        """Fraction of pixels that differ between the current screen and a
        golden image (ImageMagick absolute-error metric)."""
        png = self.capture_png()
        proc = subprocess.run(
            ["compare", "-metric", "AE", "-fuzz", fuzz, png, golden_png, "null:"],
            capture_output=True, text=True,
        )
        # AE count is reported on stderr (e.g. "1234" or "1.2e+03")
        token = proc.stderr.strip().split()[0].replace(",", "")
        diff = float(token)
        info = subprocess.run(
            ["identify", "-format", "%w %h", golden_png],
            check=True, capture_output=True, text=True,
        ).stdout.split()
        total = int(info[0]) * int(info[1])
        return diff / total if total else 1.0

    def ocr_screen(
        self,
        crop: str | None = "640x300+0+0",
        scale: int = 300,
        threshold: int = 40,
    ) -> str:
        """Screendump the framebuffer and OCR the (preprocessed) text.

        Small bitmap fonts OCR poorly at native size, so the region is
        cropped, converted to high-contrast grayscale and upscaled first.
        """
        ppm = self.screendump()
        png = ppm.replace(".ppm", ".png")
        convert = ["convert", ppm]
        if crop:
            convert += ["-crop", crop, "+repage"]
        convert += [
            "-colorspace", "Gray",
            "-threshold", f"{threshold}%",
            "-resize", f"{scale}%",
            png,
        ]
        subprocess.run(convert, check=True, capture_output=True)
        out = subprocess.run(
            ["tesseract", png, "stdout", "--psm", "6"],
            check=True, capture_output=True, text=True,
        )
        return out.stdout
