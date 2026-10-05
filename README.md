# Yocto images

Build either image from the repository root:

```sh
./yocto/build.sh intel-corei7-64.yml
./yocto/build.sh uno-q.yml
```

Both configurations include `meta-project` and its `matrix-console` package.
After normal boot messages, the first Linux virtual console (`/dev/tty1`,
normally the HDMI display) shows green Matrix rain and the Thumbs Up artwork.
This runs directly on the console, without a container or network connection.
The BSP must provide a working fbdev framebuffer (/dev/fb0) and virtual console;
installing this package does not add display support to the board.

Use **Ctrl+Alt+F2** for a local login and **Ctrl+Alt+F1** to return to the
animation. Existing SSH and serial access are unaffected. The service never
switches the active VT, including when it restarts after a crash.

## Operation

### Intel binary format handlers

The Intel kernel configures `CONFIG_BINFMT_MISC=m`. The image explicitly installs
`kernel-module-binfmt-misc`; enabling a module in the kernel configuration alone
does not install its package in the root filesystem. The image also includes
`systemd-binfmt` to mount `/proc/sys/fs/binfmt_misc` on demand and register
handlers configured in `/etc/binfmt.d/*.conf` at boot. Interpreters such as
QEMU and their handler definitions must be supplied separately.

After rebuilding and booting the Intel image, check that the interface is enabled:

```sh
modinfo binfmt_misc
cat /proc/sys/fs/binfmt_misc/status
```

`modinfo` should locate the module for the running kernel, and `status` should
print `enabled`. After adding or changing handler definitions, run
`sudo systemctl restart systemd-binfmt` to apply them.

### Matrix console

Run these commands as root (or using `sudo`):

```sh
systemctl status matrix-console
journalctl -u matrix-console
systemctl stop matrix-console
systemctl start matrix-console
systemctl restart matrix-console
```

Stopping the service clears the display and restores the cursor and terminal
settings. Console blanking is disabled while running and restored to the kernel
default when stopped; a previous custom per-console timeout is not preserved.
Stopping does not automatically start a tty1 login.

To restore a normal tty1 login persistently on an installed device:

```sh
systemctl disable --now matrix-console
systemctl unmask getty@tty1.service
systemctl enable --now getty@tty1.service
```

To restore the animation:

```sh
systemctl disable --now getty@tty1.service
systemctl mask getty@tty1.service
systemctl enable --now matrix-console
```

The package masks only tty1's getty and explicitly enables tty2's getty.
Its systemd preset skips the masked tty1 instance and selects tty2 when Yocto
applies presets during rootfs creation.
For images without the animation, remove `matrix-console` from `IMAGE_INSTALL`
in `common.yml` and rebuild. The getty mask belongs to that package.

## Development and validation

### Standalone framebuffer experiment

`experiments/framebuffer/fb-demo.c` is a separate learning demo, not installed
or started by the image. It draws red, green, blue and white bars, then two
lines of enlarged bitmap lettering directly into `/dev/fb0` for ten seconds.
The code walks through querying the layout, mapping pixel memory, packing RGB
values, drawing rectangles and a hand-drawn 5×7 font, and restoring the console.

Build it locally, without BitBake:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  yocto/experiments/framebuffer/fb-demo.c -o /tmp/fb-demo
```

For Uno Q, use an ARM64 compiler instead of the host compiler. For example,
with a Debian ARM64 cross-toolchain installed:

```sh
aarch64-linux-gnu-gcc -std=c11 -O2 -static -Wall -Wextra -Werror \
  yocto/experiments/framebuffer/fb-demo.c -o /tmp/fb-demo-aarch64
scp /tmp/fb-demo-aarch64 uno-q-2g:/tmp/fb-demo
```

Use your device's SSH host/address in the `scp` command. On the device, run
this from the host SSH shell, outside containers. First query the framebuffer
without changing its pixels or the console mode:

```sh
sudo /tmp/fb-demo --info
```

Then stop Matrix for the demo and restart it automatically afterwards:

```sh
sudo sh -c 'systemctl stop matrix-console || exit; trap "systemctl start matrix-console" EXIT; /tmp/fb-demo --seconds 10'
```

The demo expects tty1 to be active and in text mode; use `sudo chvt 1` first
if needed. `--fb` and `--tty` select other devices. It enters `KD_GRAPHICS`
to suppress console drawing, saves and restores the visible pixels, and
restores the previous console mode on normal exit, Ctrl+C, SIGTERM or SIGHUP.
It does not change resolution or implement VT switching/hotplug handling;
keep the display configuration and active VT unchanged during the short test.
If forcibly killed with SIGKILL, restore text mode explicitly:

```sh
sudo /tmp/fb-demo --restore-text
sudo systemctl start matrix-console
```

The demo supports little-endian packed truecolor RGB at 16, 24 or 32 bits per
pixel, querying channel offsets, row stride and visible offsets rather than
assuming them. Unsupported formats are reported before changing console mode.
It uses CPU drawing through the driver's fbdev interface, with no GPU
acceleration, explicit page flips or vsync. This static picture tests display
access and pixel layout; it is not an animation performance benchmark.

Drawing tests use simulated framebuffers with padded rows and nonzero offsets:

```sh
python3 -B -m unittest discover -s yocto/tests -p test_fb_demo.py -v
```

API references: [framebuffer layout](https://docs.kernel.org/fb/api.html),
[console graphics/text mode](https://man7.org/linux/man-pages/man2/ioctl_kd.2.html).

### Matrix renderer

The console renderer and artwork live in
`meta-project/recipes-core/matrix-console/files`. They are a standalone
adaptation of `../containers/matrix/matrix.sh`; the HTTP application remains
independent. The Matrix console code and artwork are licensed under BSD-2-Clause
(see `files/LICENSE.matrix-console` in the recipe); the embedded Spleen font
retains its own BSD-2-Clause copyright notice. Both license files are installed
alongside the artwork. The C renderer draws directly into mapped `/dev/fb0` memory,
using cached green/white glyph tiles and copying only cells whose character or
color changed. It pauses for 60 ms between frames. There is no terminal
rendering backend; framebuffer setup failures are reported to the journal.
Small displays clip the centered artwork.

The renderer puts tty1 in graphics mode and uses embedded Spleen 8×16 glyphs.
It supports packed true-color 16/24/32-bit layouts, stride padding and display
offsets. Use `--scale 2` for 16×32 cells. The ASCII font is from
[Spleen 2.2.0](https://github.com/fcambus/spleen), under BSD-2-Clause; its license
is installed alongside the artwork and available via `matrix-render --font-license`.

VT switching releases the display and redraws on return. Normal exit restores
text mode, and service cleanup also restores text mode after a renderer crash.
Framebuffer layout changes cause a restart to obtain the new mapping. Startup
requires tty1 to be active; if it is inactive, the service retries without
switching away from the user's console.

Override settings in `/etc/default/matrix-console`, then restart the service:
`MATRIX_FB_DEVICE=/dev/fb0` and `MATRIX_SCALE=1` (1–4).
The standalone renderer also accepts `--tty` to select a different active VT.
The service is wired to tty1 via systemd's `TTYPath`.

Build a static ARM64 executable for quick Uno Q testing (Docker and network
access required; no Yocto build):

```sh
yocto/experiments/framebuffer/build-matrix-static.sh
scp /tmp/matrix-framebuffer-build/matrix-render-aarch64 uno-q-2g:/tmp/matrix-render
```

From SSH on the device, run the framebuffer animation. Ctrl+C restores the
installed service:

```sh
chmod +x /tmp/matrix-render
sudo sh -c 'systemctl stop matrix-console || exit; trap "systemctl start matrix-console" EXIT; /tmp/matrix-render /usr/share/matrix-console/thumbs.txt'
```

Measure CPU usage from another session with `pidstat -u -C matrix-render 1 10`.
This test uses the existing artwork and does not replace installed files.
If the standalone test is forcibly killed, restore text mode before restarting:

```sh
sudo /tmp/matrix-render --restore-console
sudo systemctl start matrix-console
```

Run the host tests with Python 3, a C compiler (`cc`), `stty`, and `systemctl`
(override the latter with `SYSTEMCTL` if needed):

```sh
python3 -m unittest discover -s yocto/tests -v
```

Tests use simulated framebuffer memory and device calls to check pixel formats,
glyph tiles, changed-cell rendering, VT switching and cleanup. Wrapper tests
use a pseudoterminal and a stub renderer to check terminal settings and process
cleanup. They do not require a physical framebuffer.

After building, boot each target and check animation startup, tty2 login and
return to tty1, display resizing, and continued animation past the usual console
blanking timeout. From tty2 or SSH, kill the renderer and verify systemd restarts
it without switching away from tty2. Verify that service stop/start restores
the cursor and redraws correctly. Host tests cannot measure driver rendering
costs; compare CPU usage on the device.
