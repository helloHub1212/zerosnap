#!/usr/bin/env python3
"""Defer picamera2's optional PyAV import.

``import picamera2`` pulls in ``av`` (the PyAV Python package plus the large
libav shared libraries) even though ZeroSnap only takes still photos.  On a Pi
Zero that is a measurable part of the 10-25 s camera start-up.

The three modules that use PyAV only touch it inside methods, so replacing the
module-level ``import av`` with a lazy proxy keeps video recording working (the
first real use imports PyAV) while the still-camera path never pays for it.

Usage:
    patch-picamera2.py [ROOT]      # ROOT is / on the Pi or the mounted card
"""

from __future__ import annotations

import os
import sys

MARKER = "ZeroSnap: lazy PyAV"

SHIM = '''# ZeroSnap: lazy PyAV import.  Importing `av` costs seconds on a Pi Zero and
# only the video encoder uses it, so load it on first attribute access.
class _LazyAv:
    _module = None

    def __getattr__(self, name):
        module = _LazyAv._module
        if module is None:
            import av as module
            _LazyAv._module = module
        return getattr(module, name)


av = _LazyAv()
'''

FILES = (
    "usr/lib/python3/dist-packages/picamera2/encoders/encoder.py",
    "usr/lib/python3/dist-packages/picamera2/outputs/pyavoutput.py",
    "usr/lib/python3/dist-packages/picamera2/encoders/libav_h264_encoder.py",
    "usr/lib/python3/dist-packages/picamera2/encoders/libav_mjpeg_encoder.py",
)


def _drop_bytecode(path: str) -> None:
    """Remove the .pyc so a stale cached copy cannot win after patching."""
    cache = os.path.join(os.path.dirname(path), "__pycache__")
    if not os.path.isdir(cache):
        return
    base = os.path.basename(path)[:-3]
    for name in os.listdir(cache):
        if name.startswith(base + ".") and name.endswith(".pyc"):
            try:
                os.unlink(os.path.join(cache, name))
            except OSError:
                pass


def main(argv: list[str]) -> int:
    root = argv[1] if len(argv) > 1 else "/"
    changed = 0
    for rel in FILES:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
        if MARKER in text:
            continue
        lines = text.splitlines(keepends=True)
        out = []
        patched = False
        for line in lines:
            if not patched and line.strip() == "import av":
                out.append(SHIM)
                patched = True
            else:
                out.append(line)
        if not patched:
            continue
        backup = path + ".zerosnap.bak"
        if not os.path.exists(backup):
            with open(backup, "w", encoding="utf-8") as handle:
                handle.write(text)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("".join(out))
        _drop_bytecode(path)
        changed += 1
        print(f"[zerosnap] lazy PyAV: {rel}")

    if not changed:
        print("[zerosnap] lazy PyAV: already patched (or picamera2 not installed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
