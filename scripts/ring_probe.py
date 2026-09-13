"""Attach to a Fovea frame ring by name and report frame flow for N seconds.

Used by integration scripts. Reads only the ring header and slot headers, so it
does not copy pixel data. Output: one JSON line.
"""
import json
import struct
import sys
import time

if sys.platform == "darwin":
    def mono_ns() -> int:
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
else:
    def mono_ns() -> int:
        return time.monotonic_ns()
from multiprocessing import shared_memory, resource_tracker

RING_HEADER = struct.Struct("<IIIIIIIIQQQ")
FRAME_HEADER = struct.Struct("<QQQQIIII16sII")
RING_HEADER_BYTES = 4096


def main() -> int:
    name, seconds = sys.argv[1], float(sys.argv[2])
    shm = shared_memory.SharedMemory(name=name, create=False)
    try:
        resource_tracker.unregister(shm._name, "shared_memory")
    except Exception:
        pass
    buf = shm.buf
    magic, version, slots, slot_bytes, max_w, max_h, _, _, latest, writer_pid, _ = RING_HEADER.unpack_from(buf, 0)
    if magic != 0x46564652:
        print(json.dumps({"error": "bad magic"}))
        return 1
    stride = FRAME_HEADER.size + slot_bytes
    seen = set()
    first = None
    last = None
    latencies = []
    t_end = time.monotonic() + seconds
    while time.monotonic() < t_end:
        latest = struct.unpack_from("<Q", buf, 32)[0]
        if latest and latest not in seen:
            idx = (latest // 2) % slots
            off = RING_HEADER_BYTES + stride * idx
            seq, pts, recv_mono, cap_utc, w, h, st, fmt, sid, flags, _ = FRAME_HEADER.unpack_from(buf, off)
            if seq == latest:
                seen.add(seq)
                now_ns = mono_ns()
                latencies.append((now_ns - recv_mono) / 1e6)
                first = first or seq
                last = (seq, w, h, pts)
        time.sleep(0.004)
    shm.close()
    lat = sorted(latencies)
    p = lambda q: lat[min(len(lat) - 1, int(q * len(lat)))] if lat else None
    print(json.dumps({
        "frames": len(seen), "fps": round(len(seen) / seconds, 1), "size": last[1:3] if last else None,
        "latency_ms_p50": p(0.5), "latency_ms_p95": p(0.95), "writer_pid": writer_pid, "slots": slots,
    }))
    return 0 if seen else 2


if __name__ == "__main__":
    sys.exit(main())
