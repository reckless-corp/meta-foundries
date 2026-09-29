"""Exercise the real renderer on a controlling pseudoterminal (Linux host)."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import termios
import time
import unittest


FILES = (Path(__file__).resolve().parents[1] / "meta-project" /
         "recipes-core/matrix-console/files")
CSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


class Console:
    def __init__(self, rows, cols):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            env = dict(os.environ, MATRIX_DATA_DIR=str(FILES), TERM="xterm")
            os.execve("/bin/sh", ["sh", str(FILES / "matrix-console")], env)

    def resize(self, rows, cols):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def read(self, seconds):
        data = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if select.select([self.fd], [], [], max(0, deadline - time.monotonic()))[0]:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise
                if not chunk:
                    break
                data.extend(chunk)
        return bytes(data)

    def stop(self):
        os.kill(self.pid, signal.SIGTERM)
        data = self.read(3)
        pid, status = os.waitpid(self.pid, os.WNOHANG)
        if not pid:
            os.killpg(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
            raise AssertionError("console failed to stop within three seconds")
        return data, os.waitstatus_to_exitcode(status)


class MatrixConsoleTest(unittest.TestCase):
    def assert_frame(self, data, rows, cols):
        # A complete frame consists of one absolute-position write per row.
        positions = list(re.finditer(rb"\x1b\[(\d+);1H", data))
        for index, position in enumerate(positions):
            if int(position[1]) != 1 or index + rows >= len(positions):
                continue
            block = positions[index:index + rows + 1]
            if [int(p[1]) for p in block[:-1]] != list(range(1, rows + 1)):
                continue
            lines = [CSI.sub(b"", data[a.end():b.start()])
                     for a, b in zip(block, block[1:])]
            self.assertEqual([len(line) for line in lines],
                             [cols] * (rows - 1) + [cols - 1])
            self.assertNotIn(b"\n", b"".join(lines))
            return b"\n".join(lines)
        self.fail(f"no complete {cols}x{rows} frame in {len(data)} bytes")

    def test_render_resize_and_cleanup(self):
        console = Console(24, 80)
        try:
            data = console.read(0.6)
            frame = self.assert_frame(data, 24, 80)
            self.assertIn(b"Thumbs Up", frame)
            self.assertIn(b"\x1b[?25l", data)
            self.assertFalse(termios.tcgetattr(console.fd)[3] & termios.ECHO)
            console.resize(6, 12)
            data = console.read(1.5)
            # Ignore buffered old-size frames preceding the resize clear.
            self.assert_frame(data.rsplit(b"\x1b[2J", 1)[-1], 6, 12)
        finally:
            cleanup, status = console.stop()
            restored = termios.tcgetattr(console.fd)[3] & termios.ECHO
            os.close(console.fd)
        self.assertEqual(status, 0)
        self.assertTrue(restored)
        self.assertIn(b"\x1b[?25h", cleanup)

    def test_unknown_dimensions_fall_back(self):
        console = Console(0, 0)
        try:
            self.assert_frame(console.read(0.6), 24, 80)
        finally:
            console.stop()
            os.close(console.fd)

    def test_missing_artwork_fails_before_terminal_changes(self):
        result = subprocess.run(
            ["sh", str(FILES / "matrix-console")], capture_output=True,
            env=dict(os.environ, MATRIX_DATA_DIR="/nonexistent/matrix-console"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"missing renderer or artwork", result.stderr)


if __name__ == "__main__":
    unittest.main()
