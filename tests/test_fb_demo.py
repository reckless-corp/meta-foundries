"""Test the framebuffer drawing code without opening any display devices."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "experiments/framebuffer/fb-demo.c"


class FramebufferDemoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="fb-demo-test-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / "fb-demo")
        flags = shlex.split(os.environ.get("FB_DEMO_TEST_CFLAGS", "-O2"))
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        str(SOURCE), "-o", cls.binary], check=True)
        harness = Path(cls.build.name) / "test.c"
        harness.write_text("#define main demo_main\n#include " + json.dumps(str(SOURCE)) + r'''
#undef main
#include <assert.h>
#include <stdarg.h>

static struct canvas make_canvas(unsigned bpp) {
    struct canvas c = {0};
    c.fix.type = FB_TYPE_PACKED_PIXELS;
    c.fix.visual = FB_VISUAL_TRUECOLOR;
    c.var.xres = 64; c.var.yres = 40;
    c.var.xoffset = 3; c.var.yoffset = 2;
    c.var.xres_virtual = 70; c.var.yres_virtual = 45;
    c.var.bits_per_pixel = bpp;
    c.var.red = (struct fb_bitfield){bpp == 16 ? 11 : 16, bpp == 16 ? 5 : 8, 0};
    c.var.green = (struct fb_bitfield){bpp == 16 ? 5 : 8, bpp == 16 ? 6 : 8, 0};
    c.var.blue = (struct fb_bitfield){0, bpp == 16 ? 5 : 8, 0};
    if (bpp == 32) c.var.transp = (struct fb_bitfield){24, 8, 0};
    c.fix.line_length = 70 * (bpp / 8) + 13; /* Deliberately unaligned padding. */
    c.fix.smem_len = c.fix.line_length * 45;
    assert(validate(&c) == 0);
    return c;
}

static uint32_t read_pixel(struct canvas *c, unsigned x, unsigned y) {
    uint32_t value = 0;
    memcpy(&value, row_address(c, y) + x * c->bytes_per_pixel, c->bytes_per_pixel);
    return value;
}

/* Fake only the device-facing syscalls; exercise the actual main/cleanup path. */
static unsigned char device_memory[20000];
static int scenario, device_mode, opens, closes;
int __wrap_open(const char *path, int flags, ...) {
    (void)flags;
    ++opens;
    return !strcmp(path, "/dev/fb0") ? 50 : 51;
}
int __wrap_close(int fd) { (void)fd; ++closes; return 0; }
int __wrap_fstat(int fd, struct stat *st) {
    (void)fd;
    memset(st, 0, sizeof(*st)); st->st_rdev = makedev(4, 1); return 0;
}
int __wrap_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list args; va_start(args, request);
    struct canvas c = make_canvas(32);
    int result = 0;
    if (request == KDSETMODE) {
        int mode = va_arg(args, int);
        if (scenario == 3 && mode == KD_GRAPHICS) { errno = EIO; result = -1; }
        else device_mode = mode;
    } else {
        void *value = va_arg(args, void *);
        if (request == FBIOGET_FSCREENINFO) *(struct fb_fix_screeninfo *)value = c.fix;
        else if (request == FBIOGET_VSCREENINFO) *(struct fb_var_screeninfo *)value = c.var;
        else if (request == KDGETMODE) *(int *)value = device_mode;
        else if (request == VT_GETSTATE) ((struct vt_stat *)value)->v_active = 1;
        else assert(0);
    }
    va_end(args); return result;
}
void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t offset) {
    (void)p; (void)n; (void)prot; (void)flags; (void)fd; (void)offset;
    if (scenario == 2) { errno = ENOMEM; return MAP_FAILED; }
    return device_memory;
}
int __wrap_munmap(void *p, size_t n) { (void)p; (void)n; return 0; }
int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining) {
    (void)request; (void)remaining;
    assert(device_mode == KD_GRAPHICS);
    struct canvas c = make_canvas(32); c.pixels = device_memory;
    assert(read_pixel(&c, 0, 0) == rgb(&c, 255, 0, 0));
    if (scenario == 1) { on_signal(SIGTERM); errno = EINTR; return -1; }
    return 0;
}

int main(void) {
    for (unsigned bpp = 16; bpp <= 32; bpp += 8) {
        struct canvas c = make_canvas(bpp);
        unsigned char *allocation = malloc(c.fix.smem_len + 32);
        assert(allocation);
        memset(allocation, 0xa5, c.fix.smem_len + 32);
        c.pixels = allocation + 16;
        assert(rgb(&c, 255, 0, 0) == (bpp == 16 ? 0xf800u :
                                     bpp == 24 ? 0xff0000u : 0xffff0000u));
        assert(rgb(&c, 0, 255, 0) == (bpp == 16 ? 0x7e0u :
                                     bpp == 24 ? 0xff00u : 0xff00ff00u));
        draw_demo(&c);
        assert(read_pixel(&c, 0, 0) == rgb(&c, 255, 0, 0));
        assert(read_pixel(&c, 16, 0) == rgb(&c, 0, 255, 0));
        assert(read_pixel(&c, 32, 0) == rgb(&c, 0, 0, 255));
        assert(read_pixel(&c, 48, 0) == rgb(&c, 255, 255, 255));
        /* F at (1,20), scale 1: top bar, vertical stem, and dark inner pixel. */
        assert(read_pixel(&c, 1, 20) == rgb(&c, 0, 255, 0));
        assert(read_pixel(&c, 5, 20) == rgb(&c, 0, 255, 0));
        assert(read_pixel(&c, 1, 21) == rgb(&c, 0, 255, 0));
        assert(read_pixel(&c, 2, 21) == rgb(&c, 0, 0, 0));
        rectangle(&c, 63, 39, UINT32_MAX, UINT32_MAX, rgb(&c, 255, 255, 255));
        rectangle(&c, UINT32_MAX, UINT32_MAX, 4, 4, 0);
        assert(read_pixel(&c, 63, 39) == rgb(&c, 255, 255, 255));
        /* Neither invisible pixels, padding nor allocation guards may change. */
        for (size_t i = 0; i < c.fix.smem_len; ++i) {
            size_t y = i / c.fix.line_length, xbyte = i % c.fix.line_length;
            if (y < 2 || y >= 42 || xbyte < 3 * c.bytes_per_pixel ||
                xbyte >= 67 * c.bytes_per_pixel) assert(c.pixels[i] == 0xa5);
        }
        for (unsigned i = 0; i < 16; ++i) {
            assert(allocation[i] == 0xa5);
            assert(allocation[c.fix.smem_len + 16 + i] == 0xa5);
        }
        free(allocation);
    }
    struct canvas c = make_canvas(32);
    c.var.red.offset = 0; c.var.blue.offset = 16;
    assert(validate(&c) == 0);
    assert(rgb(&c, 255, 0, 0) == 0xff0000ffu); /* Reversed channel order. */
    c = make_canvas(32); c.var.red.offset = c.var.green.offset;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.var.red.msb_right = 1;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.fix.line_length = 4;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.fix.smem_len = 4;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.var.xoffset = UINT32_MAX;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.var.yres = 0;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.var.bits_per_pixel = 8;
    assert(validate(&c) < 0);
    c = make_canvas(32); c.fix.visual = FB_VISUAL_PSEUDOCOLOR;
    assert(validate(&c) < 0);
    for (scenario = 0; scenario < 4; ++scenario) {
        memset(device_memory, 0xa5, sizeof(device_memory));
        device_mode = KD_TEXT; interrupted = 0; opens = closes = 0;
        char *args[] = {"fb-demo", NULL};
        int status = demo_main(1, args);
        assert(status == (scenario >= 2 ? 1 : 0));
        assert(device_mode == KD_TEXT);
        assert(opens == closes);
        for (size_t i = 0; i < sizeof(device_memory); ++i) assert(device_memory[i] == 0xa5);
    }
    /* --info must not open the VT or map/change pixel memory. */
    scenario = 2; device_mode = KD_TEXT; opens = closes = 0;
    char *args[] = {"fb-demo", "--info", NULL};
    assert(demo_main(2, args) == 0);
    assert(opens == 1 && closes == 1 && device_mode == KD_TEXT);
    puts("Pixel formats, glyphs, clipping, stride, offsets and bounds passed");
    return 0;
}
''')
        cls.harness = str(Path(cls.build.name) / "test")
        wrappers = ["open", "close", "ioctl", "fstat", "mmap", "munmap", "nanosleep"]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        *[f"-Wl,--wrap={name}" for name in wrappers],
                        str(harness), "-o", cls.harness], check=True)

    def test_pixels_and_bounds(self):
        subprocess.run([self.harness], check=True, capture_output=True)

    def test_cli_failures_and_help(self):
        for value in ("0", "-1", "3601", "abc", ""):
            result = subprocess.run([self.binary, "--seconds", value], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"--seconds", result.stderr)
        result = subprocess.run([self.binary, "--help"], capture_output=True)
        self.assertEqual(result.returncode, 0)
        with tempfile.NamedTemporaryFile() as fake:
            result = subprocess.run([self.binary, "--info", "--fb", fake.name],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"framebuffer information", result.stderr)


if __name__ == "__main__":
    unittest.main()
