#!/usr/bin/env python3
"""Cross-platform M2 verification (stdlib only): N RTSP test sources -> one
fovea-core recording all of them -> soak with resource sampling -> kill -9 and
recovery -> console opened and closed while the core records -> disk floor ->
retention with an evidence hold and a crash between row commit and file removal.

Usage:
  python3 scripts/verify_m2.py --bin-dir build/macos-dev [--inputs 4] [--minutes 10]
  python3 scripts/verify_m2.py --bin-dir dist/fovea/bin --flat
The JSON report and the log go to docs/verification/m2-<UTC stamp>.{json,log}
unless --report is given. Recordings stay under --work-dir and are removed at
the end unless --keep is given.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import signal
import sqlite3
import statistics
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from multiprocessing import shared_memory
from pathlib import Path

EXE = ".exe" if os.name == "nt" else ""
MIB = 1024 * 1024
RING_HEADER_BYTES = 4096
FRAME_HEADER = struct.Struct("<QQQQIIII16sII")
RING_MAGIC = 0x46564652
REPO = Path(__file__).resolve().parent.parent
CONTENT_END_SQL = ("CASE WHEN end_utc_ms>0 THEN end_utc_ms WHEN start_utc_ms>0 THEN start_utc_ms "
                   "ELSE created_utc_ms END")


def mono_ns() -> int:
    if sys.platform == "darwin":
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    if sys.platform == "win32":
        return time.perf_counter_ns()
    return time.monotonic_ns()


def utc_ms() -> int:
    return int(time.time() * 1000)


def rel(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return path.name


class Fail(Exception):
    pass


class Verifier:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        bin_dir = Path(args.bin_dir).resolve()
        if args.flat:
            self.core = bin_dir / f"fovea-core{EXE}"
            self.testsrc = bin_dir / f"rtsp-testsrc{EXE}"
            self.console = bin_dir / f"fovea{EXE}"
        else:
            self.core = self._find(bin_dir, ["src/core", "."], f"fovea-core{EXE}")
            self.testsrc = self._find(bin_dir, ["tools/rtsp-testsrc", "."], f"rtsp-testsrc{EXE}")
            self.console = self._find(bin_dir, ["src/console", "src/console/fovea.app/Contents/MacOS", "."], f"fovea{EXE}")
        self.work = Path(args.work_dir).resolve()
        self.data = self.work / "data"
        self.db_path = self.data / "fovea.sqlite"
        self.core_proc: subprocess.Popen | None = None
        self.src_procs: list[subprocess.Popen] = []
        self.cams: list[str] = []
        self.api_port = 0
        self.token = ""
        self.log_lines: list[str] = []
        self.failures: list[str] = []
        self.results: dict = {
            "platform": sys.platform, "machine": platform.machine(), "cpu_count": os.cpu_count(),
            "inputs": args.inputs, "source": {"pattern": "ball", "width": args.width, "height": args.height, "fps": args.fps},
            "segment_seconds": args.segment_seconds, "soak_minutes": args.minutes,
        }

    @staticmethod
    def _find(root: Path, subdirs: list[str], name: str) -> Path:
        for sub in subdirs:
            candidate = root / sub / name
            if candidate.is_file():
                return candidate
        return root / subdirs[0] / name

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        self.log_lines.append(line)

    def check(self, ok: bool, msg: str) -> bool:
        if not ok:
            self.failures.append(msg)
            self.log(f"CHECK FAILED: {msg}")
        return ok

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
        except (urllib.error.URLError, OSError) as e:
            raise Fail(f"{method} {path} -> {e}") from e

    def api_status(self, method: str, path: str, body: dict) -> int:
        try:
            self.api(method, path, body)
            return 200
        except Fail as e:
            m = re.search(r"-> (\d{3}) ", str(e))
            return int(m.group(1)) if m else 0

    # processes

    def start_sources(self) -> None:
        a = self.args
        for i in range(a.inputs):
            port = a.rtsp_port_base + i
            cmd = [str(self.testsrc), "--port", str(port), "--path", "/test", "--pattern", "ball",
                   "--width", str(a.width), "--height", str(a.height), "--fps", str(a.fps)]
            out = open(self.work / f"testsrc-{i}.log", "ab")
            self.src_procs.append(subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT))
        time.sleep(2.0)
        for i, p in enumerate(self.src_procs):
            if p.poll() is not None:
                raise Fail(f"rtsp-testsrc {i} exited with {p.returncode}; see testsrc-{i}.log")

    def stop_sources(self) -> None:
        for p in self.src_procs:
            if p.poll() is None:
                p.terminate()
        for p in self.src_procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        self.src_procs = []

    def start_core(self, tag: str, extra_env: dict[str, str] | None = None) -> None:
        info = self.data / "core.json"
        if info.exists():
            info.unlink()
        env = dict(os.environ)
        env.pop("FOVEA_RETENTION_SECONDS", None)
        env["FOVEA_MIN_FREE_MB"] = str(self.args.min_free_mb)
        env.update(extra_env or {})
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
            time.sleep(0.1)
        raise Fail("core.json not written within 20 s")

    def stop_core(self) -> None:
        self.api("POST", "/v1/service/shutdown")
        try:
            self.core_proc.wait(20)
        except subprocess.TimeoutExpired:
            raise Fail("core did not exit within 20 s of shutdown")

    def core_log_line(self, tag: str, pattern: str) -> str:
        text = (self.work / f"core-{tag}.log").read_text(errors="replace")
        lines = [ln for ln in text.splitlines() if pattern in ln]
        return lines[-1].split(" ", 2)[-1] if lines else ""

    # observations

    def statuses(self) -> list[dict]:
        return [self.api("GET", f"/v1/cameras/{c}/status") for c in self.cams]

    def wait_all(self, pred, timeout: float, what: str) -> list[dict]:
        deadline = time.monotonic() + timeout
        last: list[dict] = []
        while time.monotonic() < deadline:
            try:
                last = self.statuses()
                if all(pred(s) for s in last):
                    return last
            except Fail:
                pass
            time.sleep(0.25)
        brief = [{k: s.get(k) for k in ("state", "fps_new", "recording", "last_error")} for s in last]
        raise Fail(f"timeout after {timeout:.0f} s waiting for {what}: {brief}")

    def online(self, s: dict) -> bool:
        return s["state"] == "online" and s["fps_new"] > self.args.min_fps

    def segments(self, cam: str) -> list[dict]:
        return self.api("GET", f"/v1/cameras/{cam}/segments?limit=100000")

    def finalized_count(self, cam: str) -> int:
        return sum(1 for s in self.segments(cam) if s["state"] == "finalized")

    def db(self, readonly: bool = True) -> sqlite3.Connection:
        if readonly:
            return sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=10)
        return sqlite3.connect(str(self.db_path), timeout=10)

    def ps_sample(self) -> dict | None:
        if os.name == "nt" or not self.core_proc:
            return None
        try:
            out = subprocess.run(["ps", "-o", "rss=,%cpu=", "-p", str(self.core_proc.pid)], capture_output=True,
                                 text=True, timeout=5).stdout.split()
            return {"rss_mb": round(int(out[0]) / 1024, 1), "cpu_pct": float(out[1])}
        except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
            return None

    def probe_ring(self, name: str, seconds: float) -> dict:
        shm = shared_memory.SharedMemory(name=f"Local\\{name}" if os.name == "nt" else name, create=False)
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
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                latest = struct.unpack_from("<Q", buf, 32)[0]
                if latest and latest not in seen:
                    off = RING_HEADER_BYTES + stride * ((latest // 2) % slots)
                    seq, _pts, recv, *_ = FRAME_HEADER.unpack_from(buf, off)
                    if seq == latest:
                        seen.add(seq)
                        lat.append((mono_ns() - recv) / 1e6)
                time.sleep(0.004)
            del buf
        finally:
            shm.close()
        lat.sort()
        p95 = round(lat[min(len(lat) - 1, int(0.95 * len(lat)))], 2) if lat else None
        return {"frames": len(seen), "fps": round(len(seen) / seconds, 1), "latency_ms_p95": p95}

    # phases

    def setup(self) -> None:
        for exe in (self.core, self.testsrc):
            if not exe.exists():
                raise Fail(f"missing {exe.name} under --bin-dir")
        if self.work.exists():
            shutil.rmtree(self.work)
        self.data.mkdir(parents=True)
        a = self.args
        self.log(f"{a.inputs} inputs, pattern ball {a.width}x{a.height} {a.fps} fps, segment {a.segment_seconds} s, "
                 f"soak {a.minutes} min, work dir {rel(self.work)}")
        if hasattr(os, "getloadavg"):
            self.results["loadavg_start"] = [round(x, 2) for x in os.getloadavg()]
        self.start_sources()
        self.start_core("1")
        for i in range(a.inputs):
            cam = self.api("POST", "/v1/cameras", {
                "code": f"CAM-{i + 1:02d}", "name": f"Source {i + 1}", "group_name": "M2", "kind": "rtsp",
                "main_url": f"rtsp://127.0.0.1:{a.rtsp_port_base + i}/test", "transport": "tcp", "jitter_ms": 1000,
                "segment_seconds": a.segment_seconds, "record_enabled": True})
            self.cams.append(cam["id"])
            self.check(cam.get("retention_days") == 7 and cam.get("max_bytes") == 0,
                       f"camera defaults retention_days/max_bytes: {cam.get('retention_days')}/{cam.get('max_bytes')}")
        t0 = time.monotonic()
        sts = self.wait_all(lambda s: self.online(s) and s["recording"] == "recording", 60, "all cameras online")
        self.results["time_to_all_online_s"] = round(time.monotonic() - t0, 2)
        self.log(f"all online in {self.results['time_to_all_online_s']} s: fps {[s['fps_new'] for s in sts]}")

    def soak(self) -> None:
        a = self.args
        interval = a.sample_seconds
        n = max(2, int(a.minutes * 60 // interval))
        start_metrics = self.api("GET", "/v1/metrics")
        fin0 = {c: self.finalized_count(c) for c in self.cams}
        t0 = time.monotonic()
        samples: list[dict] = []
        for k in range(1, n + 1):
            time.sleep(max(0.0, t0 + k * interval - time.monotonic()))
            m = self.api("GET", "/v1/metrics")
            by_id = {c["camera_id"]: c for c in m["cameras"]}
            sample = {"t_s": round(time.monotonic() - t0, 1), "process": {
                "rss_mb": round(m["process"]["rss_bytes"] / MIB, 1), "cpu_percent": round(m["process"]["cpu_percent"], 1),
                "cpu_time_ms": m["process"]["cpu_time_ms"]}, "ps": self.ps_sample(), "cameras": []}
            for c in self.cams:
                cm = by_id[c]
                sample["cameras"].append({
                    "state": cm["state"], "fps_new": cm["fps_new"], "latency_p95_ms": round(cm["latency_ms"]["p95"], 2),
                    "drops": cm["drops"], "queue_depth": cm["queue_depth"], "queue_max": cm["queue_max"],
                    "recording": cm["recording"], "finalized": self.finalized_count(c)})
            samples.append(sample)
            cams = " | ".join(f"{s['fps_new']:.0f}fps p95={s['latency_p95_ms']}ms q={s['queue_depth']} d={s['drops']} "
                              f"fin={s['finalized']} {s['recording']}" for s in sample["cameras"])
            self.log(f"t={sample['t_s']:.0f}s rss={sample['process']['rss_mb']}MB cpu={sample['process']['cpu_percent']}% "
                     f"ps={sample['ps']} :: {cams}")
        wall_ms = (time.monotonic() - t0) * 1000
        end_metrics = self.api("GET", "/v1/metrics")

        for s in samples:
            for i, c in enumerate(s["cameras"]):
                self.check(c["state"] == "online" and c["recording"] == "recording",
                           f"camera {i} at t={s['t_s']} s: state {c['state']} recording {c['recording']}")
                self.check(c["queue_depth"] <= c["queue_max"],
                           f"camera {i} at t={s['t_s']} s: queue_depth {c['queue_depth']} above {c['queue_max']}")
        per_minute = max(1, int(60 // interval))
        per_camera = []
        for i, cam in enumerate(self.cams):
            fins = [fin0[cam]] + [s["cameras"][i]["finalized"] for s in samples]
            for j in range(len(fins) - per_minute):
                self.check(fins[j + per_minute] > fins[j],
                           f"camera {i} gained no finalized segment between t={j * interval} s and t={(j + per_minute) * interval} s")
            fps = [s["cameras"][i]["fps_new"] for s in samples]
            p95 = [s["cameras"][i]["latency_p95_ms"] for s in samples]
            summary = {
                "fps_mean": round(statistics.mean(fps), 2), "fps_min": min(fps),
                "latency_p95_ms_median": round(statistics.median(p95), 2), "latency_p95_ms_max": max(p95),
                "drops_end": samples[-1]["cameras"][i]["drops"],
                "queue_depth_max": max(s["cameras"][i]["queue_depth"] for s in samples),
                "finalized_start": fins[0], "finalized_end": fins[-1],
                "segments_per_minute": round((fins[-1] - fins[0]) / (wall_ms / 60000), 2)}
            per_camera.append(summary)
            self.check(summary["fps_mean"] >= a.min_fps, f"camera {i} mean fps {summary['fps_mean']} below {a.min_fps}")
        base = samples[min(per_minute - 1, len(samples) - 1)]["process"]["rss_mb"]
        end = samples[-1]["process"]["rss_mb"]
        growth = round((end - base) / base * 100, 1)
        cpu_avg = round((end_metrics["process"]["cpu_time_ms"] - start_metrics["process"]["cpu_time_ms"]) / wall_ms * 100, 1)
        self.results["soak"] = {
            "samples": samples, "per_camera": per_camera, "rss_mb_start": round(start_metrics["process"]["rss_bytes"] / MIB, 1),
            "rss_mb_after_first_minute": base, "rss_mb_end": end, "rss_growth_pct_after_first_minute": growth,
            "cpu_percent_avg": cpu_avg, "cpu_percent_samples_max": max(s["process"]["cpu_percent"] for s in samples),
            "duration_s": round(wall_ms / 1000, 1)}
        self.log(f"soak summary: rss {self.results['soak']['rss_mb_start']} -> {base} (1 min) -> {end} MB "
                 f"({growth}%), cpu avg {cpu_avg}%")
        for i, pc in enumerate(per_camera):
            self.log(f"  camera {i}: {pc}")
        self.check(growth < 25, f"RSS grew {growth}% after the first minute")

    @staticmethod
    def ring_exists(name: str) -> bool:
        try:
            shm = shared_memory.SharedMemory(name=f"Local\\{name}" if os.name == "nt" else name, create=False)
        except (FileNotFoundError, OSError):
            return False
        if os.name != "nt":
            try:
                from multiprocessing import resource_tracker
                resource_tracker.unregister(shm._name, "shared_memory")
            except Exception:
                pass
        shm.close()
        return True

    def crash_restart(self) -> None:
        sts = self.statuses()
        before = {c: s["session_id"] for c, s in zip(self.cams, sts)}
        killed_rings = [s["frame_ring"]["name"] for s in sts]
        self.log("kill -9 core while recording")
        self.core_proc.kill()
        self.core_proc.wait(10)
        t0 = time.monotonic()
        self.start_core("2")
        self.wait_all(self.online, 90, "all cameras online after kill -9")
        recovery_s = round(time.monotonic() - t0, 2)
        time.sleep(2)
        report = {"restart_to_all_online_s": recovery_s, "recovery_log": self.core_log_line("2", "recovery:"),
                  "killed_session_rings_left": sum(1 for n in killed_rings if self.ring_exists(n))}
        stale_sessions = stale_segments = 0
        for cam, st in zip(self.cams, self.statuses()):
            current = st["session_id"]
            sessions = self.api("GET", f"/v1/cameras/{cam}/sessions?limit=1000")
            stale_sessions += sum(1 for s in sessions if s["ended_utc_ms"] == 0 and s["id"] != current)
            old = next((s for s in sessions if s["id"] == before[cam]), None)
            self.check(old is not None and old["ended_utc_ms"] > 0 and old["end_reason"] == "shutdown",
                       f"killed session of {cam[:8]} not closed as shutdown: {old and old['end_reason']}")
            stale_segments += sum(1 for s in self.segments(cam) if s["state"] == "recording" and s["session_id"] != current)
        report.update({"open_sessions_not_current": stale_sessions, "recording_rows_not_current": stale_segments})
        self.results["crash_restart"] = report
        self.log(f"after kill -9: {report}")
        self.check(stale_sessions == 0, f"{stale_sessions} sessions left open after recovery")
        self.check(stale_segments == 0, f"{stale_segments} stale recording rows after recovery")
        self.check(report["killed_session_rings_left"] == 0,
                   f"{report['killed_session_rings_left']} frame rings of killed sessions still allocated")

    def console_closed(self) -> None:
        if not self.console.exists():
            if self.args.skip_console:
                self.results["console_closed"] = "skipped (--skip-console)"
                return
            raise Fail(f"console binary {self.console.name} not found (build target fovea or pass --skip-console)")
        png = self.work / "console.png"
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", FOVEA_SCREENSHOT=str(png), FOVEA_SCREENSHOT_DELAY_MS="6000",
                   FOVEA_CORE_BIN=str(self.core))
        core_pid = json.loads((self.data / "core.json").read_text())["pid"]
        out = open(self.work / "console.log", "ab")
        t0 = time.monotonic()
        proc = subprocess.Popen([str(self.console), "--data-dir", str(self.data)], stdout=out, stderr=subprocess.STDOUT, env=env)
        try:
            rc = proc.wait(90)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise Fail("console did not exit within 90 s")
        exited_ms = utc_ms()
        report = {"console_exit_code": rc, "console_run_s": round(time.monotonic() - t0, 1),
                  "screenshot_bytes": png.stat().st_size if png.exists() else 0}
        self.check(rc == 0 and report["screenshot_bytes"] > 0, f"console screenshot run failed: {report}")
        alive = self.core_proc.poll() is None and json.loads((self.data / "core.json").read_text())["pid"] == core_pid
        report["core_still_running"] = alive
        self.check(alive, "core stopped or was replaced when the console exited")
        deadline = time.monotonic() + self.args.segment_seconds * 2 + 20
        pending = set(self.cams)
        while pending and time.monotonic() < deadline:
            for cam in list(pending):
                if any(s["state"] == "finalized" and s["finalized_utc_ms"] > exited_ms for s in self.segments(cam)):
                    pending.discard(cam)
            time.sleep(1)
        report["cameras_finalized_after_console_exit"] = len(self.cams) - len(pending)
        sts = self.statuses()
        report["all_online_recording"] = all(self.online(s) and s["recording"] == "recording" for s in sts)
        self.results["console_closed"] = report
        self.log(f"console closed: {report}")
        self.check(not pending and report["all_online_recording"], "recording did not continue after the console exited")

    def disk_floor(self) -> None:
        self.stop_core()
        free_mb = shutil.disk_usage(self.data).free // MIB
        floor_mb = free_mb + self.args.floor_margin_mb
        started_ms = utc_ms()
        self.log(f"disk floor: restart with FOVEA_MIN_FREE_MB={floor_mb} (free {free_mb} MB)")
        self.start_core("3", {"FOVEA_MIN_FREE_MB": str(floor_mb)})
        self.wait_all(lambda s: self.online(s) and s["recording"] == "paused_disk", 90, "online with recording paused_disk")
        time.sleep(10)
        sts = self.statuses()
        rings = [self.probe_ring(s["frame_ring"]["name"], 2.0) for s in sts]
        new_rows = sum(1 for c in self.cams for s in self.segments(c) if s["created_utc_ms"] >= started_ms)
        storage = self.api("GET", "/v1/storage")
        with self.db() as conn:
            floor_deletions = conn.execute("SELECT COUNT(*) FROM recording_segments WHERE delete_reason='disk_floor'").fetchone()[0]
        report = {"floor_mb": floor_mb, "free_mb": free_mb, "states": [(s["state"], s["recording"]) for s in sts],
                  "fps": [s["fps_new"] for s in sts], "ring_probe": rings, "segment_rows_created": new_rows,
                  "storage_floor_unreachable": storage["floor_unreachable"], "storage_deleted_last_run": storage["deleted_last_run"],
                  "disk_floor_deletions": floor_deletions}
        self.check(all(s["recording"] == "paused_disk" and self.online(s) for s in sts), "recording not paused_disk with live frames")
        self.check(all(r["frames"] >= self.args.min_fps for r in rings), f"live ring frames while paused: {rings}")
        self.check(new_rows == 0, f"{new_rows} segments opened while below the floor")
        self.check(storage["floor_bytes"] == floor_mb * MIB and storage["free_bytes"] < storage["floor_bytes"],
                   f"/v1/storage floor {storage['floor_bytes']} free {storage['free_bytes']}")
        self.check(storage["floor_unreachable"] and floor_deletions == 0,
                   "retention deleted recordings for a floor it could not reach")
        self.stop_core()
        resumed_ms = utc_ms()
        t0 = time.monotonic()
        self.start_core("4")
        self.wait_all(lambda s: self.online(s) and s["recording"] == "recording" and s["current_segment_id"], 90,
                      "recording resumed after the floor was lowered")
        report["resume_s"] = round(time.monotonic() - t0, 2)
        report["resumed_rows"] = [sum(1 for s in self.segments(c) if s["created_utc_ms"] >= resumed_ms) for c in self.cams]
        self.results["disk_floor"] = report
        self.log(f"disk floor: {report}")
        self.check(all(n > 0 for n in report["resumed_rows"]), "no new segment after recording resumed")

    def retention(self) -> None:
        a = self.args
        self.stop_core()
        with self.db(readonly=False) as conn:
            held_id, held_path, held_start = conn.execute(
                "SELECT id, path, start_utc_ms FROM recording_segments WHERE camera_id=? AND state='finalized'"
                " ORDER BY start_utc_ms, id LIMIT 1", (self.cams[0],)).fetchone()
            conn.execute("INSERT INTO evidence_holds(segment_id, until_utc_ms, reason) VALUES(?, 0, 'verify_m2')", (held_id,))
            crashed_id, crashed_path = conn.execute(
                "SELECT id, path FROM recording_segments WHERE camera_id=? AND state='finalized'"
                " ORDER BY start_utc_ms DESC LIMIT 1", (self.cams[1 % len(self.cams)],)).fetchone()
            conn.execute("UPDATE recording_segments SET state='deleted', deleted_utc_ms=?, delete_reason='age', purged_utc_ms=0"
                         " WHERE id=?", (utc_ms(), crashed_id))
        report: dict = {"retention_seconds": a.retention_seconds,
                        "crash_left_file": Path(crashed_path).exists()}
        started_ms = utc_ms()
        self.log(f"retention: hold on the oldest segment of camera 0, simulated crash after a row commit on camera 1, "
                 f"restart with FOVEA_RETENTION_SECONDS={a.retention_seconds}")
        self.start_core("5", {"FOVEA_RETENTION_SECONDS": str(a.retention_seconds)})
        storage = self.wait_storage_run(started_ms)
        run_ms = storage["last_run_utc_ms"]
        cutoff = run_ms - a.retention_seconds * 1000
        report["recovery_log"] = self.core_log_line("5", "recovery:")
        report["crashed_file_removed"] = not Path(crashed_path).exists()
        with self.db() as conn:
            report["crashed_row_purged"] = conn.execute("SELECT purged_utc_ms FROM recording_segments WHERE id=?",
                                                        (crashed_id,)).fetchone()[0] > 0
            by_age = conn.execute("SELECT id, path FROM recording_segments WHERE delete_reason='age' AND deleted_utc_ms=?",
                                  (run_ms,)).fetchall()
            too_old = conn.execute(
                f"SELECT id FROM recording_segments WHERE state IN ('finalized','damaged') AND ({CONTENT_END_SQL})<?",
                (cutoff,)).fetchall()
            held_state = conn.execute("SELECT state FROM recording_segments WHERE id=?", (held_id,)).fetchone()[0]
            audited = {r[0] for r in conn.execute("SELECT target FROM audit_log WHERE actor='retention' AND action='segment.delete'")}
            core_deleted = {r[0] for r in conn.execute("SELECT id FROM recording_segments WHERE delete_reason!='' AND id!=?",
                                                       (crashed_id,))}
        cam0 = next(p for p in storage["per_camera"] if p["camera_id"] == self.cams[0])
        report.update({
            "deleted_last_run": storage["deleted_last_run"], "deleted_mb_last_run": round(storage["deleted_bytes_last_run"] / MIB, 1),
            "held_last_run": storage["held_last_run"], "age_rows_deleted_in_run": len(by_age),
            "files_left_for_age_deleted": sum(1 for _, p in by_age if Path(p).exists()),
            "finalized_older_than_cutoff": [r[0] for r in too_old], "held_state": held_state,
            "held_file_exists": Path(held_path).exists(), "audit_rows_match": audited == core_deleted,
            "camera0_oldest_is_held": cam0["oldest_utc_ms"] == held_start,
            "storage_per_camera": [{k: p[k] for k in ("bytes", "segments", "oldest_utc_ms", "retention_days", "max_bytes",
                                                      "age_limit_ms")} for p in storage["per_camera"]]})
        self.check(report["crash_left_file"] and report["crashed_file_removed"] and report["crashed_row_purged"],
                   "startup recovery did not remove the file of a deleted row")
        self.check(storage["deleted_last_run"] > 0 and storage["deleted_last_run"] == len(by_age),
                   f"deleted_last_run {storage['deleted_last_run']} vs age rows {len(by_age)}")
        self.check(report["files_left_for_age_deleted"] == 0, "files of deleted segments still on disk")
        self.check(report["finalized_older_than_cutoff"] == [held_id], f"segments older than the cutoff: {too_old}")
        self.check(held_state == "finalized" and report["held_file_exists"] and storage["held_last_run"] >= 1,
                   "segment with an evidence hold was deleted")
        self.check(report["audit_rows_match"], "audit rows do not match deleted segments")
        self.check(report["camera0_oldest_is_held"], "camera 0 oldest_utc_ms is not the held segment")
        self.check(all(p["age_limit_ms"] == a.retention_seconds * 1000 for p in storage["per_camera"]),
                   "age limit override not reported")

        updated = self.api("PUT", f"/v1/cameras/{self.cams[0]}", {"retention_days": 3})
        report["put_retention_days"] = updated.get("retention_days")
        report["put_invalid_retention_status"] = self.api_status("PUT", f"/v1/cameras/{self.cams[0]}", {"retention_days": 0})
        last_cam = self.cams[-1]
        before_ms = utc_ms()
        self.api("PUT", f"/v1/cameras/{last_cam}", {"max_bytes": 1})
        storage2 = self.wait_storage_run(before_ms)
        with self.db() as conn:
            capped = conn.execute("SELECT COUNT(*) FROM recording_segments WHERE camera_id=? AND delete_reason='max_bytes'",
                                  (last_cam,)).fetchone()[0]
            left = conn.execute("SELECT COUNT(*) FROM recording_segments WHERE camera_id=? AND state IN ('finalized','damaged')"
                                " AND finalized_utc_ms<=?", (last_cam, storage2["last_run_utc_ms"])).fetchone()[0]
        report.update({"max_bytes_deleted": capped, "max_bytes_left_before_run": left,
                       "max_bytes_deleted_last_run": storage2["deleted_last_run"]})
        self.check(report["put_retention_days"] == 3 and report["put_invalid_retention_status"] == 400,
                   f"PUT retention_days: {report['put_retention_days']}, invalid -> {report['put_invalid_retention_status']}")
        self.check(capped > 0 and left == 0, f"max_bytes=1 left {left} segments, deleted {capped}")
        self.results["retention"] = report
        self.log(f"retention: {json.dumps({k: v for k, v in report.items() if k != 'storage_per_camera'})}")
        self.stop_core()

    def wait_storage_run(self, after_ms: int) -> dict:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                storage = self.api("GET", "/v1/storage")
                if storage["last_run_utc_ms"] >= after_ms:
                    return storage
            except Fail:
                pass
            time.sleep(0.25)
        raise Fail("retention did not run within 30 s")

    def run(self) -> None:
        self.setup()
        self.soak()
        self.crash_restart()
        self.console_closed()
        self.disk_floor()
        self.retention()
        if hasattr(os, "getloadavg"):
            self.results["loadavg_end"] = [round(x, 2) for x in os.getloadavg()]

    def cleanup(self) -> None:
        if self.core_proc and self.core_proc.poll() is None:
            self.core_proc.terminate()
            try:
                self.core_proc.wait(15)
            except subprocess.TimeoutExpired:
                self.core_proc.kill()
        self.stop_sources()
        if not self.args.keep and self.work.exists():
            for tag in ("recordings", "quarantine"):
                shutil.rmtree(self.data / tag, ignore_errors=True)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--bin-dir", required=True)
    p.add_argument("--flat", action="store_true", help="binaries are all directly in --bin-dir")
    p.add_argument("--work-dir", default="build/verify-m2-py")
    p.add_argument("--inputs", type=int, default=4)
    p.add_argument("--minutes", type=float, default=10)
    p.add_argument("--width", type=int, default=640)
    p.add_argument("--height", type=int, default=360)
    p.add_argument("--fps", type=int, default=25)
    p.add_argument("--min-fps", type=float, default=20)
    p.add_argument("--segment-seconds", type=int, default=30)
    p.add_argument("--sample-seconds", type=int, default=30)
    p.add_argument("--retention-seconds", type=int, default=120)
    p.add_argument("--min-free-mb", type=int, default=50)
    p.add_argument("--floor-margin-mb", type=int, default=100_000,
                   help="the disk floor check sets the floor this far above the free space")
    p.add_argument("--rtsp-port-base", type=int, default=8654)
    p.add_argument("--skip-console", action="store_true")
    p.add_argument("--keep", action="store_true", help="keep recordings under --work-dir")
    p.add_argument("--report", help="JSON report path (the log is written next to it)")
    args = p.parse_args()
    if args.inputs < 2:
        p.error("--inputs must be at least 2")
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    report = Path(args.report) if args.report else REPO / "docs" / "verification" / f"m2-{stamp}.json"
    v = Verifier(args)
    code = 0
    try:
        v.run()
    except Fail as e:
        v.failures.append(str(e))
        v.log(f"ABORT: {e}")
    except KeyboardInterrupt:
        v.failures.append("interrupted")
    finally:
        v.cleanup()
    v.results["failures"] = v.failures
    v.results["result"] = "PASS" if not v.failures else "FAIL"
    v.log(v.results["result"] + ("" if not v.failures else f": {len(v.failures)} failed checks"))
    if v.failures:
        code = 1
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(v.results, indent=1))
    report.with_suffix(".log").write_text("\n".join(v.log_lines) + "\n")
    print(f"report: {rel(report)}")
    return code


if __name__ == "__main__":
    if os.name != "nt":
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    sys.exit(main())
