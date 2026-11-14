# Architecture

ZeroSnap is two processes and one shared memory segment.

```
systemd (tty1)
  └── /opt/zerosnap/start-camera.sh
        ├── supervisor loop ── python3 -m app.daemon
        │                        └── Picamera2 worker thread
        └── exec /opt/zerosnap/bin/zerosnap-ui
```

There is no X server, no window manager and no compositor. `zerosnap-ui` opens
`/dev/fb1` and `/dev/input/event*` itself.

## Why the display path matters

An SPI panel is not a GPU framebuffer. On a Pi Zero the panel is fed by the
`fbtft` driver at roughly 31 MHz, so a full 320×240×16bpp frame takes
~39 ms — the ceiling is ~25 fps no matter what draws it.

The original build went through X11, which meant every frame travelled:

```
pygame → SDL → X11 → legacy /dev/fb0 → fbcp → vc_dispmanx_snapshot()
       → /dev/fb1 → fbtft → SPI
```

`vc_dispmanx_snapshot()` is a synchronous GPU readback; on the Pi Zero it
serialised with X's own framebuffer updates. Measured result: 1004 ms per
frame. Removing the chain removed the problem.

`zerosnap-ui` therefore does exactly one thing per frame:
`draw into a RAM buffer → memcpy into the mmap'd framebuffer`.

## Shared memory

One `shm_open("/zerosnap-shm")` segment, `struct CamShm` in
[`native/src/cam.h`](../native/src/cam.h). Python mirrors the layout with a
`ctypes.Structure` in [`app/camera_shm.py`](../app/camera_shm.py) and derives
every offset from it, so the two sides cannot silently drift.

| Region | Direction | Contents |
|---|---|---|
| `frame_data[2]` + `frame_seq` | daemon → UI | 320×240 RGB565, double buffered |
| `state_seq` + state fields | daemon → UI | free space, counts, flags, file names |
| `cmd_seq` + `cmd`, `cmd_arg` | UI → daemon | shutter / refresh / quit |

Ordering rule: the payload is written first, then the sequence counter is
incremented. The reader compares the counter before and after copying and
retries if it changed. Both sides are single-threaded at the boundary, so on
the Pi's single core this is sufficient without a portable fence.

## The UI, without a toolkit

- **Framebuffer**: `fb.c` mmaps `/dev/fb1`, keeps a `uint16_t` scratch buffer
  and copies rows on present.
- **Draw**: `gfx.c` has rects, lines, circles, rounded rects, alpha fills and
  blits — all clipped, all RGB565.
- **Text**: `tools/gen_font.py` runs at build time with Pillow and a CJK font,
  and emits `src/text_data.c`: one alpha bitmap per UI string plus an ASCII
  strip for dynamic values. `text.c` only looks up and blends.
- **Icons**: drawn with primitives at runtime; they are a few dozen pixels.
- **Input**: `input.c` handles multitouch protocol B, single touch and
  pressure-only panels, and coalesces moves at `SYN_REPORT`.
- **Calibration**: `touchcal.c` stores three bits (swap, invert_x, invert_y).
  Two taps are mapped through all eight combinations; the one whose two points
  land closest to the requested screen positions wins.

## The daemon

`app/daemon.py` is deliberately boring:

1. create the segment and publish an empty state (so the UI can link early);
2. wait until libcamera can enumerate a sensor (the UI now starts at ~8 s,
   before the camera stack is always ready);
3. start `Picamera2Service`;
4. every ~8 ms: poll one command, republish state, and if the preview array
   changed, convert it to RGB565 and publish it.

The conversion is `rgb888_to_rgb565()` from `backend/native/yuv420.c`,
pre-compiled to `libyuv420.so` at install time. numpy would cost ~10 ms per
frame on an ARM1176; the C loop is ~1 ms.

The service still knows how to record video, because it is the same backend the
project started with. The UI never sends those commands any more.

## Boot sequence

```
~2.5 s  kernel, fbtft registers /dev/fb1
~6 s    zerosnap-ui.service starts; launcher waits for /dev/fb1
~8 s    UI is up, shows "启动中" until the segment appears
~9 s    daemon publishes the empty state, UI links
~12 s   libcamera enumerates the OV5647, preview starts
```

If the UI cannot link within a second it retries forever; if the camera fails,
the daemon retries every 5 s. Three fast UI failures stop the service so the
console (with the log) stays visible on the panel.

## Failure handling

| Failure | Behaviour |
|---|---|
| daemon crashes | supervisor restarts it after 2 s; UI reconnects automatically |
| camera open fails | daemon logs it and retries every 5 s |
| UI crashes 3× in 120 s | service exits; tty1 shows the console |
| touch missing | UI runs without input; calibration times out after 45 s |
| panel missing | UI waits 8 s for `/dev/fb1`, then logs and exits |
