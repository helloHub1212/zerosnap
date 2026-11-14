"""camera_shm.py - Python side of the C/Python shared memory contract.

The layout is *not* duplicated by hand.  A ``ctypes.Structure`` mirrors
``src/cam.h`` and every byte offset used below is derived from it at import
time, so if ``cam.h`` changes this module refuses to run instead of silently
publishing frames into the wrong place.

Typical daemon use::

    shm = CameraShm(create=True)
    shm.publish_state(mode=CAM_MODE_VIDEO, recording=1)
    shm.publish_frame(rgb565_bytes)       # 320x240 RGB565, 153600 bytes
    command = shm.poll_command()          # (cmd, arg) or None

Typical test / UI use::

    shm = CameraShm(create=False)         # raises FileNotFoundError if absent
    shm.wait_ready()
    state = shm.read_state()

Only the Python standard library is used (ctypes, mmap, struct, os, time).
The small POSIX ``shm_open``/``shm_unlink`` wrappers go through ctypes to
libc, because Python does not expose them in ``os``.

Byte order
----------
"=I" (native byte order, standard 4-byte integer) is used everywhere.  The
target is little-endian ARMv6, and the host tests are little-endian x86, so
this matches the C struct exactly.  The C struct itself is native-endian too.

Ordering note
-------------
The "publish the payload, then bump the sequence" order relies on CPython's
bytecode ordering and on the target being a single-core ARMv6 (the Pi Zero),
where a process sees its own stores in program order and the scheduler
context-switches act as barriers.  There is no portable Python memory fence;
on a multi-core producer/consumer pair that does not periodically sleep, use
the C implementation or keep the existing single-core assumption.
"""

from __future__ import annotations

import ctypes
import errno
import mmap
import os
import struct
import time

# --------------------------------------------------------------------------
# Constants copied from cam.h.  Keep these in sync with that header.
# --------------------------------------------------------------------------

CAM_SHM_NAME = "/zerosnap-shm"
CAM_MAGIC = 0x43414D31
CAM_VERSION = 1

CAM_FRAME_W = 320
CAM_FRAME_H = 240
CAM_FRAME_BYTES = CAM_FRAME_W * CAM_FRAME_H * 2

CAM_MAX_PATH = 256

CAM_CMD_NONE = 0
CAM_CMD_SHUTTER = 1
CAM_CMD_RECORD_START = 2  # reserved, video removed
CAM_CMD_RECORD_STOP = 3  # reserved, video removed
CAM_CMD_SET_MODE_PHOTO = 4
CAM_CMD_SET_MODE_VIDEO = 5
CAM_CMD_SET_VIDEO_MODE = 6
CAM_CMD_REFRESH = 7
CAM_CMD_QUIT = 8

CAM_FLAG_BUSY = 1 << 0
CAM_FLAG_ERROR = 1 << 1
CAM_FLAG_CAM_READY = 1 << 2
CAM_FLAG_SAVING = 1 << 3

CAM_MODE_PHOTO = 0
CAM_MODE_VIDEO = 1

CAM_VIDEO_1080P30 = 0
CAM_VIDEO_720P30 = 1
CAM_VIDEO_480P60 = 2

# The size produced by gcc for the CamShm in src/cam.h.  The tests compare
# this with the size reported by the C side as well; this constant exists so
# an accidental layout change in the Python mirror fails loudly at import.
_EXPECTED_CAM_SHM_SIZE = 308048


# --------------------------------------------------------------------------
# Layout mirror: this is the single source of truth for offsets.
# --------------------------------------------------------------------------

class _CamShmLayout(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        # The frame ring.
        ("frame_seq", ctypes.c_uint32),
        ("frame_w", ctypes.c_uint32),
        ("frame_h", ctypes.c_uint32),
        ("frame_stride_px", ctypes.c_uint32),
        ("frame_pad", ctypes.c_uint32),
        ("frame_data", (ctypes.c_uint8 * CAM_FRAME_BYTES) * 2),
        # State written by the daemon.
        ("state_seq", ctypes.c_uint32),
        ("mode", ctypes.c_uint32),
        ("recording", ctypes.c_uint32),
        ("video_mode", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("rec_seconds", ctypes.c_uint32),
        ("photo_count", ctypes.c_uint32),
        ("video_count", ctypes.c_uint32),
        ("free_mb", ctypes.c_uint32),
        ("battery_pct", ctypes.c_uint32),
        ("last_photo", ctypes.c_char * CAM_MAX_PATH),
        ("last_video", ctypes.c_char * CAM_MAX_PATH),
        ("status_text", ctypes.c_char * CAM_MAX_PATH),
        # UI -> daemon command slot.
        ("cmd_seq", ctypes.c_uint32),
        ("cmd", ctypes.c_uint32),
        ("cmd_arg", ctypes.c_uint32),
    ]


CAM_SHM_SIZE = ctypes.sizeof(_CamShmLayout)

if CAM_SHM_SIZE != _EXPECTED_CAM_SHM_SIZE:
    raise RuntimeError(
        "camera_shm.py layout drift: ctypes.sizeof(CamShm)=%d but cam.h "
        "layout is expected to be %d.  Update camera_shm.py together with "
        "src/cam.h." % (CAM_SHM_SIZE, _EXPECTED_CAM_SHM_SIZE)
    )

# ``descriptor.offset`` is ctypes' precomputed "addressof(field) - base".
# (``getattr(instance, name)`` returns a plain int for scalar fields, so it
# cannot be passed to ctypes.addressof.)
_offsets_by_name = {
    name: getattr(_CamShmLayout, name).offset
    for name, _ctype in _CamShmLayout._fields_
}


class _Offsets:
    """Attribute access to the byte offsets, e.g. ``OFF.state_seq``."""


OFF = _Offsets()
for _name, _value in _offsets_by_name.items():
    setattr(OFF, _name, _value)

del _name, _value

# Self-checks tying the ctypes mirror to the invariants in cam.h.  If any of
# these fail, the Python side must be updated before touching the segment.
if OFF.frame_data != 28:
    raise RuntimeError(
        "camera_shm.py layout drift: frame_data is at %d, expected 28"
        % OFF.frame_data
    )
if OFF.state_seq != OFF.frame_data + 2 * CAM_FRAME_BYTES:
    raise RuntimeError(
        "camera_shm.py layout drift: state_seq is at %d, expected %d"
        % (OFF.state_seq, OFF.frame_data + 2 * CAM_FRAME_BYTES)
    )
if OFF.cmd_seq != CAM_SHM_SIZE - 12:
    raise RuntimeError(
        "camera_shm.py layout drift: cmd_seq is at %d, expected %d"
        % (OFF.cmd_seq, CAM_SHM_SIZE - 12)
    )

_U32 = struct.Struct("=I")  # native byte order, exactly 4 bytes

_U32_FIELDS = (
    "mode",
    "recording",
    "video_mode",
    "flags",
    "rec_seconds",
    "photo_count",
    "video_count",
    "free_mb",
    "battery_pct",
)

_STR_FIELDS = ("last_photo", "last_video", "status_text")


# --------------------------------------------------------------------------
# libc shm wrappers
# --------------------------------------------------------------------------

try:
    _libc = ctypes.CDLL(None, use_errno=True)
    _shm_open = _libc.shm_open
    _shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    _shm_open.restype = ctypes.c_int
    _shm_unlink = _libc.shm_unlink
    _shm_unlink.argtypes = [ctypes.c_char_p]
    _shm_unlink.restype = ctypes.c_int
except (OSError, AttributeError) as _exc:  # pragma: no cover - host only
    raise RuntimeError(
        "camera_shm.py needs libc shm_open/shm_unlink (POSIX shared memory)"
    ) from _exc


def unlink(name: str = CAM_SHM_NAME) -> None:
    """Remove the shared memory object if it exists (test/cleanup helper)."""
    rc = _shm_unlink(name.encode("utf-8"))
    if rc != 0:
        err = ctypes.get_errno()
        if err != errno.ENOENT:
            raise OSError(err, os.strerror(err) + " (%s)" % name)


def open_when_ready(timeout: float | None = 5.0, create: bool = False,
                    name: str = CAM_SHM_NAME) -> "CameraShm":
    """Open the segment, retrying while the daemon creates it.

    Raises ``TimeoutError`` if the segment does not appear (or is not
    stamped with the right header) before ``timeout`` seconds.
    """
    deadline = None if timeout is None else time.monotonic() + timeout
    while True:
        try:
            shm = CameraShm(create=create, name=name)
        except FileNotFoundError:
            shm = None
        if shm is not None and shm.wait_ready(0.0):
            return shm
        if shm is not None:
            shm.close()
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError("camera shm %r was not ready in time" % name)
        time.sleep(0.01)


# --------------------------------------------------------------------------
# Producer / consumer object
# --------------------------------------------------------------------------

class CameraShm:
    """Maps the camera shared memory segment.

    ``create=True`` zeroes the segment and stamps magic/version.  The UI
    side (and consumers) pass ``create=False`` and handle ``FileNotFoundError``
    while the daemon has not started yet.
    """

    def __init__(self, create: bool = False, name: str = CAM_SHM_NAME):
        self.name = name
        self._mm = None
        self._last_cmd_seq = 0
        self._last_state_seq = 0

        flags = os.O_RDWR | (os.O_CREAT if create else 0)
        fd = _shm_open(name.encode("utf-8"), flags, 0o600)
        if fd < 0:
            err = ctypes.get_errno()
            message = "%s (%s)" % (os.strerror(err), name)
            if err == errno.ENOENT:
                raise FileNotFoundError(err, message)
            raise OSError(err, message)

        try:
            if create:
                os.ftruncate(fd, CAM_SHM_SIZE)
            self._mm = mmap.mmap(
                fd,
                CAM_SHM_SIZE,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
            )
        except BaseException:
            os.close(fd)
            raise
        os.close(fd)

        if create:
            # Fresh header: zero everything, then publish version and magic.
            self._mm[:] = b"\x00" * CAM_SHM_SIZE
            self._set_u32(OFF.version, CAM_VERSION)
            self._set_u32(OFF.magic, CAM_MAGIC)

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        if self._mm is not None:
            try:
                self._mm.close()
            finally:
                self._mm = None

    def __enter__(self) -> "CameraShm":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    # -- low level ---------------------------------------------------------

    def _get_u32(self, offset: int) -> int:
        return _U32.unpack_from(self._mm, offset)[0]

    def _set_u32(self, offset: int, value: int) -> None:
        _U32.pack_into(self._mm, offset, value & 0xFFFFFFFF)

    def _require_mapped(self) -> mmap.mmap:
        if self._mm is None:
            raise ValueError("CameraShm is closed")
        return self._mm

    @property
    def mm(self) -> mmap.mmap:
        return self._require_mapped()

    # -- header ------------------------------------------------------------

    @property
    def is_ready(self) -> bool:
        if self._mm is None:
            return False
        return (
            self._get_u32(OFF.magic) == CAM_MAGIC
            and self._get_u32(OFF.version) == CAM_VERSION
        )

    def wait_ready(self, timeout: float | None = 5.0,
                   interval: float = 0.01) -> bool:
        """Poll until magic/version are valid.  ``None`` waits forever."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if self.is_ready:
                return True
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                time.sleep(min(interval, remaining))
            else:
                time.sleep(interval)

    # -- frames ------------------------------------------------------------

    def publish_frame(self, rgb565, w: int = CAM_FRAME_W,
                      h: int = CAM_FRAME_H) -> int:
        """Publish one RGB565 frame and return the new ``frame_seq``.

        The producer writes slot ``(seq + 1) & 1`` and only then bumps
        ``frame_seq``.  The consumer therefore finds the freshly written
        slot at ``frame_seq & 1``.

        ``rgb565`` may be ``bytes`` or a writable ``memoryview``.  The slice
        assignment into the mmap is a direct copy; the caller must not mutate
        the buffer until this call returns.  (CPython has no true shared
        memory zero-copy assignment.)
        """
        mm = self._require_mapped()
        if w <= 0 or h <= 0 or w > CAM_FRAME_W or h > CAM_FRAME_H:
            raise ValueError("frame size %dx%d out of range" % (w, h))

        view = memoryview(rgb565)
        if view.format != "B" or view.ndim != 1:
            try:
                view = view.cast("B")
            except (TypeError, ValueError) as exc:
                raise TypeError("rgb565 must be a contiguous byte buffer") from exc
        if view.nbytes < CAM_FRAME_BYTES:
            raise ValueError(
                "rgb565 buffer is %d bytes, need %d"
                % (view.nbytes, CAM_FRAME_BYTES)
            )

        seq = self._get_u32(OFF.frame_seq)
        slot = (seq + 1) & 1
        start = OFF.frame_data + slot * CAM_FRAME_BYTES

        # Metadata first, then pixels, then the release-style sequence bump.
        self._set_u32(OFF.frame_w, w)
        self._set_u32(OFF.frame_h, h)
        self._set_u32(OFF.frame_stride_px, w)
        self._set_u32(OFF.frame_pad, 0)
        mm[start:start + CAM_FRAME_BYTES] = view[:CAM_FRAME_BYTES]
        new_seq = (seq + 1) & 0xFFFFFFFF
        self._set_u32(OFF.frame_seq, new_seq)
        return new_seq

    def read_frame_bytes(self) -> tuple[bytes, int, int, int] | None:
        """Test helper: copy the slot belonging to the current frame_seq."""
        if self._mm is None:
            return None
        seq = self._get_u32(OFF.frame_seq)
        if seq == 0:
            return None
        slot = seq & 1
        start = OFF.frame_data + slot * CAM_FRAME_BYTES
        data = bytes(self._mm[start:start + CAM_FRAME_BYTES])
        return data, self._get_u32(OFF.frame_w), self._get_u32(OFF.frame_h), seq

    # -- state -------------------------------------------------------------

    def publish_state(self, **fields) -> int:
        """Write the supplied state fields, then bump ``state_seq``.

        Unknown names raise ``KeyError`` *before* anything is written, so a
        typo cannot leave a half-updated snapshot behind.
        """
        for name in fields:
            if name not in _U32_FIELDS and name not in _STR_FIELDS:
                raise KeyError("unknown state field %r" % (name,))

        # Read/bump bookkeeping first so the sequence store is the very next
        # operation after the last field write (smaller publish window).
        seq = self._get_u32(OFF.state_seq)
        new_seq = (seq + 1) & 0xFFFFFFFF

        for name, value in fields.items():
            if name in _U32_FIELDS:
                self._set_u32(getattr(OFF, name), int(value))
            else:
                raw = value
                if isinstance(raw, str):
                    raw = raw.encode("utf-8", "replace")
                elif isinstance(raw, (bytes, bytearray, memoryview)):
                    raw = bytes(raw)
                else:
                    raise TypeError("%s must be str or bytes" % name)
                raw = raw[:CAM_MAX_PATH - 1]
                offset = getattr(OFF, name)
                self._mm[offset:offset + CAM_MAX_PATH] = (
                    raw + b"\x00" * (CAM_MAX_PATH - len(raw))
                )

        self._set_u32(OFF.state_seq, new_seq)
        return new_seq

    def read_state(self, retries: int = 8) -> dict | None:
        """Return a consistent state dict, or ``None`` under heavy churn."""
        mm = self._require_mapped()
        for _ in range(retries):
            before = self._get_u32(OFF.state_seq)
            data = {
                name: self._get_u32(getattr(OFF, name))
                for name in _U32_FIELDS
            }
            data["state_seq"] = before
            for name in _STR_FIELDS:
                offset = getattr(OFF, name)
                data[name] = bytes(mm[offset:offset + CAM_MAX_PATH]) \
                    .split(b"\x00", 1)[0].decode("utf-8", "replace")
            if self._get_u32(OFF.state_seq) == before:
                self._last_state_seq = before
                return data
        return None

    # -- commands ----------------------------------------------------------

    def send_command(self, cmd: int, arg: int = 0) -> int:
        """Test helper mirroring ``ipc_send`` (the UI normally does this in C)."""
        seq = self._get_u32(OFF.cmd_seq)
        self._set_u32(OFF.cmd, int(cmd))
        self._set_u32(OFF.cmd_arg, int(arg))
        new_seq = (seq + 1) & 0xFFFFFFFF
        self._set_u32(OFF.cmd_seq, new_seq)
        return new_seq

    def poll_command(self) -> tuple[int, int] | None:
        """Return ``(cmd, arg)`` for a newly published command, else None.

        The last executed ``cmd_seq`` is remembered, so a command is only
        returned once.
        """
        seq = self._get_u32(OFF.cmd_seq)
        if seq == self._last_cmd_seq or seq == 0:
            return None
        self._last_cmd_seq = seq
        return self._get_u32(OFF.cmd), self._get_u32(OFF.cmd_arg)


# --------------------------------------------------------------------------
# Colour conversion helpers
# --------------------------------------------------------------------------

def rgb888_to_rgb565(buf, w: int, h: int) -> bytearray:
    """Convert packed RGB888 (3 bytes per pixel) to RGB565 little-endian.

    Bit layout (same value the UI's RGB565 code expects)::

        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    Stored low byte first, i.e. ``bytearray[i*2]`` is the low byte.

    WARNING: this is a pure Python per-pixel loop.  On the Pi Zero (ARMv6)
    it takes hundreds of milliseconds for 320x240.  It is the *correct*
    fallback, not the fast path.  See ``rgb565_from_yuv420_native``.
    """
    src = memoryview(buf)
    if src.format != "B" or src.ndim != 1:
        src = src.cast("B")
    count = w * h
    if w <= 0 or h <= 0:
        raise ValueError("w and h must be positive")
    if src.nbytes < count * 3:
        raise ValueError(
            "rgb888 buffer is %d bytes, need %d" % (src.nbytes, count * 3)
        )

    out = bytearray(count * 2)
    j = 0
    for i in range(count):
        r = src[j]
        g = src[j + 1]
        b = src[j + 2]
        j += 3
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[2 * i] = value & 0xFF
        out[2 * i + 1] = value >> 8
    return out


_YUV420_LIB_DEFAULT = "~/.cache/zerosnap/libyuv420.so"


def _load_yuv420(lib_path: str | None = None):
    path = lib_path or os.environ.get("CAM_YUV420_LIB") or _YUV420_LIB_DEFAULT
    path = os.path.expanduser(path)
    if not os.path.exists(path):
        raise FileNotFoundError(
            "native YUV420 helper not found at %r; build it with the command "
            "documented in camera/app/backend/native/yuv420.c" % path
        )
    lib = ctypes.CDLL(path)
    fn = lib.yuv420_to_rgb
    fn.argtypes = [
        ctypes.c_char_p,   # const uint8_t *src
        ctypes.c_int,      # width
        ctypes.c_int,      # height
        ctypes.c_int,      # stride (Y row bytes)
        ctypes.c_void_p,   # uint8_t *dst (RGB888)
    ]
    fn.restype = None
    return fn


def rgb565_from_yuv420_native(yuv, w: int, h: int,
                              stride: int | None = None,
                              lib_path: str | None = None) -> bytearray:
    """I420 -> RGB565 using the existing ``libyuv420.so`` helper.

    API note / why this is only a partial fast path
    -----------------------------------------------
    ``camera/app/backend/native/yuv420.c`` exposes::

        void yuv420_to_rgb(const uint8_t *src, int width, int height,
                           int stride, uint8_t *dst);

    It produces **RGB888**, not RGB565.  It is not modified here (it belongs
    to the daemon).  This wrapper therefore does the expensive YUV decode in
    native code and then calls the pure-Python ``rgb888_to_rgb565`` packer;
    the RGB888 -> RGB565 tail is still slow on ARMv6.

    For a fully native path, either add an RGB565 output kernel to the C
    helper (and load it here), or have the daemon publish RGB565 directly.
    Neither is done in this module because it may not modify the helper.

    ``stride`` is the Y-plane row stride in bytes (defaults to ``w``).  The
    input is a plain I420 buffer: Y ``stride*h`` bytes, then U, then V, each
    ``(stride//2)*(h//2)`` bytes.
    """
    if stride is None:
        stride = w
    if w <= 0 or h <= 0:
        raise ValueError("w and h must be positive")
    if stride < w:
        raise ValueError("stride must be >= width")

    src = memoryview(yuv)
    if src.format != "B" or src.ndim != 1:
        src = src.cast("B")
    required = stride * h + 2 * ((stride // 2) * (h // 2))
    if src.nbytes < required:
        raise ValueError(
            "I420 buffer is %d bytes, need %d for %dx%d stride %d"
            % (src.nbytes, required, w, h, stride)
        )

    fn = _load_yuv420(lib_path)
    # bytes() on a memoryview gives ctypes a stable buffer for the duration
    # of the call.  Avoid copying when the caller already passed bytes.
    if not isinstance(yuv, bytes):
        yuv_bytes = src.tobytes()
    else:
        yuv_bytes = yuv
    dst = (ctypes.c_uint8 * (w * h * 3))()
    fn(yuv_bytes, w, h, stride, ctypes.cast(dst, ctypes.c_void_p))
    return rgb888_to_rgb565(memoryview(dst), w, h)
