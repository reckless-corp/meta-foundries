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
The Uno Q BSP must provide a working display driver and virtual console;
installing this package does not add display support to the board.

Use **Ctrl+Alt+F2** for a local login and **Ctrl+Alt+F1** to return to the
animation. Existing SSH and serial access are unaffected. The service never
switches the active VT, including when it restarts after a crash.

## Operation

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

The console renderer and artwork live in
`meta-project/recipes-core/matrix-console/files`. They are a standalone
adaptation of `../containers/matrix/matrix.sh`; the HTTP application remains
independent. ASCII and standard Linux-console colors avoid font dependencies.
Terminal dimensions are checked approximately once per second, falling back to
80×24 when unavailable. Small screens clip the centered artwork.

Run the terminal lifecycle and rendering checks on a Linux host with Python 3,
`awk`, `stty`, and fractional `sleep` support. The rootfs preset regression test
also requires `systemctl` (override its path with `SYSTEMCTL` if needed):

```sh
python3 -m unittest discover -s yocto/tests -v
```

For an interactive host preview (Ctrl+C exits):

```sh
MATRIX_DATA_DIR="$PWD/yocto/meta-project/recipes-core/matrix-console/files" \
  sh yocto/meta-project/recipes-core/matrix-console/files/matrix-console
```

After building, boot each target and check animation startup, tty2 login and
return to tty1, display resizing, and continued animation past the usual console
blanking timeout. From tty2 or SSH, kill the renderer and verify systemd restarts
it without switching away from tty2. Verify that service stop/start restores
the cursor and redraws correctly. These hardware checks are required especially
for the Uno Q display path; host pseudoterminal tests cannot verify it.
