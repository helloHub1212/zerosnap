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
