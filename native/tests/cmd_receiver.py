"""Receives one command from the C side through the shared memory."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "app"))
from camera_shm import CameraShm

shm = CameraShm(create=True)
print("receiver: ready")
deadline = time.time() + 8.0
while time.time() < deadline:
    command = shm.poll_command()
    if command is not None:
        cmd, arg = command
        ok = cmd == 1 and arg == 7
        print(("PASS" if ok else "FAIL") + f" receiver: cmd={cmd} arg={arg}")
        shm.close()
        raise SystemExit(0 if ok else 1)
    time.sleep(0.02)
print("FAIL receiver: timeout")
shm.close()
raise SystemExit(1)
