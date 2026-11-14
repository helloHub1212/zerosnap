"""Static configuration for the camera application.

Everything here is import-safe on a PC (no pygame / picamera2 imports).
"""

from __future__ import annotations

import os
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

#: Directory the user sees on Windows / Linux when the SD card is mounted.
#: This is the FAT32 boot partition on the Pi.
MEDIA_DIR = os.environ.get("CAMERA_MEDIA_DIR", "/boot/firmware/Camera")

#: Thumbnails, logs and other regenerable data live on the ext4 rootfs.
CACHE_DIR = Path(os.environ.get("CAMERA_CACHE_DIR", os.path.expanduser("~/.cache/zerosnap")))

LOG_PATH = CACHE_DIR / "app.log"
THUMB_DIR = CACHE_DIR / "thumbs"

#: Installed application directory on the Pi.
INSTALL_DIR = Path("/opt/zerosnap")

# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

SCREEN_SIZE = (320, 240)
PREVIEW_SIZE = (320, 240)
UI_FPS = 30
#: Frame rate requested for the live preview.  The sensor is forced into a
#: small mode for preview so the ISP does not have to downscale 5 MP.
PREVIEW_FPS = 30

FONT_PATH = "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"

# ---------------------------------------------------------------------------
# Camera
# ---------------------------------------------------------------------------

PHOTO_EXT = ".jpg"
VIDEO_EXT = ".mp4"
PHOTO_QUALITY = 95

#: name -> (width, height, fps, bitrate)
VIDEO_MODES: dict[str, tuple[int, int, int, int]] = {
    "1080p30": (1920, 1080, 30, 12_000_000),
    "720p30": (1280, 720, 30, 8_000_000),
    "480p60": (640, 480, 60, 6_000_000),
}

DEFAULT_VIDEO_MODE = "1080p30"

#: Stop recording before the FAT32 4 GiB per-file limit, leaving room for the
#: container to be finalised.
MAX_VIDEO_BYTES = int(3.8 * 1024**3)

#: Stop recording when the card is (almost) full.
MIN_FREE_BYTES = 300 * 1024**2

#: How long a photo capture may take before the UI reports a timeout.
PHOTO_TIMEOUT_S = 20.0

# ---------------------------------------------------------------------------
# Development helpers
# ---------------------------------------------------------------------------

#: Force the synthetic camera.  Also used automatically when picamera2 is
#: missing (e.g. on a PC).
MOCK = os.environ.get("CAMERA_MOCK", "") == "1"

#: Set to 1 to use a window instead of fullscreen (desktop development).
WINDOWED = os.environ.get("CAMERA_WINDOWED", "") == "1"
