#!/usr/bin/env python3
"""Cross-platform M4 verification (stdlib only): fovea-core supervising the
real model worker, three local videos imported into three cameras through
POST /v1/imports, and the checks of docs/M4_DESIGN.md "Done when":

  1. imports: each video becomes finalized "imported" segments of its camera
     (session capture_clock "imported") at the given start_utc_ms
  2. restart mid-index: the core is killed without cleanup after the walking
     video's single 54-sample job stored its first 32-frame batch; after a
     restart the records of that attempt are deleted, the job runs again,
     every camera reaches coverage 1.0 and no sampled instant has two live
     records
  3. queries: each video's positive query ranks that video's camera first with
     the siglip2-b16-224 index version; camera and time filters hold; the
     session and the representative thumbnail can be fetched again
  4. playback at a result's representative utc delivers frames
  5. retention: the core restarts with FOVEA_RETENTION_SECONDS so the
     classroom camera's footage (imported three days in the past) expires;
     its segments and records are deleted, its thumbnails are gone, the stored
     session loses its ranges and a new query returns none of its footage,
     while the other cameras still answer

Usage:
  python3 scripts/verify_m4.py --bin-dir build/macos-dev \\
      --classroom classroom.mp4 --walking lot-walking.mp4 --whitecar lot-whitecar.mp4
The worker command defaults to the development venv (worker/.venv) running
python -m fovea_worker.cli and is passed to the core as FOVEA_WORKER_CMD. The
JSON report and the log go to docs/verification/m4-<UTC stamp>.{json,log}
unless --report is given. File paths never appear in the report.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from multiprocessing import shared_memory
from pathlib import Path

EXE = ".exe" if os.name == "nt" else ""
REPO = Path(__file__).resolve().parent.parent
RING_HEADER_BYTES = 4096
FRAME_HEADER = struct.Struct("<QQQQIIII16sII")
RING_MAGIC = 0x46564652
HOUR_MS = 3_600_000
DAY_MS = 24 * HOUR_MS
VERSION_NAME = "siglip2-b16-224"
DEFAULT_QUERIES = {
    "classroom": "people sitting at desks in a classroom",
    "walking": "a person walking across an empty parking lot",
    "whitecar": "a white car on dark wet asphalt",
}


def utc_ms() -> int:
    return int(time.time() * 1000)


def rel(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return path.name


def pid_alive(pid: int) -> bool:
    if os.name == "nt":
        out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout
        return str(pid) in out
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


class Fail(Exception):
    pass


class Verifier:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        bin_dir = Path(args.bin_dir).resolve()
        self.core = bin_dir / f"fovea-core{EXE}" if args.flat else self._find(bin_dir, ["src/core", "."], f"fovea-core{EXE}")
        self.work = Path(args.work_dir).resolve() if args.work_dir else Path(tempfile.gettempdir()) / "fovea-verify-m4"
        self.data = self.work / "data"
        self.db_path = self.data / "fovea.sqlite"
        self.core_proc: subprocess.Popen | None = None
        self.api_port = 0
        self.token = ""
        self.log_lines: list[str] = []
        self.failures: list[str] = []
        now = utc_ms()
        # The classroom footage is three days old so a one-day retention override expires only it.
        # The walking video stays one segment so its job needs two embed batches.
        self.videos = {
            "classroom": {"path": Path(args.classroom), "start_utc_ms": now - 3 * DAY_MS, "segment_seconds": args.segment_seconds},
            "walking": {"path": Path(args.walking), "start_utc_ms": now - 2 * HOUR_MS, "segment_seconds": 60},
            "whitecar": {"path": Path(args.whitecar), "start_utc_ms": now - HOUR_MS, "segment_seconds": args.segment_seconds},
        }
        self.queries = dict(DEFAULT_QUERIES)
        for item in args.query or []:
            key, _, text = item.partition("=")
            if key not in self.queries or not text:
                raise SystemExit(f"--query expects one of {sorted(self.queries)}=TEXT, got {item!r}")
            self.queries[key] = text
        self.cams: dict[str, str] = {}
        self.names: dict[str, str] = {}
        self.results: dict = {"platform": sys.platform, "machine": platform.machine(), "cpu_count": os.cpu_count(),
                              "index_version_name": VERSION_NAME, "queries": self.queries}

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
        self.log(("ok    " if ok else "FAIL  ") + msg)
        if not ok:
            self.failures.append(msg)
        return ok

    def request(self, method: str, path: str, body: dict | None = None, timeout: float = 30.0) -> tuple[int, bytes, str]:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.api_port}{path}", data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read(), r.headers.get("Content-Type", "")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), e.headers.get("Content-Type", "")
        except (urllib.error.URLError, OSError) as e:
            raise Fail(f"{method} {path} -> {e}") from e

    def api(self, method: str, path: str, body: dict | None = None, timeout: float = 30.0):
        status, payload, _ = self.request(method, path, body, timeout)
        if status >= 300:
            raise Fail(f"{method} {path} -> {status} {payload[:300]!r}")
        return json.loads(payload) if payload else None

    def wait(self, what: str, timeout: float, probe, interval: float = 0.5):
        deadline = time.monotonic() + timeout
        last_error = ""
        while time.monotonic() < deadline:
            try:
                value = probe()
                if value:
                    return value
            except Fail as e:
                last_error = str(e)
            time.sleep(interval)
        raise Fail(f"timeout after {timeout:.0f} s waiting for {what}" + (f" ({last_error})" if last_error else ""))

    def query(self, sql: str, params: tuple = ()) -> list[tuple]:
        with sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=10) as conn:
            return conn.execute(sql, params).fetchall()

    # processes

    def worker_cmd(self) -> str:
        if self.args.worker_cmd:
            return self.args.worker_cmd
        python = REPO / "worker" / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        return f'"{python}" -m fovea_worker.cli'

    def worker_pids(self) -> list[int]:
        try:
            return [int(json.loads((self.data / "worker.json").read_text())["pid"])]
        except (OSError, ValueError, KeyError):
            return []

    def start_core(self, tag: str, retention_seconds: int = 0) -> None:
        info = self.data / "core.json"
        if info.exists():
            info.unlink()
        env = dict(os.environ, FOVEA_WORKER_CMD=self.worker_cmd(), FOVEA_MIN_FREE_MB=str(self.args.min_free_mb))
        env.pop("FOVEA_RETENTION_SECONDS", None)
        if retention_seconds:
            env["FOVEA_RETENTION_SECONDS"] = str(retention_seconds)
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
            self.core_proc.wait(60)
        except subprocess.TimeoutExpired:
            raise Fail("core did not exit within 60 s of shutdown")

    def kill_core(self) -> None:
        worker = self.worker_pids()
        self.core_proc.kill()
        self.core_proc.wait(10)
        deadline = time.monotonic() + 20
        while worker and pid_alive(worker[0]) and time.monotonic() < deadline:
            time.sleep(0.2)
        if worker and pid_alive(worker[0]):
            raise Fail("the worker outlived the killed core")

    def core_log(self, tag: str) -> str:
        try:
            return (self.work / f"core-{tag}.log").read_text(errors="replace")
        except OSError:
            return ""

    # observations

    def index(self, storage: bool = False) -> dict:
        return self.api("GET", "/v1/index" + ("?storage=1" if storage else ""))

    def fully_indexed(self) -> dict | None:
        status = self.index()
        cams = {c["camera_id"]: c for c in status["cameras"]}
        q = status["queue"]
        if status.get("active_index_version") != VERSION_NAME or not status.get("index_version"):
            return None
        if q["queued"] or q["running"] or q["failed"]:
            return None
        if all(cams.get(cid, {}).get("frames_expected", 0) > 0 and cams[cid]["coverage_ratio"] >= 1.0 for cid in self.cams.values()):
            return status
        return None

    def search(self, query: str, **filters) -> dict:
        body = {"query": query, "limit": self.args.limit}
        body.update(filters)
        return self.api("POST", "/v1/search", body, timeout=60)

    def probe_ring(self, name: str, seconds: float) -> int:
        shm = shared_memory.SharedMemory(name=f"Local\\{name}" if os.name == "nt" else name, create=False)
        if os.name != "nt":
            try:
                from multiprocessing import resource_tracker
                resource_tracker.unregister(shm._name, "shared_memory")
            except Exception:
                pass
        seen: set[int] = set()
        try:
            buf = shm.buf
            magic, _ver, slots, slot_bytes = struct.unpack_from("<IIII", buf, 0)
            if magic != RING_MAGIC:
                raise Fail("frame ring magic mismatch")
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                latest = struct.unpack_from("<Q", buf, 32)[0]
                if latest:
                    off = RING_HEADER_BYTES + (FRAME_HEADER.size + slot_bytes) * ((latest // 2) % slots)
                    if FRAME_HEADER.unpack_from(buf, off)[0] == latest:
                        seen.add(latest)
                time.sleep(0.005)
            del buf
        finally:
            shm.close()
        return len(seen)

    def offset_s(self, key: str, utc: int) -> float:
        return round((utc - self.videos[key]["start_utc_ms"]) / 1000, 1)

    def describe(self, result: dict) -> dict:
        key = self.names.get(result["camera_id"], "?")
        return {"camera": key, "start_s": self.offset_s(key, result["start_utc_ms"]) if key in self.videos else None,
                "end_s": self.offset_s(key, result["end_utc_ms"]) if key in self.videos else None,
                "relevance": result["relevance"], "samples": len(result["samples"]), "evidence_state": result["evidence_state"]}

    # phases

    def setup(self) -> None:
        for key, video in self.videos.items():
            if not video["path"].is_file():
                raise Fail(f"missing input video for --{key}")
        if not self.core.exists():
            raise Fail(f"missing {self.core.name} under --bin-dir")
        if self.work.exists():
            shutil.rmtree(self.work)
        self.data.mkdir(parents=True)
        if hasattr(os, "getloadavg"):
            self.results["loadavg_start"] = [round(x, 2) for x in os.getloadavg()]
        self.start_core("1")
        for key in self.videos:
            cam = self.api("POST", "/v1/cameras", {"code": f"M4-{key.upper()}", "name": f"M4 {key}", "group_name": "M4", "kind": "file",
                                                   "main_url": self.videos[key]["path"].name, "enabled": False,
                                                   "segment_seconds": self.videos[key]["segment_seconds"]})
            self.check(cam["index_enabled"] is True, f"camera {key} has index_enabled true by default")
            self.cams[key] = cam["id"]
            self.names[cam["id"]] = key

    def phase_import(self) -> None:
        refused = self.request("POST", "/v1/imports", {"path": str(self.work / "missing.mp4"), "camera_id": self.cams["walking"],
                                                        "start_utc_ms": utc_ms()})
        self.check(refused[0] == 400 and b"file_not_found" in refused[1], f"import of a missing file refused ({refused[0]})")
        imports = {}
        started = time.monotonic()
        for key, video in self.videos.items():
            record = self.api("POST", "/v1/imports", {"path": str(video["path"].resolve()), "camera_id": self.cams[key],
                                                      "start_utc_ms": video["start_utc_ms"]}, timeout=60)
            imports[key] = record["id"]
        overlap = self.request("POST", "/v1/imports", {"path": str(self.videos["walking"]["path"].resolve()),
                                                        "camera_id": self.cams["walking"],
                                                        "start_utc_ms": self.videos["walking"]["start_utc_ms"] + 5000}, timeout=60)
        self.check(overlap[0] == 409, f"a second import over the same footage refused ({overlap[0]})")
        done = self.wait("imports", 300, lambda: (lambda rows: rows if all(r["state"] in ("done", "failed") for r in rows) else None)(
            [self.api("GET", f"/v1/imports/{i}") for i in imports.values()]))
        out = {}
        for key, record in zip(imports, done):
            segments = self.api("GET", f"/v1/cameras/{self.cams[key]}/segments")
            session = self.api("GET", f"/v1/cameras/{self.cams[key]}/sessions")[0]
            first = min(segments, key=lambda s: s["start_utc_ms"]) if segments else {}
            last = max(segments, key=lambda s: s["end_utc_ms"]) if segments else {}
            out[key] = {"state": record["state"], "error": record["error"], "codec": record["codec"], "duration_s": record["duration_ns"] / 1e9,
                        "segments": record["segments"], "bytes": record["bytes"], "capture_clock": session["capture_clock"],
                        "import_ms": record["finished_utc_ms"] - record["started_utc_ms"],
                        "first_segment_offset_ms": first.get("start_utc_ms", 0) - self.videos[key]["start_utc_ms"],
                        "last_segment_end_s": round((last.get("end_utc_ms", 0) - self.videos[key]["start_utc_ms"]) / 1000, 3)}
            wanted = 1 if self.videos[key]["segment_seconds"] >= out[key]["duration_s"] else 2
            self.check(record["state"] == "done" and record["segments"] == len(segments) and len(segments) >= wanted
                       and all(s["state"] == "finalized" for s in segments),
                       f"{key}: import done with {len(segments)} finalized segments")
            self.check(session["capture_clock"] == "imported" and abs(out[key]["first_segment_offset_ms"]) <= 100
                       and abs(out[key]["last_segment_end_s"] - out[key]["duration_s"]) <= 0.5,
                       f"{key}: imported session anchored at start_utc_ms, footage {out[key]['last_segment_end_s']} s of "
                       f"{out[key]['duration_s']:.2f} s")
        self.results["imports"] = out
        self.results["imports_wall_s"] = round(time.monotonic() - started, 1)
        self.log(f"imports: {json.dumps(out)}")

    def phase_restart_mid_index(self) -> None:
        walking = self.cams["walking"]

        def partial(status: dict) -> dict | None:
            job = status.get("running_job") or {}
            return status if job.get("camera_id") == walking and 0 < job.get("frames_stored", 0) < job.get("frames_sampled", 0) else None

        interrupted = self.wait("the walking job to store its first batch", self.args.index_timeout, lambda: partial(self.index()),
                                interval=0.02)
        job = interrupted["running_job"]
        self.kill_core()
        at_kill = self.query("SELECT generation, state FROM index_jobs WHERE id=?", (job["id"],))[0]
        stored_rows = self.query("SELECT COUNT(*) FROM embedding_records WHERE job_id=? AND generation=? AND deleted=0",
                                 (job["id"], at_kill[0]))[0][0]
        state = {"queue_at_kill": interrupted["queue"], "job_frames_sampled": job["frames_sampled"], "job_frames_stored": job["frames_stored"],
                 "job_generation_at_kill": at_kill[0], "job_state_at_kill": at_kill[1], "job_live_rows_at_kill": stored_rows}
        self.log(f"core killed mid-job: {state}")
        started = time.monotonic()
        self.start_core("2")
        status = self.wait("coverage 1.0 on every camera after the restart", self.args.index_timeout, self.fully_indexed, interval=1.0)
        state["resume_to_full_coverage_s"] = round(time.monotonic() - started, 1)
        version = status["index_version"]
        duplicates = self.query("SELECT COUNT(*) FROM (SELECT segment_id, utc_ms FROM embedding_records WHERE deleted=0 AND index_version=?"
                                " GROUP BY segment_id, utc_ms HAVING COUNT(*)>1)", (version,))[0][0]
        live = self.query("SELECT COUNT(*) FROM embedding_records WHERE deleted=0 AND index_version=?", (version,))[0][0]
        expected = sum(c["frames_expected"] for c in status["cameras"] if c["camera_id"] in self.cams.values())
        after = self.query("SELECT generation, state, frames_indexed, frames_expected FROM index_jobs WHERE id=?", (job["id"],))[0]
        dropped = self.query("SELECT COUNT(*) FROM embedding_records WHERE job_id=? AND generation=? AND deleted=1",
                             (job["id"], at_kill[0]))[0][0]
        state.update({"index_version": version, "live_records": live, "frames_expected": expected, "duplicate_instants": duplicates,
                      "job_generation_after": after[0], "job_state_after": after[1], "job_frames_indexed_after": after[2],
                      "interrupted_attempt_rows_deleted": dropped, "recovery_logged": "interrupted jobs requeued" in self.core_log("2")})
        self.results["restart_mid_index"] = state
        self.log(f"restart mid-index: {state}")
        self.check(at_kill[1] == "running" and stored_rows == job["frames_stored"],
                   f"the core was killed with {stored_rows} of {job['frames_sampled']} records of a running job stored")
        self.check(dropped == stored_rows and state["recovery_logged"], f"restart deleted the interrupted attempt's {dropped} records")
        self.check(after[0] > at_kill[0] and after[1] == "done" and after[2] == after[3],
                   f"the job ran again (generation {at_kill[0]} -> {after[0]}) and indexed {after[2]} of {after[3]}")
        self.check(duplicates == 0, f"no sampled instant has two live records ({duplicates})")
        self.check(live == expected, f"live records ({live}) equal expected samples ({expected}) after resuming")

        status = self.index(storage=True)
        active = next(v for v in status["versions"] if v["active"])
        self.results["index"] = {"version": version, "model": status["model"], "model_revision": active["model_revision"],
                                 "dims": active["dims"], "throughput": status["throughput"], "storage": active.get("storage"),
                                 "cameras": {self.names.get(c["camera_id"], "?"): {k: c[k] for k in ("frames_expected", "frames_indexed", "coverage_ratio")}
                                             for c in status["cameras"]}}
        self.log(f"index: {json.dumps(self.results['index'])}")

    def phase_queries(self) -> None:
        out = {}
        self.sessions = {}
        for key, text in self.queries.items():
            response = self.search(text)
            results = response["results"]
            top = self.describe(results[0]) if results else None
            out[key] = {"query": text, "index_version_name": response["index_version_name"], "stats": response["stats"],
                        "top": [self.describe(r) for r in results[:5]]}
            self.sessions[key] = response
            self.check(response["index_version_name"] == VERSION_NAME and response["scoring"] == "embedding_similarity",
                       f"{key}: answered by {response['index_version_name']} as similarity only")
            self.check(bool(top) and top["camera"] == key, f"{key}: '{text}' ranks its video first (top: {top})")
            self.check(response["stats"]["coverage_ratio"] >= 1.0, f"{key}: coverage {response['stats']['coverage_ratio']}")
        self.results["queries_result"] = out

        walking = self.cams["walking"]
        filtered = self.search(self.queries["classroom"], camera_ids=[walking])
        self.check(bool(filtered["results"]) and all(r["camera_id"] == walking for r in filtered["results"]),
                   f"camera filter keeps only that camera ({len(filtered['results'])} results)")
        start = self.videos["walking"]["start_utc_ms"]
        window = self.search(self.queries["walking"], from_utc_ms=start + 10_000, to_utc_ms=start + 20_000)
        self.check(bool(window["results"]) and all(r["camera_id"] == walking and start + 10_000 <= s["utc_ms"] <= start + 20_000
                                                   for r in window["results"] for s in r["samples"]),
                   f"time filter keeps samples inside the window ({window['stats']['samples_scanned']} scanned)")
        bad = self.request("POST", "/v1/search", {"query": "x", "camera_ids": ["no-such-camera"]})
        self.check(bad[0] == 400, f"unknown camera refused ({bad[0]})")

        session = self.sessions["walking"]
        again = self.api("GET", f"/v1/search/{session['session_id']}")
        self.check([r["start_utc_ms"] for r in again["results"]] == [r["start_utc_ms"] for r in session["results"]],
                   "stored session returns the same ranges")
        record = session["results"][0]["representative"]["record_id"]
        status, payload, content_type = self.request("GET", f"/v1/search/thumbnails/{record}")
        self.check(status == 200 and content_type.startswith("image/jpeg") and payload[:2] == b"\xff\xd8",
                   f"representative thumbnail is a JPEG ({status}, {len(payload)} bytes)")
        self.results["latency_ms"] = {key: {k: q["stats"][k] for k in ("embed_ms", "scan_ms", "total_ms")} for key, q in out.items()}

    def phase_playback(self) -> None:
        rep = self.sessions["walking"]["results"][0]["representative"]
        pb = self.api("POST", "/v1/playback", {"camera_id": self.cams["walking"], "at_utc_ms": rep["utc_ms"]}, timeout=30)
        self.api("POST", f"/v1/playback/{pb['id']}/play")
        time.sleep(0.5)
        frames = self.probe_ring(pb["frame_ring"]["name"], 2.0)
        state = self.api("GET", f"/v1/playback/{pb['id']}")
        self.api("DELETE", f"/v1/playback/{pb['id']}")
        self.results["playback"] = {"offset_s": self.offset_s("walking", rep["utc_ms"]), "frames_in_2s": frames,
                                    "segment_start_offset_s": self.offset_s("walking", state["start_utc_ms"]),
                                    "position_s": round(state["position_ns"] / 1e9, 2)}
        self.log(f"playback: {self.results['playback']}")
        self.check(frames >= 5, f"playback at the result's utc delivered {frames} frames in 2 s")

    def phase_retention(self) -> None:
        classroom = self.cams["classroom"]
        session = self.sessions["classroom"]
        records = [s["record_id"] for r in session["results"] if r["camera_id"] == classroom for s in r["samples"]]
        thumbs = [row[0] for row in self.query("SELECT thumbnail_path FROM embedding_records WHERE camera_id=? AND deleted=0", (classroom,))]
        self.stop_core()
        self.start_core("3", retention_seconds=DAY_MS // 1000)
        self.wait("classroom segments deleted", 60, lambda: not self.api("GET", f"/v1/cameras/{classroom}/segments"))
        live = self.query("SELECT COUNT(*) FROM embedding_records WHERE camera_id=? AND deleted=0", (classroom,))[0][0]
        others = self.query("SELECT COUNT(*) FROM embedding_records WHERE camera_id!=? AND deleted=0", (classroom,))[0][0]
        stored = self.api("GET", f"/v1/search/{session['session_id']}")
        thumb_status = self.request("GET", f"/v1/search/thumbnails/{records[0]}")[0] if records else 0
        self.wait("the worker", 180, lambda: self.api("GET", "/v1/analysis")["worker"]["state"] == "ready")
        fresh = self.wait("a query after the restart", 120, lambda: self.search(self.queries["classroom"]), interval=2.0)
        walking = self.search(self.queries["walking"])
        state = {"classroom_live_records": live, "other_live_records": others,
                 "thumbnails_left": sum(1 for t in thumbs if os.path.exists(t)), "thumbnails_before": len(thumbs),
                 "stored_session_classroom_ranges": sum(1 for r in stored["results"] if r["camera_id"] == classroom),
                 "thumbnail_status": thumb_status,
                 "new_query_classroom_ranges": sum(1 for r in fresh["results"] if r["camera_id"] == classroom),
                 "new_query_top": self.describe(fresh["results"][0]) if fresh["results"] else None,
                 "walking_top": self.describe(walking["results"][0]) if walking["results"] else None,
                 "retention_audit": self.query("SELECT COUNT(*) FROM audit_log WHERE action='segment.delete' AND detail LIKE ?",
                                               (f"%{classroom}%",))[0][0]}
        self.results["retention"] = state
        self.log(f"retention: {state}")
        self.check(live == 0 and others > 0, f"retention deleted the classroom records ({live} live) and kept the others ({others})")
        self.check(state["thumbnails_before"] > 0 and state["thumbnails_left"] == 0, "the classroom thumbnails were removed")
        self.check(state["stored_session_classroom_ranges"] == 0, "the stored session no longer lists classroom ranges")
        self.check(thumb_status == 404, f"a deleted record's thumbnail answers 404 ({thumb_status})")
        self.check(state["new_query_classroom_ranges"] == 0, "a new query returns no classroom footage")
        self.check((state["walking_top"] or {}).get("camera") == "walking", "the walking query still ranks its video first")

    def run(self) -> None:
        self.setup()
        self.phase_import()
        self.phase_restart_mid_index()
        self.phase_queries()
        self.phase_playback()
        self.phase_retention()
        if hasattr(os, "getloadavg"):
            self.results["loadavg_end"] = [round(x, 2) for x in os.getloadavg()]

    def cleanup(self) -> None:
        if self.core_proc and self.core_proc.poll() is None:
            try:
                self.stop_core()
            except Fail:
                self.core_proc.kill()
        for pid in self.worker_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        if not self.args.keep and self.work.exists():
            shutil.rmtree(self.work, ignore_errors=True)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--bin-dir", required=True)
    p.add_argument("--flat", action="store_true", help="binaries are all directly in --bin-dir")
    p.add_argument("--classroom", required=True, help="classroom video (people seated at desks)")
    p.add_argument("--walking", required=True, help="overhead parking lot with a person walking")
    p.add_argument("--whitecar", required=True, help="parking lot asphalt with a white car")
    p.add_argument("--query", action="append", help="override a positive query: classroom|walking|whitecar=TEXT")
    p.add_argument("--worker-cmd", help="FOVEA_WORKER_CMD for the core (default: worker/.venv python -m fovea_worker.cli)")
    p.add_argument("--work-dir", help="data and logs (default: <temp>/fovea-verify-m4, removed afterwards unless --keep)")
    p.add_argument("--segment-seconds", type=int, default=10)
    p.add_argument("--limit", type=int, default=10)
    p.add_argument("--index-timeout", type=float, default=900)
    p.add_argument("--min-free-mb", type=int, default=256)
    p.add_argument("--keep", action="store_true", help="keep --work-dir")
    p.add_argument("--report", help="JSON report path (the log is written next to it)")
    args = p.parse_args()
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    report = Path(args.report) if args.report else REPO / "docs" / "verification" / f"m4-{stamp}.json"
    v = Verifier(args)
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
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(v.results, indent=1, default=str))
    report.with_suffix(".log").write_text("\n".join(v.log_lines) + "\n")
    print(f"report: {rel(report)}")
    return 0 if not v.failures else 1


if __name__ == "__main__":
    if os.name != "nt":
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    sys.exit(main())
