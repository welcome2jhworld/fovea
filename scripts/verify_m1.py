#!/usr/bin/env python3
"""Cross-platform M1 verification (stdlib only): RTSP test source -> fovea-core ->
live frames in the shared-memory ring -> finalized segments -> disconnect and
reconnect with a new session -> playback -> shutdown -> restart recovery.

Usage:
  python scripts/verify_m1.py --bin-dir build/macos-dev            (build tree)
  python scripts/verify_m1.py --bin-dir dist/fovea/bin --flat      (packaged layout)
Options: --source pattern|file|webcam, --file clip.mp4, --work-dir DIR, --keep
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from multiprocessing import shared_memory
from pathlib import Path

EXE = ".exe" if os.name == "nt" else ""
RING_HEADER_BYTES = 4096
FRAME_HEADER = struct.Struct("<QQQQIIII16sII")
RING_MAGIC = 0x46564652


def mono_ns() -> int:
    if sys.platform == "darwin":
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    if sys.platform == "win32":
        return time.perf_counter_ns()
    return time.monotonic_ns()


class Fail(Exception):
    pass


class Verifier:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        bin_dir = Path(args.bin_dir).resolve()
        if args.flat:
            self.core = bin_dir / f"fovea-core{EXE}"
            self.testsrc = bin_dir / f"rtsp-testsrc{EXE}"
        else:
            self.core = self._find(bin_dir, [Path("src/core"), Path(".")], f"fovea-core{EXE}")
            self.testsrc = self._find(bin_dir, [Path("tools/rtsp-testsrc"), Path(".")], f"rtsp-testsrc{EXE}")
        self.work = Path(args.work_dir).resolve()
        self.data = self.work / "data"
        self.port = args.rtsp_port
        self.url = f"rtsp://127.0.0.1:{self.port}/test"
        self.core_proc: subprocess.Popen | None = None
        self.src_proc: subprocess.Popen | None = None
        self.log_lines: list[str] = []
        self.api_port = 0
        self.token = ""
        self.results: dict = {"platform": sys.platform, "source": args.source}

    @staticmethod
    def _find(root: Path, subdirs: list[Path], name: str) -> Path:
        for sub in subdirs:
            candidate = root / sub / name
            if candidate.exists():
                return candidate
        return root / subdirs[0] / name

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        self.log_lines.append(line)

    def api(self, method: str, path: str, body: dict | None = None, timeout: float = 15.0):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.api_port}{path}", data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                raw = r.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            raise Fail(f"{method} {path} -> {e.code} {e.read()[:300]!r}") from e

    def start_source(self) -> None:
        cmd = [str(self.testsrc), "--port", str(self.port), "--path", "/test"]
        if self.args.source == "file":
            cmd += ["--file", str(Path(self.args.file).resolve()), "--loop"]
        elif self.args.source == "webcam":
            cmd += ["--webcam"]
        else:
            cmd += ["--pattern", "ball", "--width", "640", "--height", "360", "--fps", "25"]
        out = open(self.work / "testsrc.log", "ab")
        self.src_proc = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
        time.sleep(2.0)
        if self.src_proc.poll() is not None:
            raise Fail(f"rtsp-testsrc exited with {self.src_proc.returncode}; see testsrc.log")

    def stop_source(self) -> None:
        if self.src_proc and self.src_proc.poll() is None:
            self.src_proc.terminate()
            try:
                self.src_proc.wait(5)
            except subprocess.TimeoutExpired:
                self.src_proc.kill()
        self.src_proc = None

    def start_core(self, tag: str) -> None:
        info = self.data / "core.json"
        if info.exists():
            info.unlink()
        env = dict(os.environ, FOVEA_MIN_FREE_MB="50")
        out = open(self.work / f"core-{tag}.log", "ab")
        self.core_proc = subprocess.Popen([str(self.core), "--data-dir", str(self.data), "--port", "0"],
                                          stdout=out, stderr=subprocess.STDOUT, env=env)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if info.exists():
                try:
                    self.api_port = int(json.loads(info.read_text())["port"])
                    self.token = (self.data / "core.token").read_text().strip()
                    return
                except (ValueError, KeyError, json.JSONDecodeError):
                    pass
            if self.core_proc.poll() is not None:
                raise Fail(f"fovea-core exited with {self.core_proc.returncode}; see core-{tag}.log")
            time.sleep(0.25)
        raise Fail("core.json not written within 20 s")

    def wait_status(self, cam: str, pred, timeout: float, what: str) -> dict:
        deadline = time.monotonic() + timeout
        last: dict = {}
        while time.monotonic() < deadline:
            last = self.api("GET", f"/v1/cameras/{cam}/status")
            if pred(last):
                return last
            time.sleep(0.25)
        raise Fail(f"timeout waiting for {what}: {json.dumps(last)[:400]}")

    def probe_ring(self, name: str, seconds: float) -> dict:
        shm_name = f"Local\\{name}" if os.name == "nt" else name
        shm = shared_memory.SharedMemory(name=shm_name, create=False)
        if os.name != "nt":
            try:
                from multiprocessing import resource_tracker
                resource_tracker.unregister(shm._name, "shared_memory")
            except Exception:
                pass
        try:
            buf = shm.buf
            magic, _ver, slots, slot_bytes = struct.unpack_from("<IIII", buf, 0)
            if magic != RING_MAGIC:
                raise Fail("frame ring magic mismatch")
            stride = FRAME_HEADER.size + slot_bytes
            seen: set[int] = set()
            lat: list[float] = []
            size = None
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                latest = struct.unpack_from("<Q", buf, 32)[0]
                if latest and latest not in seen:
                    off = RING_HEADER_BYTES + stride * ((latest // 2) % slots)
                    seq, _pts, recv, _cap, w, h, *_ = FRAME_HEADER.unpack_from(buf, off)
                    if seq == latest:
                        seen.add(seq)
                        lat.append((mono_ns() - recv) / 1e6)
                        size = (w, h)
                time.sleep(0.004)
            del buf
        finally:
            shm.close()
        lat.sort()
        pick = lambda q: round(lat[min(len(lat) - 1, int(q * len(lat)))], 2) if lat else None
        return {"frames": len(seen), "fps": round(len(seen) / seconds, 1), "size": size,
                "latency_ms_p50": pick(0.5), "latency_ms_p95": pick(0.95)}

    def run(self) -> None:
        for exe in (self.core, self.testsrc):
            if not exe.exists():
                raise Fail(f"missing {exe}")
        if self.work.exists():
            shutil.rmtree(self.work)
        self.data.mkdir(parents=True)

        self.log(f"source: {self.args.source}")
        self.start_source()
        self.start_core("1")
        health = self.api("GET", "/v1/health")
        self.log(f"core on 127.0.0.1:{self.api_port} health={health.get('status')}")

        cam = self.api("POST", "/v1/cameras", {
            "code": "CAM-01", "name": "Test source", "group_name": "Lab", "kind": "rtsp",
            "main_url": self.url, "transport": self.args.transport, "jitter_ms": 1000,
            "segment_seconds": 10, "record_enabled": True})["id"]
        self.log(f"camera {cam}")

        t0 = time.monotonic()
        st = self.wait_status(cam, lambda s: s["state"] == "online" and s["fps_new"] > 1, 40, "online with frames")
        self.results["time_to_online_s"] = round(time.monotonic() - t0, 2)
        time.sleep(3)
        st = self.api("GET", f"/v1/cameras/{cam}/status")
        session1 = st["session_id"]
        self.results["status_online"] = {k: st.get(k) for k in ("fps_new", "latency_ms", "codec", "width", "height", "drops", "queue_depth")}
        self.log(f"online: {self.results['status_online']}")
        ring = self.probe_ring(st["frame_ring"]["name"], 3.0)
        self.results["ring_live"] = ring
        self.log(f"ring: {ring}")
        if ring["frames"] < 30:
            raise Fail("fewer than 30 frames in 3 s from the live ring")

        deadline = time.monotonic() + 60
        segs: list = []
        while time.monotonic() < deadline:
            segs = self.api("GET", f"/v1/cameras/{cam}/segments")
            if sum(1 for s in segs if s["state"] == "finalized") >= 2:
                break
            time.sleep(0.5)
        fin = [s for s in segs if s["state"] == "finalized"]
        if len(fin) < 2:
            raise Fail(f"finalized segments: {len(fin)}")
        seg = fin[-1]
        seg_path = Path(seg["path"])
        if not seg_path.exists() or seg_path.stat().st_size < 10000:
            raise Fail(f"segment file missing or tiny: {seg_path}")
        durations = [round((s["end_pts_ns"] - s["start_pts_ns"]) / 1e9, 2) for s in fin]
        self.results["segments"] = {"finalized": len(fin), "durations_s": durations, "bytes": [s["bytes"] for s in fin]}
        self.log(f"segments: {self.results['segments']}")

        self.log("stopping source")
        self.stop_source()
        t0 = time.monotonic()
        st = self.wait_status(cam, lambda s: s["state"] != "online", 30, "offline after source stop")
        self.results["disconnect_detect_s"] = round(time.monotonic() - t0, 2)
        self.log(f"state {st['state']} after {self.results['disconnect_detect_s']} s, last_frame_age_ms={st['last_frame_age_ms']}")
        time.sleep(3)

        self.log("restarting source")
        self.start_source()
        t0 = time.monotonic()
        st = self.wait_status(cam, lambda s: s["state"] == "online" and s["fps_new"] > 1 and s["session_id"] != session1,
                              60, "reconnect with a new session")
        self.results["reconnect_s"] = round(time.monotonic() - t0, 2)
        time.sleep(2)
        gaps = self.api("GET", f"/v1/cameras/{cam}/gaps")
        self.results["gaps"] = {"total": len(gaps), "closed": sum(1 for g in gaps if g["to_utc_ms"] > 0)}
        self.log(f"reconnected in {self.results['reconnect_s']} s, gaps {self.results['gaps']}")
        if not gaps:
            raise Fail("no receive gap recorded for the outage")

        pb = self.api("POST", "/v1/playback", {"segment_id": seg["id"]})
        self.api("POST", f"/v1/playback/{pb['id']}/play")
        time.sleep(2.5)
        state = self.api("GET", f"/v1/playback/{pb['id']}")
        pring = self.probe_ring(state["frame_ring"]["name"], 2.0)
        self.results["playback"] = {"state": state["state"], "position_s": round(state["position_ns"] / 1e9, 2), "ring": pring}
        self.log(f"playback: {self.results['playback']}")
        if state["position_ns"] <= 0 or pring["frames"] < 10:
            raise Fail("playback did not advance")
        self.api("DELETE", f"/v1/playback/{pb['id']}")

        self.results["metrics"] = self.api("GET", "/v1/metrics").get("process")
        self.log("shutdown via API")
        self.api("POST", "/v1/service/shutdown")
        try:
            self.core_proc.wait(15)
        except subprocess.TimeoutExpired:
            raise Fail("core did not exit after shutdown")
        if (self.data / "core.json").exists():
            raise Fail("core.json left behind after shutdown")

        self.start_core("2")
        time.sleep(2)
        current = self.api("GET", f"/v1/cameras/{cam}/status").get("session_id")
        segs = self.api("GET", f"/v1/cameras/{cam}/segments")
        stale = [s for s in segs if s["state"] == "recording" and s["session_id"] != current]
        self.results["after_restart"] = {
            "finalized": sum(1 for s in segs if s["state"] == "finalized"),
            "damaged": sum(1 for s in segs if s["state"] == "damaged"),
            "stale_recording": len(stale)}
        self.log(f"after restart: {self.results['after_restart']}")
        if stale:
            raise Fail("segments from earlier sessions still marked recording")
        self.api("POST", "/v1/service/shutdown")
        self.core_proc.wait(15)

    def cleanup(self) -> None:
        self.stop_source()
        if self.core_proc and self.core_proc.poll() is None:
            self.core_proc.terminate()
            try:
                self.core_proc.wait(5)
            except subprocess.TimeoutExpired:
                self.core_proc.kill()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--bin-dir", required=True)
    p.add_argument("--flat", action="store_true", help="binaries are all directly in --bin-dir")
    p.add_argument("--work-dir", default="build/verify-m1-py")
    p.add_argument("--source", choices=["pattern", "file", "webcam"], default="pattern")
    p.add_argument("--file")
    p.add_argument("--transport", choices=["tcp", "udp"], default="tcp")
    p.add_argument("--rtsp-port", type=int, default=8554)
    p.add_argument("--report", help="write a JSON report here")
    args = p.parse_args()
    if args.source == "file" and not args.file:
        p.error("--source file needs --file")
    v = Verifier(args)
    code = 0
    try:
        v.run()
        v.results["result"] = "PASS"
        v.log("PASS")
    except Fail as e:
        v.results["result"] = f"FAIL: {e}"
        v.log(f"FAIL: {e}")
        code = 1
    finally:
        v.cleanup()
    if args.report:
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(v.results, indent=1))
    return code


if __name__ == "__main__":
    if os.name != "nt":
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    sys.exit(main())
