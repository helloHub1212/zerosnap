#!/usr/bin/env python3
"""Host tests for python/camera_shm.py.

Modes (driven by tests/run_ipc_test.sh):

    selftest [libipc.so]   layout, converters and in-process publish/read
    producer               create the segment and publish a known state+frame
    check-command          consume the command sent by the C test
    cleanup                unlink the segment

The cross-language direction is:
    camera_shm.py producer  ->  ipc_test read-published   (C reads Python)
    ipc_test send-command   ->  camera_shm.py check-command (Python reads C)
"""

import ctypes
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "app"))

import camera_shm as cs  # noqa: E402

failures = 0


def check(condition, name, detail=""):
    global failures
    if condition:
        if detail:
            print("PASS: %s (%s)" % (name, detail))
        else:
            print("PASS: %s" % name)
    else:
        if detail:
            print("FAIL: %s (%s)" % (name, detail))
        else:
            print("FAIL: %s" % name)
        failures += 1


def check_eq(got, want, name):
    check(got == want, name, "got %r, want %r" % (got, want))


def finish():
    print("RESULT: %s" % ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


def selftest(lib_path=None):
    # ---- layout ---------------------------------------------------------
    check_eq(cs.CAM_SHM_SIZE, 308048, "layout: ctypes sizeof(CamShm)")
    check_eq(cs.OFF.frame_data, 28, "layout: frame_data offset")
    check_eq(cs.OFF.state_seq, 28 + 2 * cs.CAM_FRAME_BYTES,
             "layout: state_seq offset")
    check_eq(cs.OFF.last_photo, cs.OFF.state_seq + 40,
             "layout: last_photo offset")
    check_eq(cs.OFF.last_video, cs.OFF.last_photo + cs.CAM_MAX_PATH,
             "layout: last_video offset")
    check_eq(cs.OFF.status_text, cs.OFF.last_video + cs.CAM_MAX_PATH,
             "layout: status_text offset")
    check_eq(cs.OFF.cmd_seq, cs.CAM_SHM_SIZE - 12, "layout: cmd_seq offset")

    if lib_path:
        lib = ctypes.CDLL(lib_path)
        lib.cam_shm_size.argtypes = []
        lib.cam_shm_size.restype = ctypes.c_size_t
        c_size = int(lib.cam_shm_size())
        check_eq(c_size, cs.CAM_SHM_SIZE,
                 "layout: C sizeof(CamShm) == ctypes sizeof")

    # ---- RGB888 -> RGB565 ----------------------------------------------
    pixels = [
        (255, 0, 0, 0xF800),
        (0, 255, 0, 0x07E0),
        (0, 0, 255, 0x001F),
        (255, 255, 255, 0xFFFF),
        (0, 0, 0, 0x0000),
        (0x12, 0x34, 0x56, 0x11AA),
    ]
    src = bytearray()
    for r, g, b, _ in pixels:
        src += bytes((r, g, b))
    out = cs.rgb888_to_rgb565(bytes(src), len(pixels), 1)
    check_eq(len(out), len(pixels) * 2, "rgb565: output length")
    for idx, (_r, _g, _b, want) in enumerate(pixels):
        got = out[2 * idx] | (out[2 * idx + 1] << 8)
        check_eq(got, want, "rgb565: pixel %d" % idx)

    # ---- in-process publish/read ---------------------------------------
    cs.unlink()
    shm = cs.CameraShm(create=True)
    try:
        check(shm.is_ready, "shm: magic/version stamped")
        check(shm.wait_ready(0), "shm: wait_ready immediate")
        check_eq(shm._get_u32(cs.OFF.magic), cs.CAM_MAGIC, "shm: magic value")
        check_eq(shm._get_u32(cs.OFF.version), cs.CAM_VERSION,
                 "shm: version value")

        before = shm._get_u32(cs.OFF.state_seq)
        try:
            shm.publish_state(not_a_real_field=1)
            check(False, "state: unknown field rejected")
        except KeyError:
            check(True, "state: unknown field rejected")
        check_eq(shm._get_u32(cs.OFF.state_seq), before,
                 "state: rejected publish did not bump seq")

        seq1 = shm.publish_state(
            mode=1, recording=1, video_mode=2, flags=5, rec_seconds=17,
            photo_count=3, video_count=4, free_mb=1234, battery_pct=88,
            status_text="hello-from-python",
            last_photo="/tmp/py-photo.jpg",
            last_video="/tmp/py-video.mp4",
        )
        check(seq1 > 0, "state: publish bumps seq")
        st = shm.read_state()
        check(st is not None, "state: read returns a snapshot")
        if st is not None:
            check_eq(st["mode"], 1, "state: mode")
            check_eq(st["recording"], 1, "state: recording")
            check_eq(st["video_mode"], 2, "state: video_mode")
            check_eq(st["flags"], 5, "state: flags")
            check_eq(st["rec_seconds"], 17, "state: rec_seconds")
            check_eq(st["photo_count"], 3, "state: photo_count")
            check_eq(st["video_count"], 4, "state: video_count")
            check_eq(st["free_mb"], 1234, "state: free_mb")
            check_eq(st["battery_pct"], 88, "state: battery_pct")
            check_eq(st["status_text"], "hello-from-python",
                     "state: status_text")
            check_eq(st["last_photo"], "/tmp/py-photo.jpg",
                     "state: last_photo")
            check_eq(st["last_video"], "/tmp/py-video.mp4",
                     "state: last_video")

        frame_a = bytes(((i * 13 + 7) & 0xFF)
                        for i in range(cs.CAM_FRAME_BYTES))
        fseq_a = shm.publish_frame(frame_a, 320, 240)
        check(fseq_a > 0, "frame: publish bumps seq")
        read_a = shm.read_frame_bytes()
        check(read_a is not None, "frame: readback available")
        if read_a is not None:
            data, w, h, seq = read_a
            check_eq(len(data), cs.CAM_FRAME_BYTES, "frame: readback length")
            check_eq((w, h), (320, 240), "frame: readback dimensions")
            check_eq(seq, fseq_a, "frame: readback seq")
            check(data == frame_a, "frame: readback bytes match")

        frame_b = bytes(((i * 29 + 11) & 0xFF)
                        for i in range(cs.CAM_FRAME_BYTES))
        fseq_b = shm.publish_frame(frame_b, 320, 240)
        check_eq(fseq_b, fseq_a + 1, "frame: second seq increments")
        read_b = shm.read_frame_bytes()
        check(read_b is not None and read_b[0] == frame_b,
              "frame: second readback bytes match")

        check(shm.poll_command() is None, "command: none before send")
        shm.send_command(cs.CAM_CMD_SET_VIDEO_MODE, 2)
        check_eq(shm.poll_command(), (cs.CAM_CMD_SET_VIDEO_MODE, 2),
                 "command: poll returns command")
        check(shm.poll_command() is None, "command: not returned twice")
    finally:
        shm.close()

    cs.unlink()
    try:
        cs.CameraShm(create=False)
        check(False, "open: missing segment raises FileNotFoundError")
    except FileNotFoundError:
        check(True, "open: missing segment raises FileNotFoundError")

    return finish()


def producer():
    """Create the segment and publish a deterministic state and frame."""
    cs.unlink()
    shm = cs.CameraShm(create=True)
    try:
        check(shm.is_ready, "producer: segment ready")
        shm.publish_state(
            mode=1, recording=1, video_mode=2, flags=5, rec_seconds=17,
            photo_count=3, video_count=4, free_mb=1234, battery_pct=88,
            status_text="hello-from-python",
            last_photo="/tmp/py-photo.jpg",
            last_video="/tmp/py-video.mp4",
        )
        frame = bytes(((i * 13 + 7) & 0xFF)
                      for i in range(cs.CAM_FRAME_BYTES))
        seq = shm.publish_frame(frame, 320, 240)
        check(True, "producer: published state and frame", "frame_seq=%d" % seq)
    finally:
        shm.close()
    return finish()


def check_command():
    deadline = time.monotonic() + 5.0
    try:
        shm = cs.CameraShm(create=False)
    except FileNotFoundError:
        check(False, "consumer: segment exists")
        return finish()

    try:
        check(shm.is_ready, "consumer: segment ready")
        cmd = None
        while time.monotonic() < deadline:
            cmd = shm.poll_command()
            if cmd is not None:
                break
            time.sleep(0.01)
        check_eq(cmd, (cs.CAM_CMD_SHUTTER, 2),
                 "consumer: command from C")
    finally:
        shm.close()
    return finish()


def cleanup():
    cs.unlink()
    try:
        cs.CameraShm(create=False)
        check(False, "cleanup: segment removed")
        # Should not happen; close the accidental mapping before returning.
        # (The object is local, so just let it be collected.)
    except FileNotFoundError:
        check(True, "cleanup: segment removed")
    return finish()


def main(argv):
    mode = argv[1] if len(argv) > 1 else "selftest"
    if mode == "selftest":
        lib = argv[2] if len(argv) > 2 else os.environ.get("CAM_IPC_LIB")
        return selftest(lib)
    if mode == "producer":
        return producer()
    if mode == "check-command":
        return check_command()
    if mode == "cleanup":
        return cleanup()
    print("usage: %s [selftest [libipc.so]|producer|check-command|cleanup]"
          % argv[0], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
