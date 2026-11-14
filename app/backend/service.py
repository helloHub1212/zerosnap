"""Real camera backend built on Picamera2.

All libcamera / Picamera2 calls run in one worker thread.  The public methods
only flip state under a lock or push a command into a queue, so the 30 fps UI
can poll them without ever blocking.

picamera2 is imported lazily inside :meth:`Picamera2Service._open_camera`; this
module can therefore be imported (and :func:`app.backend.create_camera_service`
used) on a PC without libcamera.
"""

from __future__ import annotations

import ctypes
import logging
import os
import queue
import threading
import time
from dataclasses import replace
from typing import Any

import numpy as np

from .. import config
from ..interfaces import CameraState, StorageProtocol

logger = logging.getLogger("camera.backend")

#: Commands accepted by the worker queue.
_COMMAND_STOP = "stop"
_COMMAND_MODE = "set_mode"
_COMMAND_VIDEO_MODE = "set_video_mode"
_COMMAND_PHOTO = "photo"
_COMMAND_START_RECORDING = "start_recording"
_COMMAND_STOP_RECORDING = "stop_recording"
_COMMAND_RETRY = "retry"

def _minimal_noise_reduction() -> Any:
    """Hardware-only denoise; the software colour denoise is unusable on ARMv6.

    ``create_video_configuration`` defaults to NoiseReductionMode "Fast",
    which runs a software colour-denoise pass on the CPU.  On a Pi Zero that
    drops 1080p to a few frames per second and starves the encoder.  "Minimal"
    keeps only the ISP's hardware spatial denoise (full pipeline rate).
    """
    try:
        from libcamera import controls as libcamera_controls

        return libcamera_controls.draft.NoiseReductionModeEnum.Minimal
    except Exception:
        logger.debug("libcamera NoiseReductionMode unavailable", exc_info=True)
        return 3  # NoiseReductionModeEnum.Minimal


_NATIVE_YUV: Any = None
_NATIVE_YUV_TRIED = False


def _native_yuv_function() -> Any:
    """Load the optional C I420->RGB converter compiled on first run."""
    global _NATIVE_YUV, _NATIVE_YUV_TRIED
    if _NATIVE_YUV_TRIED:
        return _NATIVE_YUV
    _NATIVE_YUV_TRIED = True
    candidates = [
        os.path.join(os.path.expanduser("~"), ".cache", "camera", "libyuv420.so"),
        "/opt/zerosnap/lib/libyuv420.so",
    ]
    for path in candidates:
        try:
            library = ctypes.CDLL(path)
            function = library.yuv420_to_rgb
        except (OSError, AttributeError):
            continue
        function.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
        ]
        function.restype = None
        _NATIVE_YUV = function
        logger.info("native YUV conversion enabled: %s", path)
        return function
    logger.info("native YUV conversion not available; using numpy")
    return None


#: Preview stream formats, best first.  Picamera2's own PIL helper maps
#: "BGR888" to RGB and "RGB888" to BGR, i.e. the *memory order* is the
#: opposite of the name.  BGR888 therefore gives us RGB bytes for free.
_PREVIEW_FORMATS = ("BGR888", "RGB888", "XRGB8888", "YUV420")


class Picamera2Service:
    """CameraProtocol implementation that drives Picamera2 in a worker thread.

    ``storage`` provides the media paths and fsync helper; it must implement
    :class:`~app.interfaces.StorageProtocol`.
    """

    def __init__(self, storage: StorageProtocol) -> None:
        self._storage = storage

        # Shared state: everything in here is guarded by ``_lock``.
        self._lock = threading.Lock()
        self._state = CameraState()
        self._preview: np.ndarray | None = None

        # Command queue / worker lifecycle.
        self._commands: queue.Queue[tuple[str, object]] = queue.Queue()
        self._stop_flag = threading.Event()
        self._thread: threading.Thread | None = None

        # The fields below are only ever touched by the worker thread.
        self._cam: Any = None
        self._encoder_cls: Any = None
        self._output_cls: Any = None
        self._configured_mode: str | None = None
        self._configured_video_mode: str | None = None
        self._preview_format = _PREVIEW_FORMATS[0]
        self._preview_size: tuple[int, int] = config.PREVIEW_SIZE
        self._encoder: Any = None
        self._output: Any = None
        self._record_path: str | None = None
        self._record_started = 0.0
        self._record_pending = False
        self._preview_errors = 0
        self._fps_frames = 0
        self._fps_since = time.monotonic()
        self._fps_log_at = time.monotonic()
        self._cap_ms_sum = 0.0
        self._conv_ms_sum = 0.0
        self._timing_frames = 0
        self._last_free_check = 0.0

    # ------------------------------------------------------------------
    # Public API (non-blocking)
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start the worker thread; returns immediately."""
        try:
            if self._thread is not None and self._thread.is_alive():
                return
            # Drop commands left over from a previous run (e.g. a stale stop).
            while True:
                try:
                    self._commands.get_nowait()
                except queue.Empty:
                    break
            self._stop_flag.clear()
            self._thread = threading.Thread(
                target=self._run, name="camera-worker", daemon=True
            )
            self._thread.start()
        except Exception:
            logger.exception("cannot start the camera worker thread")
            self._set_error("cannot start the camera worker")

    def stop(self) -> None:
        """Ask the worker to finish and wait a bounded amount of time."""
        try:
            self._stop_flag.set()
            self._enqueue(_COMMAND_STOP)
            thread = self._thread
            if thread is not None and thread.is_alive():
                thread.join(timeout=5.0)
                if thread.is_alive():
                    logger.warning("camera worker did not stop within 5 s")
            self._thread = None
        except Exception:
            logger.exception("cannot stop the camera worker thread")

    def get_state(self) -> CameraState:
        """Return a copy of the current state."""
        try:
            with self._lock:
                return replace(self._state)
        except Exception:
            logger.exception("cannot read the camera state")
            return CameraState()

    def get_preview(self) -> np.ndarray | None:
        """Return the newest preview frame (uint8 HxWx3 RGB) or None."""
        try:
            with self._lock:
                return self._preview
        except Exception:
            logger.exception("cannot read the preview frame")
            return None

    def set_mode(self, mode: str) -> None:
        """Switch between ``"photo"`` and ``"video"`` (rebuilds the config)."""
        try:
            if mode not in ("photo", "video"):
                logger.warning("ignoring invalid camera mode %r", mode)
                return
            with self._lock:
                if self._state.mode == mode:
                    return
                self._state.mode = mode
            self._enqueue(_COMMAND_MODE, mode)
        except Exception:
            logger.exception("set_mode(%r) failed", mode)

    def set_video_mode(self, mode: str) -> None:
        """Select a video mode; rejected while a recording is running."""
        try:
            if mode not in config.VIDEO_MODES:
                logger.warning("ignoring unknown video mode %r", mode)
                return
            with self._lock:
                if self._state.recording:
                    logger.info("cannot change video mode while recording")
                    return
                if self._record_pending:
                    logger.info("cannot change video mode while recording starts")
                    return
                if self._state.video_mode == mode:
                    return
                self._state.video_mode = mode
            self._enqueue(_COMMAND_VIDEO_MODE, mode)
        except Exception:
            logger.exception("set_video_mode(%r) failed", mode)

    def request_photo(self) -> None:
        """Queue a still capture; returns immediately."""
        try:
            with self._lock:
                if self._state.recording or self._record_pending:
                    logger.info("cannot take a photo while recording")
                    return
                if self._state.mode != "photo":
                    logger.info("photo requested outside photo mode; ignored")
                    return
                if self._state.photo_busy:
                    logger.info("a photo is already being captured")
                    return
                self._state.photo_busy = True
            self._enqueue(_COMMAND_PHOTO)
        except Exception:
            logger.exception("request_photo failed")
            self._set_photo_busy(False)

    def start_recording(self) -> None:
        """Queue a recording start; requires video mode and an idle camera."""
        try:
            with self._lock:
                if self._state.recording or self._record_pending:
                    return
                if self._state.mode != "video":
                    logger.info("recording requires video mode; ignored")
                    return
                if not self._state.available:
                    logger.info("camera unavailable; recording ignored")
                    return
                self._record_pending = True
            self._enqueue(_COMMAND_START_RECORDING)
        except Exception:
            logger.exception("start_recording failed")
            with self._lock:
                self._record_pending = False

    def stop_recording(self) -> None:
        """Queue a recording stop; returns immediately."""
        try:
            with self._lock:
                if not self._state.recording:
                    return
            self._enqueue(_COMMAND_STOP_RECORDING)
        except Exception:
            logger.exception("stop_recording failed")

    def retry(self) -> None:
        """Clear the error and ask the worker to reopen the camera."""
        try:
            with self._lock:
                self._state.error = None
                self._state.available = False
                self._state.photo_busy = False
                self._state.recording = False
                self._record_pending = False
                self._state.fps = 0.0
            if self._thread is None or not self._thread.is_alive():
                self.start()
                return
            self._enqueue(_COMMAND_RETRY)
        except Exception:
            logger.exception("retry failed")

    # ------------------------------------------------------------------
    # Worker loop
    # ------------------------------------------------------------------

    def _run(self) -> None:
        logger.info("camera worker starting")
        try:
            if self._open_camera():
                try:
                    self._configure()
                except Exception:
                    logger.exception("initial camera configuration failed")
                    self._handle_fatal("camera configuration failed")

            while not self._stop_flag.is_set():
                try:
                    self._drain_commands()
                    if self._stop_flag.is_set():
                        break
                    self._tick()
                except Exception:
                    logger.exception("camera worker loop error")
                    self._handle_fatal("camera worker error")
                    self._stop_flag.wait(0.2)
        finally:
            self._shutdown()

    def _drain_commands(self) -> None:
        for _ in range(32):
            try:
                name, arg = self._commands.get_nowait()
            except queue.Empty:
                return
            try:
                self._handle_command(name, arg)
            except Exception:
                logger.exception("camera command %r failed", name)
                self._handle_fatal(f"camera command {name} failed")

    def _handle_command(self, name: str, arg: object) -> None:
        if name == _COMMAND_STOP:
            self._stop_flag.set()
        elif name == _COMMAND_MODE:
            self._apply_mode(str(arg))
        elif name == _COMMAND_VIDEO_MODE:
            self._apply_video_mode(str(arg))
        elif name == _COMMAND_PHOTO:
            self._capture_photo()
        elif name == _COMMAND_START_RECORDING:
            self._do_start_recording()
        elif name == _COMMAND_STOP_RECORDING:
            self._do_stop_recording()
        elif name == _COMMAND_RETRY:
            self._do_retry()
        else:
            logger.warning("unknown camera command %r", name)

    def _tick(self) -> None:
        if self._cam is None:
            # Idle until retry() or a mode command opens the camera again.
            self._stop_flag.wait(0.05)
            return

        if self._record_path is not None:
            self._update_recording_stats()
            # Do not consume camera requests while recording.  Picamera2
            # hands each completed request to exactly one consumer, so a
            # preview grab here would directly steal frames from the encoder
            # (which is why the MP4 was choppy).  The UI keeps showing the
            # last frame with the REC overlay instead.
        else:
            self._grab_preview()
        self._update_free_bytes()

    # ------------------------------------------------------------------
    # Camera lifecycle
    # ------------------------------------------------------------------

    def _open_camera(self) -> bool:
        """Open Picamera2 (lazy import) and remember the sensor size."""
        if self._cam is not None:
            return True
        try:
            from picamera2 import Picamera2
            from picamera2.encoders import H264Encoder
            from picamera2.outputs import FfmpegOutput
        except Exception as exc:
            logger.exception("cannot import picamera2")
            self._set_error(f"picamera2 unavailable: {exc}")
            return False

        try:
            cam = Picamera2()
            sensor = self._read_sensor_size(cam)
            self._cam = cam
            self._encoder_cls = H264Encoder
            self._output_cls = FfmpegOutput
            with self._lock:
                self._state.sensor_max = sensor
            logger.info("camera opened, sensor %dx%d", sensor[0], sensor[1])
            return True
        except Exception as exc:
            logger.exception("cannot open the camera")
            self._set_error(f"cannot open the camera: {exc}")
            return False

    @staticmethod
    def _read_sensor_size(cam: Any) -> tuple[int, int]:
        """Best-effort sensor resolution, defaulting to the OV5647 size."""
        try:
            size = tuple(int(value) for value in cam.sensor_resolution)
            if len(size) == 2 and size[0] > 0 and size[1] > 0:
                return (size[0], size[1])
        except Exception:
            logger.debug("sensor_resolution is unavailable", exc_info=True)
        try:
            size = tuple(
                int(value) for value in cam.camera_properties["PixelArraySize"]
            )
            if len(size) == 2 and size[0] > 0 and size[1] > 0:
                return (size[0], size[1])
        except Exception:
            logger.debug("PixelArraySize is unavailable", exc_info=True)
        return (2592, 1944)

    def _close_camera(self) -> None:
        cam = self._cam
        self._cam = None
        self._configured_mode = None
        self._configured_video_mode = None
        if cam is None:
            return
        try:
            cam.stop()
        except Exception:
            logger.debug("stopping the camera during close failed", exc_info=True)
        try:
            cam.close()
        except Exception:
            logger.exception("closing the camera failed")

    def _do_retry(self) -> None:
        logger.info("reopening the camera after an error")
        self._abort_recording()
        self._close_camera()
        with self._lock:
            self._state.error = None
            self._state.available = False
            self._state.photo_busy = False
            self._state.recording = False
            self._record_pending = False
            self._state.fps = 0.0
            self._state.dropped_frames = 0
        if not self._open_camera():
            return
        try:
            self._configure()
        except Exception:
            logger.exception("camera configuration after retry failed")
            self._handle_fatal("camera configuration failed")

    def _configure(self) -> None:
        """(Re)build the preview configuration for the current mode."""
        if self._cam is None:
            raise RuntimeError("camera is not open")

        with self._lock:
            mode = self._state.mode
            video_mode = self._state.video_mode
        if mode not in ("photo", "video"):
            mode = "photo"
        if video_mode not in config.VIDEO_MODES:
            video_mode = config.DEFAULT_VIDEO_MODE

        try:
            self._cam.stop()
        except Exception:
            logger.debug("stopping the camera before configure failed", exc_info=True)

        last_error: Exception | None = None
        sensor_modes: list[dict[str, Any] | None] = [None]
        if mode == "photo":
            preview_sensor = self._pick_preview_sensor_mode()
            if preview_sensor is not None:
                sensor_modes = [preview_sensor, None]

        # On VC4 the lores stream must be YUV, so video mode can only use the
        # YUV conversion path; photo mode can use a packed RGB format.
        formats = _PREVIEW_FORMATS if mode == "photo" else ("YUV420",)

        for sensor_mode in sensor_modes:
            for preview_format in formats:
                try:
                    cam_config = self._build_config(
                        mode, video_mode, preview_format, sensor_mode
                    )
                    self._cam.configure(cam_config)
                    self._cam.start()
                except Exception as exc:
                    last_error = exc
                    logger.warning(
                        "camera rejected preview format %s (sensor=%s): %s",
                        preview_format,
                        sensor_mode,
                        exc,
                    )
                    try:
                        self._cam.stop()
                    except Exception:
                        logger.debug("camera stop after failed configure", exc_info=True)
                    continue

                self._preview_format = self._actual_preview_format(mode, preview_format)
                self._configured_mode = mode
                self._configured_video_mode = video_mode
                self._preview_size = self._actual_preview_size(mode, video_mode)
                self._preview_errors = 0
                self._fps_frames = 0
                self._fps_since = time.monotonic()
                with self._lock:
                    self._preview = None
                    self._state.available = True
                    self._state.error = None
                    self._state.fps = 0.0
                    self._state.dropped_frames = 0
                logger.info(
                    "camera configured: mode=%s video=%s preview=%s sensor=%s",
                    mode,
                    video_mode,
                    self._preview_format,
                    sensor_mode,
                )
                return

        raise RuntimeError(f"no usable camera format: {last_error}")

    def _actual_preview_format(self, mode: str, requested: str) -> str:
        """Read back the format libcamera really chose for the preview stream.

        libcamera may adjust a configuration instead of rejecting it; using
        the effective format avoids decoding the buffer with the wrong channel
        order (which is what made the colours look inverted before).
        """
        try:
            config = self._cam.camera_config
            stream = "main" if mode == "photo" else "lores"
            actual = str(config[stream]["format"])
        except Exception:
            logger.debug("cannot read back the preview format", exc_info=True)
            return requested
        if actual != requested:
            logger.info("preview format adjusted: %s -> %s", requested, actual)
        return actual

    def _actual_preview_size(self, mode: str, video_mode: str) -> tuple[int, int]:
        """Effective preview size (libcamera may align it to an even size)."""
        try:
            cam_config = self._cam.camera_config
            stream = "main" if mode == "photo" else "lores"
            width, height = (int(v) for v in cam_config[stream]["size"])
            if width > 0 and height > 0:
                return (width, height)
        except Exception:
            logger.debug("cannot read back the preview size", exc_info=True)
        return config.PREVIEW_SIZE if mode == "photo" else self._lores_size(video_mode)

    def _pick_preview_sensor_mode(self) -> dict[str, Any] | None:
        """Choose a small, fast sensor mode for the live preview.

        Asking for a 320x240 preview while the sensor runs its 5 MP mode makes
        the ISP downscale by 8x every frame, which is far too slow on a Pi
        Zero.  640x480 at ~59 fps (or the smallest 4:3 mode above 30 fps) keeps
        the live view smooth; photos switch back to the full still config.
        """
        cam = self._cam
        if cam is None:
            return None
        try:
            modes = list(cam.sensor_modes)
        except Exception:
            logger.debug("sensor_modes is unavailable", exc_info=True)
            return None

        best: dict[str, Any] | None = None
        best_area = 0
        for mode in modes:
            try:
                width, height = (int(v) for v in mode["output_size"])
                fps = float(mode.get("fps") or 0.0)
            except (KeyError, TypeError, ValueError):
                continue
            if width <= 0 or height <= 0 or fps < 25.0:
                continue
            if abs(width / float(height) - 4.0 / 3.0) > 0.06:
                continue
            area = width * height
            if best is None or area < best_area:
                best = mode
                best_area = area
        if best is None:
            return None
        return {
            "output_size": tuple(int(v) for v in best["output_size"]),
            "bit_depth": int(best.get("bit_depth") or 10),
        }

    def _build_config(
        self,
        mode: str,
        video_mode: str,
        fmt: str,
        sensor_mode: dict[str, Any] | None = None,
    ) -> Any:
        """Build the picamera2 configuration for one mode / preview format."""
        cam = self._cam
        if mode == "photo":
            kwargs: dict[str, Any] = {
                "main": {"size": config.PREVIEW_SIZE, "format": fmt},
                "buffer_count": 2,
                "controls": {"FrameRate": config.PREVIEW_FPS},
                # No raw stream: it wastes ISP bandwidth and memory, and the
                # preview/JPEG only needs the processed output.
                "raw": None,
            }
            if sensor_mode is not None:
                kwargs["sensor"] = sensor_mode
            return cam.create_preview_configuration(**kwargs)
        width, height, fps, _bitrate = config.VIDEO_MODES[video_mode]
        return cam.create_video_configuration(
            main={"size": (width, height), "format": "YUV420"},
            lores={"size": self._lores_size(video_mode), "format": fmt},
            controls={
                "FrameRate": fps,
                # Avoid the software colour denoise (the Pi Zero cannot run it).
                "NoiseReductionMode": _minimal_noise_reduction(),
            },
            raw=None,
        )

    @staticmethod
    def _lores_size(video_mode: str) -> tuple[int, int]:
        """Preview size that matches the recording aspect, already screen sized.

        The UI blits it directly, so no per-frame scaling is needed.
        """
        width, height, _fps, _bitrate = config.VIDEO_MODES.get(
            video_mode, config.VIDEO_MODES[config.DEFAULT_VIDEO_MODE]
        )
        if width * 3 == height * 4:
            return (config.PREVIEW_SIZE[0], config.PREVIEW_SIZE[1])
        return (config.PREVIEW_SIZE[0], config.PREVIEW_SIZE[0] * 9 // 16)

    def _apply_mode(self, mode: str) -> None:
        with self._lock:
            desired = self._state.mode
            recording = self._state.recording
        if mode != desired:
            logger.info(
                "mode request %s superseded by state mode %s; using state",
                mode,
                desired,
            )
        if recording:
            self._do_stop_recording()
        if self._cam is None:
            if not self._open_camera():
                return
        try:
            self._configure()
        except Exception:
            logger.exception("cannot reconfigure the camera for mode %s", desired)
            self._handle_fatal("camera configuration failed")

    def _apply_video_mode(self, mode: str) -> None:
        with self._lock:
            recording = self._state.recording
            desired = self._state.video_mode
            current_mode = self._state.mode
        if recording:
            logger.info("video mode change rejected while recording")
            with self._lock:
                if self._configured_video_mode is not None:
                    self._state.video_mode = self._configured_video_mode
            return
        if mode != desired:
            logger.info(
                "video mode request %s superseded by state mode %s; using state",
                mode,
                desired,
            )
        if current_mode != "video" or self._cam is None:
            return
        try:
            self._configure()
        except Exception:
            logger.exception("cannot reconfigure the camera for video mode %s", desired)
            self._handle_fatal("camera configuration failed")

    # ------------------------------------------------------------------
    # Preview
    # ------------------------------------------------------------------

    def _grab_preview(self) -> None:
        if self._cam is None or self._configured_mode is None:
            return
        stream = "lores" if self._configured_mode == "video" else "main"
        t0 = time.perf_counter()
        try:
            array = self._cam.capture_array(stream)
            t1 = time.perf_counter()
        except Exception:
            self._preview_errors += 1
            with self._lock:
                self._state.dropped_frames += 1
            if self._preview_errors == 1:
                logger.exception("preview capture failed")
            if self._preview_errors >= 3:
                self._handle_fatal("preview capture failed")
            return

        frame = self._to_rgb(array, self._preview_format, self._preview_size)
        t2 = time.perf_counter()
        self._cap_ms_sum += (t1 - t0) * 1000.0
        self._conv_ms_sum += (t2 - t1) * 1000.0
        self._timing_frames += 1
        if frame is None:
            self._preview_errors += 1
            return

        self._preview_errors = 0
        with self._lock:
            self._preview = frame

        now = time.monotonic()
        self._fps_frames += 1
        elapsed = now - self._fps_since
        if elapsed >= 1.0:
            fps = self._fps_frames / elapsed
            self._fps_frames = 0
            self._fps_since = now
            with self._lock:
                self._state.fps = round(fps, 1)
            if now - self._fps_log_at >= 10.0:
                self._fps_log_at = now
                frames = max(1, self._timing_frames)
                logger.info(
                    "preview: %.1f fps, format=%s, size=%dx%d, capture=%.1f ms, convert=%.1f ms",
                    fps,
                    self._preview_format,
                    self._preview_size[0],
                    self._preview_size[1],
                    self._cap_ms_sum / frames,
                    self._conv_ms_sum / frames,
                )
                self._cap_ms_sum = 0.0
                self._conv_ms_sum = 0.0
                self._timing_frames = 0

    @classmethod
    def _to_rgb(
        cls, array: Any, fmt: str, size: tuple[int, int]
    ) -> np.ndarray | None:
        """Convert a picamera2 array to a contiguous uint8 HxWx3 RGB frame."""
        try:
            data = np.asarray(array)
            if fmt == "BGR888":
                # Picamera2 stores BGR888 as R,G,B in memory (its PIL helper
                # maps this format to mode "RGB"), so no swap is needed.
                return cls._packed_rgb_to_frame(data, size, swap_rb=False)
            if fmt == "XBGR8888":
                # R,G,B,X in memory; keep the first three bytes.
                return cls._packed_rgb_to_frame(data, size, swap_rb=False)
            if fmt == "RGB888":
                # The opposite is true for RGB888: memory order is B,G,R.
                return cls._packed_rgb_to_frame(data, size, swap_rb=True)
            if fmt in ("XRGB8888", "ARGB8888"):
                return cls._xrgb8888_to_frame(data, size)
            return cls._yuv420_to_rgb(data, size)
        except Exception:
            logger.exception("cannot convert preview frame (format %s)", fmt)
            return None

    @staticmethod
    def _packed_rgb_to_frame(
        data: np.ndarray, size: tuple[int, int], swap_rb: bool
    ) -> np.ndarray | None:
        """Turn a packed 24-bit buffer into an HxWx3 uint8 RGB array."""
        width, height = size
        if (
            data.ndim == 3
            and data.shape[2] >= 3
            and data.shape[0] >= height
            and data.shape[1] >= width
        ):
            frame = data[:height, :width, :3]
        elif data.ndim == 2:
            flat = data.reshape(-1)
            needed = width * height * 3
            if flat.size < needed:
                return None
            frame = flat[:needed].reshape(height, width, 3)
        else:
            logger.warning("unexpected packed RGB array shape %s", data.shape)
            return None
        if swap_rb:
            frame = frame[:, :, ::-1]
        return np.ascontiguousarray(frame, dtype=np.uint8)

    @staticmethod
    def _xrgb8888_to_frame(data: np.ndarray, size: tuple[int, int]) -> np.ndarray | None:
        """XRGB8888 is B,G,R,X in memory; keep R,G,B and drop the pad byte."""
        width, height = size
        if (
            data.ndim != 3
            or data.shape[2] < 3
            or data.shape[0] < height
            or data.shape[1] < width
        ):
            logger.warning("unexpected XRGB8888 array shape %s", data.shape)
            return None
        frame = data[:height, :width, 2::-1]
        return np.ascontiguousarray(frame, dtype=np.uint8)

    @classmethod
    def _yuv420_to_rgb(cls, data: np.ndarray, size: tuple[int, int]) -> np.ndarray | None:
        """BT.601 limited-range conversion of a YUV420 (I420) buffer."""
        width, height = size
        if width <= 0 or height <= 0:
            return None
        if data.ndim == 3 and data.shape[2] == 1:
            data = data[:, :, 0]
        if data.ndim == 3 and data.shape[2] == 3 and data.shape[0] >= height:
            # Some pipelines hand back an already unpacked 3-channel image.
            frame = data[:height, :width, :]
            return np.ascontiguousarray(frame.astype(np.uint8, copy=False))
        if data.ndim != 2:
            logger.warning("unexpected YUV420 array shape %s", data.shape)
            return None

        stride = int(data.shape[1])
        if stride < width or data.shape[0] < height:
            logger.warning("YUV420 buffer too small: %s", data.shape)
            return None

        native = _native_yuv_function()
        if native is not None and width % 2 == 0 and height % 2 == 0:
            try:
                source = np.ascontiguousarray(data)
                output = np.empty((height, width, 3), dtype=np.uint8)
                native(
                    source.ctypes.data,
                    int(width),
                    int(height),
                    int(stride),
                    output.ctypes.data,
                )
                return output
            except Exception:
                logger.exception("native YUV conversion failed; using numpy")

        y_plane = data[:height, :width]
        chroma_h = height // 2
        chroma_w = width // 2
        flat = data.reshape(-1)
        start = height * stride
        chroma_stride = stride // 2 if stride != width else chroma_w
        chroma_bytes = chroma_h * chroma_stride
        if flat.size < start + 2 * chroma_bytes:
            logger.warning("YUV420 buffer too small for chroma")
            return None
        # I420: the U plane comes first, then the V plane; each chroma row may
        # be padded to half of the luma stride.
        u_plane = flat[start : start + chroma_bytes].reshape(
            chroma_h, chroma_stride
        )[:, :chroma_w]
        v_plane = flat[start + chroma_bytes : start + 2 * chroma_bytes].reshape(
            chroma_h, chroma_stride
        )[:, :chroma_w]

        if chroma_h * 2 == height and chroma_w * 2 == width:
            # Usual 4:2:0 case: a plain 2x nearest upsample in C.
            u_plane = np.repeat(np.repeat(u_plane, 2, axis=0), 2, axis=1)
            v_plane = np.repeat(np.repeat(v_plane, 2, axis=0), 2, axis=1)
        else:
            u_plane = cls._nearest_resize(u_plane, height, width)
            v_plane = cls._nearest_resize(v_plane, height, width)

        # Integer BT.601 limited-range conversion (fixed point, x64).  Float32
        # arrays were far too slow on the Pi Zero's ARMv6.
        c = (y_plane.astype(np.int16) - 16) * 74
        d = u_plane.astype(np.int16) - 128
        e = v_plane.astype(np.int16) - 128
        red = (c + 102 * e) >> 6
        green = (c - 25 * d - 52 * e) >> 6
        blue = (c + 129 * d) >> 6
        rgb = np.stack((red, green, blue), axis=-1)
        np.clip(rgb, 0, 255, out=rgb)
        return np.ascontiguousarray(rgb.astype(np.uint8))

    @staticmethod
    def _nearest_resize(plane: np.ndarray, height: int, width: int) -> np.ndarray:
        """Nearest-neighbour upsample that tolerates odd source sizes."""
        src_h, src_w = plane.shape[:2]
        if src_h == height and src_w == width:
            return plane
        if src_h <= 0 or src_w <= 0:
            return np.zeros((height, width), dtype=plane.dtype)
        rows = np.minimum((np.arange(height) * src_h) // height, src_h - 1)
        cols = np.minimum((np.arange(width) * src_w) // width, src_w - 1)
        return plane[rows][:, cols]

    # ------------------------------------------------------------------
    # Photos
    # ------------------------------------------------------------------

    def _capture_photo(self) -> None:
        if self._cam is None or self._configured_mode != "photo":
            logger.info("photo request ignored: camera is not in photo mode")
            self._set_photo_busy(False)
            return

        path: str | None = None
        tmp_path: str | None = None
        try:
            path = self._storage.next_photo_path()
            directory = os.path.dirname(path) or "."
            os.makedirs(directory, exist_ok=True)
            # Keep the .jpg suffix so picamera2 saves JPEG into the temp file.
            tmp_path = os.path.join(directory, f".{os.path.basename(path)}.tmp.jpg")

            still_config = self._cam.create_still_configuration(
                raw=None,
                controls={"NoiseReductionMode": _minimal_noise_reduction()},
            )
            self._cam.switch_mode_and_capture_file(still_config, tmp_path)
            os.replace(tmp_path, path)

            self._storage.fsync_file(path)
            size = self._file_size(path)
            with self._lock:
                self._state.last_photo = path
                self._state.photo_busy = False
                self._state.error = None
            logger.info("photo saved: %s (%d bytes)", path, size)
            self._update_free_bytes(force=True)
        except Exception as exc:
            logger.exception("photo capture failed")
            message = f"photo capture failed: {exc}"
            self._set_error(message)
            if self._recover_preview_after_photo():
                # Keep the message so the UI can show a toast, but the preview
                # recovered so the service stays usable without a retry.
                with self._lock:
                    self._state.error = message
            self._set_photo_busy(False)
        finally:
            if tmp_path is not None:
                try:
                    if os.path.exists(tmp_path):
                        os.unlink(tmp_path)
                except OSError:
                    logger.warning("cannot remove temporary photo %s", tmp_path)

    def _recover_preview_after_photo(self) -> bool:
        """Try to return to a usable preview after a failed still capture."""
        if self._cam is None:
            return False
        try:
            self._configure()
            return True
        except Exception:
            logger.exception("cannot restore the preview after a failed photo")
            self._handle_fatal("camera preview recovery failed")
            return False

    def _set_photo_busy(self, busy: bool) -> None:
        with self._lock:
            self._state.photo_busy = busy

    # ------------------------------------------------------------------
    # Video recording
    # ------------------------------------------------------------------

    def _do_start_recording(self) -> None:
        with self._lock:
            self._record_pending = False
            recording = self._state.recording
            mode = self._state.mode
            video_mode = self._state.video_mode
        if recording:
            logger.info("recording already running; start ignored")
            return
        if mode != "video":
            logger.info("recording requires video mode; start ignored")
            return
        if video_mode not in config.VIDEO_MODES:
            video_mode = config.DEFAULT_VIDEO_MODE

        if self._cam is None:
            if not self._open_camera():
                return
        if self._configured_mode != "video" or self._configured_video_mode != video_mode:
            try:
                self._configure()
            except Exception:
                logger.exception("cannot configure the camera for recording")
                self._handle_fatal("camera configuration failed")
                return

        path: str | None = None
        encoder: Any = None
        output: Any = None
        try:
            path = self._storage.next_video_path()
            directory = os.path.dirname(path) or "."
            os.makedirs(directory, exist_ok=True)
            _width, _height, _fps, bitrate = config.VIDEO_MODES[video_mode]
            encoder = self._encoder_cls(bitrate=bitrate)
            output = self._output_cls(path)
            self._cam.start_recording(encoder, output)
            self._encoder = encoder
            self._output = output
            self._record_path = path
            self._record_started = time.monotonic()
            self._last_free_check = self._record_started
            with self._lock:
                self._state.recording = True
                self._state.recording_seconds = 0.0
                self._state.recording_size = 0
                self._state.error = None
            logger.info("recording started: %s (%s)", path, video_mode)
        except Exception as exc:
            logger.exception("cannot start recording")
            self._encoder = None
            self._output = None
            self._record_path = None
            self._abort_encoder(encoder, output)
            self._set_error(f"cannot start recording: {exc}")
            self._handle_fatal("camera recording start failed")

    def _update_recording_stats(self) -> None:
        path = self._record_path
        if path is None:
            return
        started = self._record_started
        now = time.monotonic()
        size = self._file_size(path)
        with self._lock:
            if self._state.recording:
                self._state.recording_seconds = round(now - started, 2)
                self._state.recording_size = size

        if size >= config.MAX_VIDEO_BYTES:
            logger.info("stopping recording: size limit reached (%d bytes)", size)
            self._do_stop_recording()
            return

        if now - self._last_free_check >= 1.0:
            self._last_free_check = now
            try:
                free = int(self._storage.free_bytes())
            except Exception:
                logger.exception("cannot read free disk space")
                return
            with self._lock:
                self._state.free_bytes = max(0, free)
            if free < config.MIN_FREE_BYTES:
                logger.info("stopping recording: low disk space (%d bytes)", free)
                self._do_stop_recording()

    def _do_stop_recording(self, restart: bool = True) -> None:
        path = self._record_path
        started = self._record_started
        if path is None:
            with self._lock:
                self._state.recording = False
                self._record_pending = False
            return

        logger.info("stopping recording: %s", path)
        stopped_cleanly = False
        if self._cam is not None:
            try:
                self._cam.stop_recording()
                stopped_cleanly = True
            except Exception:
                logger.exception("camera stop_recording failed")

        encoder, output = self._encoder, self._output
        self._encoder = None
        self._output = None
        self._record_path = None
        if not stopped_cleanly:
            self._abort_encoder(encoder, output)

        size = self._file_size(path)
        try:
            self._storage.fsync_file(path)
        except Exception:
            logger.exception("cannot fsync video file %s", path)

        elapsed = max(0.0, time.monotonic() - started)
        with self._lock:
            self._state.recording = False
            self._record_pending = False
            self._state.recording_size = size
            self._state.recording_seconds = round(elapsed, 2)
            self._state.last_video = path
        logger.info("recording saved: %s (%d bytes, %.1f s)", path, size, elapsed)
        self._update_free_bytes(force=True)

        if not restart:
            return
        if self._cam is None:
            return
        try:
            self._configure()
        except Exception:
            logger.exception("cannot restore the preview after recording")
            self._handle_fatal("camera preview recovery failed")

    def _abort_recording(self) -> None:
        """Best-effort finalisation used when the camera fails or on shutdown."""
        path = self._record_path
        encoder, output = self._encoder, self._output
        self._encoder = None
        self._output = None
        self._record_path = None
        cam = self._cam
        if path is not None and cam is not None:
            try:
                cam.stop_recording()
            except Exception:
                logger.debug("stop_recording during abort failed", exc_info=True)
                self._abort_encoder(encoder, output)
        else:
            self._abort_encoder(encoder, output)
        if path is not None:
            try:
                self._storage.fsync_file(path)
            except Exception:
                logger.debug("fsync during abort failed", exc_info=True)
        with self._lock:
            self._state.recording = False
            if path is not None:
                self._state.last_video = path
                self._state.recording_size = self._file_size(path)

    @staticmethod
    def _abort_encoder(encoder: Any, output: Any) -> None:
        if encoder is not None:
            try:
                encoder.stop()
            except Exception:
                logger.debug("stopping the encoder failed", exc_info=True)
        if output is not None:
            try:
                output.stop()
            except Exception:
                logger.debug("stopping the ffmpeg output failed", exc_info=True)

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _handle_fatal(self, message: str) -> None:
        """Finalise anything in flight, report the error and close the camera."""
        try:
            if self._record_path is not None:
                self._abort_recording()
        except Exception:
            logger.exception("cannot finalise the recording after an error")
        self._set_error(message)
        self._close_camera()

    def _set_error(self, message: str) -> None:
        logger.error("camera error: %s", message)
        with self._lock:
            self._state.available = False
            self._state.error = message
            self._state.photo_busy = False
            self._state.recording = False
            self._record_pending = False
            self._state.fps = 0.0
            self._preview = None

    def _enqueue(self, name: str, arg: object = None) -> None:
        try:
            self._commands.put_nowait((name, arg))
        except Exception:
            logger.exception("cannot enqueue camera command %s", name)

    def _update_free_bytes(self, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self._last_free_check < 2.0:
            return
        self._last_free_check = now
        try:
            free = int(self._storage.free_bytes())
        except Exception:
            logger.exception("cannot read free disk space")
            return
        with self._lock:
            self._state.free_bytes = max(0, free)

    @staticmethod
    def _file_size(path: str) -> int:
        try:
            return os.path.getsize(path)
        except OSError:
            return 0

    def _shutdown(self) -> None:
        logger.info("camera worker stopping")
        try:
            if self._record_path is not None:
                self._do_stop_recording(restart=False)
        except Exception:
            logger.exception("cannot finalise the recording during shutdown")
        self._close_camera()
        with self._lock:
            self._state.available = False
            self._state.recording = False
            self._record_pending = False
            self._state.photo_busy = False
            self._state.fps = 0.0
            self._preview = None
        logger.info("camera worker stopped")
