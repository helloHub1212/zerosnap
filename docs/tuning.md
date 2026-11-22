# Tuning

Everything here is measured on a Pi Zero v1.3 with a 2.4" ILI9341 panel.
The panel, not the CPU, is almost always the limit.

## The arithmetic

A full frame is:

```
320 × 240 × 2 bytes = 153,600 bytes
```

At an SPI clock `f`, the transfer alone takes `153600 × 8 / f` seconds:

| Requested SPI | Effective (core_freq 250) | Full frame | Ceiling |
|---|---|---|---|
| 16 MHz | 16.0 MHz | 76.8 ms | 13.0 fps |
| 32 MHz | 31.25 MHz | 39.3 ms | 25.4 fps |
| 50 MHz | 41.67 MHz | 29.5 ms | 33.9 fps |
| 60 MHz | 50.0 MHz | 24.6 ms | 40.6 fps |

`fbtft` divides `core_freq` by an **even** integer, so a request of 32 MHz at a
250 MHz core gives 31.25 MHz, and 40 MHz gives the same 31.25 MHz — to go
faster you must request 50 or 60.

## `fps=` is a delay, not a frame rate

`fbtft`'s `fps` parameter is the delay between a dirty page and the flush:

```c
fbdefio->delay = HZ / fps;
```

Effective throughput is roughly `1 / (1/fps + transfer)`:

| `fps=` | Added delay | At 31.25 MHz |
|---|---|---|
| 25 (vendor default) | 40 ms | ~12.6 fps |
| 30 | 33 ms | ~13.8 fps |
| 60 (our default) | 17 ms | ~17.8 fps |
| 100 | 10 ms | ~20.3 fps |

Measured end-to-end with `fps=60` is **26.5 fps**, higher than the naive model,
because writes are pipelined rather than strictly serialised. Still, raising
`fps` is the cheapest win if you want more.

## What we ship

```
dtoverlay=tft9341:rotate=270,speed=32000000,fps=60,txbuflen=65536
```

- `rotate=270` matches the panel's physical mounting;
- `speed=32000000` → 31.25 MHz, stable on the modules we tested;
- `fps=60` balances latency against CPU time;
- `txbuflen=65536` uses fewer DMA transfers per frame than the 32 KB default.

## Measuring

```bash
sudo systemctl stop zerosnap-ui
/opt/zerosnap/bin/zerosnap-bench --seconds 6            # full frame
/opt/zerosnap/bin/zerosnap-bench --seconds 6 --rows 120
sudo systemctl start zerosnap-ui
```

It writes straight into `/dev/fb1`, with no UI code involved. If this reports
~1 fps, the problem is the panel or the overlay, not the app.

The UI's own numbers go to `/var/log/zerosnap/app.log`:

```
ui: 26.5 fps, draw=3.4 present=18.0 ms/frame
```

- `draw` is CPU work in the renderer;
- `present` is the `memcpy` (and, indirectly, how long the kernel made us wait).

## Things that do not help

- **`draw` is not the bottleneck.** It is 3–4 ms. Optimising icons or text will
  not move the frame rate.
- **The 64 px side column is free.** `fbtft` sends whole rows; narrowing the
  preview to the left of the sidebar does not reduce SPI bytes.
- **Only fewer rows help.** A 16:9 preview (e.g. 256×144) cuts the transfer to
  92 KB and lifts the ceiling to ~30 fps, at the cost of letterboxing.
- **More `fps` costs CPU.** On a single core that competes with the camera.

## Overclocking risks

Modules rated for 10–15 MHz often run happily at 31 MHz; 40–50 MHz is luck.
Symptoms of too fast: shifted rows, coloured noise, occasional white frames.
If you see them, drop to `speed=24000000`.

Also note that `core_freq` changes when the Pi is idle, which changes the
divider and therefore the SPI clock. If you overclock, fix the core clock:

```
core_freq=250
core_freq_min=250
```

## Boot latency

Measured on the same Pi Zero v1.3 (kernel 6.12.109+rpt-rpi-v6, ~9 MB/s SD):

| Stage | Before | After |
|---|---|---|
| Firmware → `Booting Linux` | 15.6–28.6 s (varied with FAT repair) | ~10.3 s |
| `zerosnap-ui.service` started | 27–41 s | ~11.8 s |
| UI process in `app.log` | ~40 s | ~12–13 s |

Firmware variance was caused by the FAT boot partition being dirty: the
firmware repaired it at boot, which took ~13 s extra. The installer now sets

```
auto_initramfs=0
display_auto_detect=0
boot_delay=0
```

and changes the `/boot/firmware` fstab entry to pass `0` with `flush,noatime`.
The kernel has ext4, MMC, the BCM2835 SD controller and SCSI built in
(`=y`), so no initramfs is needed to mount root. Unused `initramfs7*`,
`initramfs8` and the stale `FSCK*.REC` files are removed (about 50 MB).
The original `initramfs` is kept for rollback.

`zerosnap-ui.service` starts with `DefaultDependencies=no` right after
`systemd-remount-fs.service` instead of waiting for `basic.target`. The SPI
panel modules are preloaded through `etc/modules-load.d/zerosnap-panel.conf`,
and `glamor-test`, `sshswitch` and three `Persistent=true` timers are masked.
`systemd-timesyncd` moved from `sysinit.target.wants` to
`multi-user.target.wants` (it cannot sync before the network is up anyway).

**`default.target` stays `graphical.target`.** LXDE needs `udisks2.service`
from `graphical.target.wants`; without it `gvfs-udisks2-volume-monitor`
segfaults and `pcmanfm`/`lxpanel` crash, leaving a black desktop with only a
mouse cursor. LightDM is not started at boot (`display-manager.service` is
removed), so this does not bring back the old startup delay.

## Camera start-up

The slow part is Python, not libcamera: importing `numpy` + `picamera2` takes
10–15 s on ARMv6. Three things overlap or remove that cost:

1. The daemon publishes shared memory first, so the UI can link and draw
   "camera starting" while Python is still loading.
2. `app/daemon.py` starts the import in a helper thread and waits for the
   `/boot/firmware` mount in parallel. The helper calls `os.nice(10)` so it
   does not starve udev on the single core — at the daemon's default `nice -10`
   the camera kernel drivers were delayed by ~11 s.
3. `install/patch-picamera2.py` replaces four module-level `import av`
   statements with a lazy proxy. Still photos never load PyAV/libav; video
   loads it on first use. Originals are kept as `*.zerosnap.bak`.

**Do not call `Picamera2.global_camera_info()` before the camera drivers are
ready.** libcamera's `CameraManager.singleton()` enumerates cameras once per
process and caches the result. An early call freezes an empty camera list and
the UI stays on "camera not started" forever. `Daemon.open()` waits for
`/dev/media*` before the first enumeration; if enumeration still fails, the
daemon exits so the supervisor restarts it with a fresh singleton.

Timeline markers in `/var/log/zerosnap/app.log` (all times are uptime):

```
daemon: modules loaded, initializing (uptime ...)
daemon: media partition ready (...)
daemon: camera imports preloaded (...)
daemon: camera media devices ready (...)
libcamera sees 1 camera(s), first is ov5647
daemon: first preview published (...)
```

## Switching to the desktop

LightDM's vendor unit waits for `dev-dri-card0.device` and
`dev-dri-renderD128.device`. Those devices never exist with the legacy fbdev
panel, so the start job timed out after 90 s. A drop-in cannot remove
`Wants=`/`After=` entries — those lists are additive across unit files. The
installer therefore writes a **full override** to
`/etc/systemd/system/lightdm.service` with the two device names stripped, and
removes the old ineffective drop-in. The override is written during install, so
the normal exit path does not need `systemctl daemon-reload`.

`exit-to-desktop.sh` also makes sure `udisks2.service` and
`accounts-daemon.service` are active before starting LightDM, for the reason
described above.
