"""Exercise wrapper cleanup on a PTY and framebuffer-only renderer CLI errors."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unittest


FILES = (Path(__file__).resolve().parents[1] / "meta-project" /
         "recipes-core/matrix-console/files")


class Console:
    def __init__(self, rows, cols, **environment):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            env = dict(os.environ, MATRIX_DATA_DIR=str(FILES), TERM="xterm")
            env.update(environment)
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
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="matrix-render-test-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / "matrix-render")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(FILES / "matrix-render.c"), "-o", cls.binary], check=True)
        cls.stub = Path(cls.build.name) / "renderer-stub"
        cls.stub.write_text("""#!/bin/sh
if [ "$1" = --restore-console ]; then
    echo RESTORED
    exit
fi
printf 'ARGS: %s\\n' "$*"
trap 'echo STOPPED; exit 0' TERM INT HUP
while :; do sleep 0.05; done
""")
        cls.stub.chmod(0o755)

    def test_wrapper_arguments_and_cleanup(self):
        console = Console(24, 80, MATRIX_RENDERER=str(self.stub),
                          MATRIX_FB_DEVICE="/dev/test-fb", MATRIX_SCALE="2",
                          MATRIX_TTY="/dev/test-tty")
        try:
            data = console.read(0.3)
            self.assertIn(b"ARGS: --fb /dev/test-fb --tty /dev/test-tty --scale 2", data)
            self.assertNotIn(b"--backend", data)
            self.assertIn(b"\x1b[?25l", data)
            self.assertFalse(termios.tcgetattr(console.fd)[3] & termios.ECHO)
        finally:
            cleanup, status = console.stop()
            restored = termios.tcgetattr(console.fd)[3] & termios.ECHO
            os.close(console.fd)
        self.assertEqual(status, 0)
        self.assertTrue(restored)
        self.assertIn(b"STOPPED", cleanup)
        self.assertIn(b"RESTORED", cleanup)
        self.assertIn(b"\x1b[?25h", cleanup)

    def test_framebuffer_failure_has_no_terminal_fallback(self):
        result = subprocess.run([self.binary, "--fb", "/nonexistent/matrix-fb",
                                 str(FILES / "thumbs.txt")], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"framebuffer setup", result.stderr)

    def test_removed_backend_option_and_invalid_scale(self):
        for args in (["--backend", "terminal"], ["--backend", "auto"],
                     ["--scale", "0"], ["--scale", "5"]):
            result = subprocess.run([self.binary, *args, str(FILES / "thumbs.txt")],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"Usage:", result.stderr)

    def test_renderer_rejects_missing_artwork(self):
        result = subprocess.run([self.binary, "/nonexistent/artwork"],
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"artwork", result.stderr)

    def test_missing_artwork_fails_before_terminal_changes(self):
        result = subprocess.run(
            ["sh", str(FILES / "matrix-console")], capture_output=True,
            env=dict(os.environ, MATRIX_DATA_DIR="/nonexistent/matrix-console"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"missing renderer or artwork", result.stderr)


if __name__ == "__main__":
    unittest.main()
