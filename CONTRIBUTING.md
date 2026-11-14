# Contributing to ZeroSnap

Thanks for taking the time to contribute. This project targets one very small
device, so the bar for new dependencies is high.

## Reporting a bug

Please include:

- the output of `uname -a` and `cat /etc/os-release | head -3`;
- your panel model and which overlay you use (`grep dtoverlay /boot/firmware/config.txt`);
- `/var/log/zerosnap/app.log` (at least the last 40 lines);
- whether the desktop (`sudo systemctl start lightdm`) still works on the same SD card.

If the screen shows a message instead of the preview, quote it — the UI
distinguishes "starting", "camera not started" and "no signal" on purpose.

## Pull requests

1. **Run the tests.** `make test` must pass on the host; it needs `gcc` and
   `python3` only.
2. **Keep the UI dependency-free.** `native/` may use libc, libm, pthread and
   librt and nothing else. No SDL, no freetype, no libjpeg — text and previews
   are pre-rendered by design.
3. **Do not allocate on the draw path.** `ui_draw` and everything below it runs
   at 26 fps on a 1 GHz single core.
4. **Match the surrounding style.** C11, 4-space indent, 80 columns, `snake_case`
   for functions and types, `g_` only for file-static state.
5. **Explain measurements.** If a change is about performance, include the
   before/after numbers from `zerosnap-bench` or the `ui:` line in the log.
6. **Do not add a feature that needs a display server.** That is the whole point
   of the project.

## Where things live

| Area | Files |
|---|---|
| Framebuffer, drawing | `native/src/fb.c`, `native/src/gfx.c` |
| Screen layout, hit testing | `native/src/ui.c` (geometry lives at the top) |
| Touch + calibration | `native/src/input.c`, `native/src/touchcal.c` |
| Shared memory protocol | `native/src/cam.h` (single source of truth), `native/src/ipc.c`, `app/camera_shm.py` |
| Camera | `app/daemon.py`, `app/backend/` |
| Build / install | `native/Makefile`, `install/install.sh` |

`native/src/cam.h` is the contract between C++ and Python. If you change the
struct, both sides must change together and `native/tests/ipc_test.c` must be
updated.

## Commit messages

Short imperative subject line, then a body that explains *why*. Example:

```
touch: solve the axis transform from two taps

The ADS7846 reports native panel axes while the display is rotated 270
degrees, so every tap landed 90 degrees away. Two taps are enough to pick
one of eight transforms.
```

## License

By contributing you agree that your work is released under the MIT license
(see `LICENSE`).
