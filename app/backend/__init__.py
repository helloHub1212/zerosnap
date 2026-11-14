"""Camera backend factory.

This package is the only place that knows whether the real Picamera2 backend
or the synthetic mock is used.  Importing it must work on a PC without
libcamera, so picamera2 is only imported when :func:`create_camera_service`
actually tries to build the real service.
"""

from __future__ import annotations

import logging

from .. import config
from ..interfaces import CameraProtocol, StorageProtocol

logger = logging.getLogger("camera.backend")

__all__ = ["create_camera_service"]


def _picamera2_importable() -> bool:
    """Return True when picamera2 and its native libcamera bindings load."""
    try:
        import picamera2  # noqa: F401  (availability probe only)
    except Exception as exc:  # ImportError, OSError from libcamera, ...
        logger.info("picamera2 is not available (%s); using the mock camera", exc)
        return False
    return True


def create_camera_service(storage: StorageProtocol) -> CameraProtocol:
    """Build the camera service that fits the current machine.

    ``CAMERA_MOCK=1`` always selects the mock.  Otherwise the real backend is
    used when picamera2 can be imported; any failure falls back to the mock so
    the UI still starts.
    """
    if not config.MOCK and _picamera2_importable():
        try:
            from .service import Picamera2Service

            return Picamera2Service(storage)
        except Exception:
            logger.exception("cannot create the Picamera2 service; using the mock camera")

    from .mock import MockCameraService

    return MockCameraService(storage)
