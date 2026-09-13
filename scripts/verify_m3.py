#!/usr/bin/env python3
"""Cross-platform M3 verification (stdlib only): looped RTSP test sources ->
fovea-core supervising the real detector worker -> zones and dwell rules
through the API -> the scenarios of docs/M3_DESIGN.md "Verification":

  1. dwell 10 s on a zone covering seated people: exactly one event 10-15 s
     after the first known detection, alert pending then delivered, no second
     event within 60 s
  2. a zone nobody enters on the same camera: no event
  3. worker killed while a rule is pending: unknown evaluations, no event
     during the outage, the supervisor restarts the worker, dwell restarts;
     the seated event open at the kill stays open through the outage
  4. rule revised while its event is active: same event id
  5. source stopped: session change, the event clears, the next occupancy is
     suppressed until rearm
  6. evidence ref available after the post window and segment finalize;
     playback at the trigger time shows frames
  7. acknowledge and review survive a core restart (killed without cleanup; its
     worker must exit with it), and a clean shutdown stops the worker
Extra check: analytics switched off and on again resumes rule evaluation.

Usage:
  python3 scripts/verify_m3.py --bin-dir build/macos-dev --seated classroom.mp4 [--walking lot.mp4]
Inputs must be H.264 MP4/MOV (rtsp-testsrc --file sends them without
re-encoding). A video wider than 960 px, faster than 25 fps or with B-frames is
transcoded with ffmpeg into <work-dir>-media first (960x540 max, 25 fps,
keyframe every 25 frames). The worker command defaults to the development
venv (worker/.venv) running python -m fovea_worker.cli and is passed to the
core as FOVEA_WORKER_CMD. The JSON report and the log go to
docs/verification/m3-<UTC stamp>.{json,log} unless --report is given.
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
NS = 1_000_000_000
CONSOLE_ID = "verify-m3"


def utc_ms() -> int:
    return int(time.time() * 1000)


def rel(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return path.name


def rect(text: str) -> list[list[float]]:
    x1, y1, x2, y2 = (float(v) for v in text.split(","))
    return [[x1, y1], [x2, y1], [x2, y2], [x1, y2]]


def inside(points: list[list[float]], x: float, y: float) -> bool:
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return min(xs) <= x <= max(xs) and min(ys) <= y <= max(ys)


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
        if args.flat:
            self.core = bin_dir / f"fovea-core{EXE}"
            self.testsrc = bin_dir / f"rtsp-testsrc{EXE}"
        else:
            self.core = self._find(bin_dir, ["src/core", "."], f"fovea-core{EXE}")
            self.testsrc = self._find(bin_dir, ["tools/rtsp-testsrc", "."], f"rtsp-testsrc{EXE}")
        self.work = Path(args.work_dir).resolve()
        self.data = self.work / "data"
        self.db_path = self.data / "fovea.sqlite"
        self.core_proc: subprocess.Popen | None = None
        self.sources: dict[int, subprocess.Popen] = {}
        self.media: list[Path] = []
        self.cams: list[str] = []
        self.api_port = 0
        self.token = ""
        self.log_lines: list[str] = []
        self.failures: list[str] = []
        self.ids: dict[str, str] = {}
        self.results: dict = {"platform": sys.platform, "machine": platform.machine(), "cpu_count": os.cpu_count()}

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

    def api(self, method: str, path: str, body: dict | None = None, timeout: float = 15.0, raw: bool = False):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.api_port}{path}", data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
                if raw:
                    return payload
                return json.loads(payload) if payload else None
        except urllib.error.HTTPError as e:
            raise Fail(f"{method} {path} -> {e.code} {e.read()[:300]!r}") from e
        except (urllib.error.URLError, OSError) as e:
            raise Fail(f"{method} {path} -> {e}") from e

    def status_of(self, method: str, path: str, body: dict | None = None) -> int:
        try:
            self.api(method, path, body)
            return 200
        except Fail as e:
            text = str(e)
            marker = "-> "
            code = text[text.find(marker) + len(marker):].split(" ", 1)[0] if marker in text else ""
            return int(code) if code.isdigit() else 0

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

    def db(self) -> sqlite3.Connection:
        return sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=10)

    def query(self, sql: str, params: tuple = ()) -> list[tuple]:
        with self.db() as conn:
            return conn.execute(sql, params).fetchall()

    # media and processes

    def prepare_media(self, video: str) -> Path:
        src = Path(video).resolve()
        if not src.is_file():
            raise Fail(f"missing input video {src.name}")
        info = {}
        if shutil.which("ffprobe"):
            out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                  "stream=codec_name,width,height,r_frame_rate,has_b_frames", "-of", "json", str(src)],
                                 capture_output=True, text=True, timeout=30).stdout
            streams = json.loads(out or "{}").get("streams") or [{}]
            info = streams[0]
        num, _, den = str(info.get("r_frame_rate", "0/1")).partition("/")
        fps = float(num) / float(den or 1) if float(den or 1) else 0.0
        suitable = (info.get("codec_name") == "h264" and int(info.get("width", 0)) <= 960 and fps <= 25.5
                    and int(info.get("has_b_frames", 1)) == 0)
        entry = {"name": src.name, "codec": info.get("codec_name"), "width": info.get("width"),
                 "height": info.get("height"), "fps": round(fps, 2), "transcoded": not suitable}
        self.results.setdefault("media", []).append(entry)
        if suitable:
            return src
        if not shutil.which("ffmpeg"):
            raise Fail(f"{src.name} needs a 960x540 25 fps H.264 copy and ffmpeg is not installed")
        media_dir = self.work.parent / f"{self.work.name}-media"
        media_dir.mkdir(parents=True, exist_ok=True)
        target = media_dir / f"{src.stem}-540p25.mp4"
        if not target.exists():
            self.log(f"transcoding {src.name} to {target.name}")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(src), "-an", "-vf",
                            "scale='min(960,iw)':-2,fps=25", "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p",
                            "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-bf", "0", str(target)],
                           check=True, timeout=600)
        entry["used"] = target.name
        return target

    def start_source(self, index: int) -> None:
        port = self.args.rtsp_port_base + index
        out = open(self.work / f"testsrc-{index}.log", "ab")
        self.sources[index] = subprocess.Popen(
            [str(self.testsrc), "--port", str(port), "--path", "/test", "--file", str(self.media[index]), "--loop"],
            stdout=out, stderr=subprocess.STDOUT)
        time.sleep(1.5)
        if self.sources[index].poll() is not None:
            raise Fail(f"rtsp-testsrc {index} exited with {self.sources[index].returncode}; see testsrc-{index}.log")

    def stop_source(self, index: int) -> None:
        proc = self.sources.pop(index, None)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()

    def worker_cmd(self) -> str:
        if self.args.worker_cmd:
            return self.args.worker_cmd
        python = REPO / "worker" / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        return f'"{python}" -m fovea_worker.cli'

    def start_core(self, tag: str) -> None:
        info = self.data / "core.json"
        if info.exists():
            info.unlink()
        env = dict(os.environ, FOVEA_WORKER_CMD=self.worker_cmd(), FOVEA_MIN_FREE_MB=str(self.args.min_free_mb))
        env.pop("FOVEA_RETENTION_SECONDS", None)
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
            self.core_proc.wait(30)
        except subprocess.TimeoutExpired:
            raise Fail("core did not exit within 30 s of shutdown")

    def worker_pids(self) -> list[int]:
        info = self.data / "worker.json"
        try:
            return [int(json.loads(info.read_text())["pid"])]
        except (OSError, ValueError, KeyError):
            return []

    # observations

    def analysis(self) -> dict:
        return self.api("GET", "/v1/analysis")

    def camera_analysis(self, cam: str) -> dict:
        return next(c for c in self.analysis()["cameras"] if c["camera_id"] == cam)

    def events(self, rule_id: str) -> list[dict]:
        return [e for e in self.api("GET", f"/v1/events?limit=1000&camera_id={self.cams[0]}") if e["rule_id"] == rule_id]

    def evaluations(self, rule_id: str, transition: str, after_ms: int = 0) -> list[tuple]:
        return self.query("SELECT utc_ms, note, event_id, dwell_ns, quality FROM rule_evaluations WHERE rule_id=? AND transition=?"
                          " AND utc_ms>=? ORDER BY id", (rule_id, transition, after_ms))

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

    def rule_body(self, name: str, zone_id: str, dwell_s: int) -> dict:
        return {"name": name, "camera_id": self.cams[0], "zone_id": zone_id, "dwell_ns": dwell_s * NS,
                "clear_after_ns": 5 * NS, "rearm_ns": self.args.rearm_s * NS, "max_observation_gap_ns": int(1.5 * NS),
                "result_ttl_ns": 5 * NS, "min_confidence": 0.3, "target_class": "person", "time_zone": "Asia/Seoul",
                "schedule": [{"days": 127, "start_minute": 0, "end_minute": 1440}], "severity": "critical",
                "actions": {"sound": True, "pop_to_main_view": True}}

    # phases

    def setup(self) -> None:
        a = self.args
        for exe in (self.core, self.testsrc):
            if not exe.exists():
                raise Fail(f"missing {exe.name} under --bin-dir")
        if self.work.exists():
            shutil.rmtree(self.work)
        self.data.mkdir(parents=True)
        videos = [a.seated] + ([a.walking] if a.walking else [])
        self.media = [self.prepare_media(v) for v in videos]
        if hasattr(os, "getloadavg"):
            self.results["loadavg_start"] = [round(x, 2) for x in os.getloadavg()]
        for i in range(len(self.media)):
            self.start_source(i)
        started = time.monotonic()
        self.start_core("1")
        for i, name in enumerate(["Classroom", "Parking lot"][:len(self.media)]):
            cam = self.api("POST", "/v1/cameras", {
                "code": f"M3-{i + 1:02d}", "name": name, "group_name": "M3", "kind": "rtsp",
                "main_url": f"rtsp://127.0.0.1:{a.rtsp_port_base + i}/test", "transport": "tcp",
                "segment_seconds": a.segment_seconds, "record_enabled": True, "analytics_enabled": True})
            self.cams.append(cam["id"])
        self.wait("the detector to return known results for every camera", 180, lambda: (lambda cams: len(cams) == len(self.cams) and all(
            c["frames_known"] >= 4 for c in cams))(self.analysis()["cameras"]))
        worker = self.analysis()["worker"]
        self.results["worker_start"] = {
            "core_start_to_known_results_s": round(time.monotonic() - started, 1), "source": worker.get("source"),
            "detector": worker.get("detector_model"), "device": worker.get("detector_device"),
            "detector_load_ms": worker.get("detector_load_ms")}
        self.log(f"worker ready: {self.results['worker_start']}")
        self.check(worker["state"] == "ready" and worker.get("detector_state") == "ready", f"worker state {worker['state']}")

        det = self.api("GET", f"/v1/cameras/{self.cams[0]}/detections/latest")
        self.frame_size = (det["width"], det["height"])
        positive, negative = rect(a.positive_zone), rect(a.negative_zone)
        pos_hits = neg_hits = 0
        end = time.monotonic() + a.sample_seconds
        while time.monotonic() < end:
            det = self.api("GET", f"/v1/cameras/{self.cams[0]}/detections/latest")
            for d in det["detections"]:
                if d["cls"] != "person" or d["track_id"] is None:
                    continue
                x, y = d["anchor_foot"]
                pos_hits += inside(positive, x, y)
                neg_hits += inside(negative, x, y)
            time.sleep(0.5)
        self.results["zone_sampling"] = {"frame_size": list(self.frame_size), "positive_anchor_hits": pos_hits,
                                         "negative_anchor_hits": neg_hits, "seconds": a.sample_seconds}
        self.log(f"zone sampling: {self.results['zone_sampling']}")
        if pos_hits == 0 or neg_hits > 0:
            raise Fail(f"zones do not fit the video: {pos_hits} tracked anchors in the positive zone, {neg_hits} in the negative zone")
        self.steady_start = (time.monotonic(), self.camera_analysis(self.cams[0]))

    def scenario_dwell(self) -> None:
        a = self.args
        w, h = self.frame_size
        zp = self.api("POST", "/v1/zones", {"camera_id": self.cams[0], "name": "Seated desks", "points": rect(a.positive_zone),
                                            "anchor": "foot", "ref_width": w, "ref_height": h})
        zn = self.api("POST", "/v1/zones", {"camera_id": self.cams[0], "name": "Window (nobody)", "points": rect(a.negative_zone),
                                            "anchor": "foot", "ref_width": w, "ref_height": h})
        self.ids.update(zone_positive=zp["id"], zone_negative=zn["id"])
        rp = self.api("POST", "/v1/rules", self.rule_body("Seated dwell", zp["id"], 10))
        rn = self.api("POST", "/v1/rules", self.rule_body("Window dwell", zn["id"], 10))
        self.ids.update(rule_positive=rp["id"], rule_negative=rn["id"])
        self.negative_since_ms = utc_ms()
        self.log(f"rules created: positive {rp['id'][:8]} rev {rp['revision']}, negative {rn['id'][:8]}")

        events = self.wait("the dwell event", 45, lambda: self.events(rp["id"]))
        event = events[0]
        self.ids["event1"] = event["id"]
        pending = self.evaluations(rp["id"], "became_pending")
        triggered = self.evaluations(rp["id"], "triggered")
        first_known_ms = pending[0][0] if pending else 0
        created_ms = min(d["created_utc_ms"] for d in event["deliveries"])
        overshoot_ms = (triggered[0][3] - 10 * NS) // 1_000_000 if triggered else 0
        opened_audit = self.query("SELECT detail FROM audit_log WHERE action='event.open' AND target=?", (event["id"],))
        frame_age_ms = json.loads(opened_audit[0][0]).get("frame_age_ms") if opened_audit else None
        s1 = {"event_id": event["id"], "opened_after_first_known_detection_ms": event["opened_utc_ms"] - first_known_ms,
              "pending_rows_before_trigger": len(pending), "dwell_at_trigger_ms": triggered[0][3] // 1_000_000 if triggered else None,
              "trigger_frame_receive_to_event_row_ms": frame_age_ms,
              "dwell_overshoot_ms": overshoot_ms,
              "condition_satisfied_to_event_row_ms": frame_age_ms + overshoot_ms if frame_age_ms is not None else None,
              "trigger_frame_utc_to_delivery_row_utc_ms": created_ms - event["opened_utc_ms"],
              "title": event["title"], "detail": event["detail"]}
        self.results["scenario1_dwell"] = s1
        self.log(f"scenario 1: {s1}")
        self.check(len(self.events(rp["id"])) == 1, "exactly one event for the seated zone")
        self.check(10_000 <= s1["opened_after_first_known_detection_ms"] <= 15_000,
                   f"event opened {s1['opened_after_first_known_detection_ms']} ms after the first known detection (10-15 s)")

        pending_alerts = [d for d in self.api("GET", f"/v1/alerts/pending?console_id={CONSOLE_ID}") if d["event_id"] == event["id"]]
        channels = sorted(d["channel"] for d in pending_alerts)
        self.check(channels == ["console", "sound"] and all(d["state"] == "pending" for d in pending_alerts),
                   f"alert pending on console and sound: {[(d['channel'], d['state']) for d in pending_alerts]}")
        for d in pending_alerts:
            confirmed = self.api("POST", f"/v1/alerts/{d['id']}/delivered", {"console_id": CONSOLE_ID})
            self.check(confirmed["state"] == "delivered", f"{d['channel']} delivery confirmed")
        still = [d for d in self.api("GET", f"/v1/alerts/pending?console_id={CONSOLE_ID}") if d["event_id"] == event["id"]]
        self.check(not still, f"delivered alerts left the pending list ({len(still)} left)")
        self.trigger_monotonic = time.monotonic()
        self.event1 = event

    def scenario_revision(self) -> None:
        rid = self.ids["rule_positive"]
        revised = self.api("PUT", f"/v1/rules/{rid}", {"min_confidence": 0.35, "severity": "review"})
        time.sleep(5)
        events = self.events(rid)
        runtime = next(r for r in self.api("GET", "/v1/rules") if r["id"] == rid)["runtime"][0]
        s4 = {"revision": revised["revision"], "events": len(events), "open_event_id": runtime["open_event_id"],
              "condition": runtime["condition"]}
        self.results["scenario4_revision"] = s4
        self.log(f"scenario 4: {s4}")
        self.check(revised["revision"] == 2, "rule revision 2 stored")
        self.check(len(events) == 1 and events[0]["id"] == self.ids["event1"] and events[0]["condition"] in ("active", "clearing"),
                   "the open event kept its id across the revision")
        self.check(runtime["open_event_id"] == self.ids["event1"], "evaluator still holds the same open event")

    def scenario_review(self) -> None:
        eid = self.ids["event1"]
        acked = self.api("POST", f"/v1/events/{eid}/acknowledge", {"operator": "verify_m3"})
        reviewed = self.api("POST", f"/v1/events/{eid}/review", {"label": "confirmed", "note": "seated people", "operator": "verify_m3"})
        self.check(acked["operator_state"] == "acknowledged" and reviewed["review"]["label"] == "confirmed",
                   "event acknowledged and reviewed")

    def scenario_evidence(self) -> None:
        eid = self.ids["event1"]
        t0 = time.monotonic()
        event = self.wait("evidence available", 90, lambda: (lambda e: e if e["evidence"]["state"] == "available" else None)(
            self.api("GET", f"/v1/events/{eid}")), interval=1.0)
        evidence = event["evidence"]
        thumb = self.api("GET", f"/v1/evidence/{evidence['id']}/thumbnail", raw=True)
        pb = self.api("POST", "/v1/playback", {"camera_id": self.cams[0], "at_utc_ms": event["opened_utc_ms"]}, timeout=30)
        self.api("POST", f"/v1/playback/{pb['id']}/play")
        time.sleep(0.5)
        frames = self.probe_ring(pb["frame_ring"]["name"], 3.0)
        state = self.api("GET", f"/v1/playback/{pb['id']}")
        self.api("DELETE", f"/v1/playback/{pb['id']}")
        s6 = {"evidence_id": evidence["id"], "window_ms": evidence["to_utc_ms"] - evidence["from_utc_ms"],
              "segments": len(evidence["segment_ids"]), "available_s_after_trigger": round(time.monotonic() - self.trigger_monotonic, 1),
              "waited_s": round(time.monotonic() - t0, 1), "thumbnail_bytes": len(thumb), "thumbnail_jpeg": thumb[:2] == b"\xff\xd8",
              "playback_segment_start_utc_ms": state.get("start_utc_ms"), "playback_frames_in_3s": frames,
              "holds": self.query("SELECT COUNT(*), MAX(until_utc_ms) FROM evidence_holds WHERE reason=?", (f"event:{eid}",))[0]}
        self.results["scenario6_evidence"] = s6
        self.log(f"scenario 6: {s6}")
        self.check(s6["thumbnail_jpeg"], "trigger thumbnail is a JPEG")
        self.check(frames >= 10, f"playback at the trigger time delivered {frames} frames in 3 s")
        self.check(s6["holds"][0] >= 1 and s6["holds"][1] == 0, f"open-ended evidence holds on {s6['holds'][0]} segments")

        remaining = 60 - (time.monotonic() - self.trigger_monotonic)
        if remaining > 0:
            time.sleep(remaining)
        count = len(self.events(self.ids["rule_positive"]))
        self.results["scenario1_dwell"]["events_60s_after_trigger"] = count
        self.check(count == 1, f"no second seated event within 60 s of the trigger ({count} events)")
        self.steady_end = (time.monotonic(), self.camera_analysis(self.cams[0]))

    def scenario_source_stop(self) -> None:
        a = self.args
        rid = self.ids["rule_positive"]
        cam = self.cams[0]
        before = self.api("GET", f"/v1/cameras/{cam}/status")
        generation_before = self.camera_analysis(cam)["generation"]
        stopped_ms = utc_ms()
        self.stop_source(0)
        self.log("scenario 5: source stopped")
        time.sleep(a.source_down_s)
        self.start_source(0)
        status = self.wait("a new session", 90, lambda: (lambda s: s if s["state"] == "online" and s["session_id"] != before["session_id"]
                                                          else None)(self.api("GET", f"/v1/cameras/{cam}/status")))
        cleared = self.wait("the event to clear", 60, lambda: (lambda e: e if e["condition"] == "cleared" else None)(
            self.api("GET", f"/v1/events/{self.ids['event1']}")))
        second = self.wait("the next occupancy event after rearm", a.rearm_s + 60,
                           lambda: [e for e in self.events(rid) if e["id"] != self.ids["event1"]], interval=1.0)[0]
        self.ids["event2"] = second["id"]
        suppressed = self.evaluations(rid, "suppressed", cleared["cleared_utc_ms"])
        clear_rows = self.evaluations(rid, "cleared", stopped_ms)
        gaps = self.api("GET", f"/v1/cameras/{cam}/gaps?from_utc_ms={stopped_ms}")
        s5 = {"old_session": before["session_id"], "new_session": status["session_id"],
              "generation_before": generation_before, "generation_after": self.camera_analysis(cam)["generation"],
              "cleared_note": clear_rows[0][1] if clear_rows else None, "cleared_after_stop_ms": cleared["cleared_utc_ms"] - stopped_ms,
              "suppressed_rows": len(suppressed), "first_suppressed_after_clear_ms": suppressed[0][0] - cleared["cleared_utc_ms"] if suppressed else None,
              "second_event_after_clear_ms": second["opened_utc_ms"] - cleared["cleared_utc_ms"], "receive_gaps": len(gaps)}
        self.results["scenario5_source_stop"] = s5
        self.log(f"scenario 5: {s5}")
        self.check(s5["new_session"] != s5["old_session"] and s5["generation_after"] > s5["generation_before"],
                   "session changed and the camera generation increased")
        self.check(cleared["condition"] == "cleared" and s5["cleared_note"] is not None, f"event cleared ({s5['cleared_note']})")
        self.check(s5["suppressed_rows"] >= 1, "next occupancy reported suppressed while rearm ran")
        self.check(s5["second_event_after_clear_ms"] >= a.rearm_s * 1000 - 1000,
                   f"next event opened {s5['second_event_after_clear_ms']} ms after the clear (rearm {a.rearm_s} s)")

    def scenario_worker_kill(self) -> None:
        zone = self.ids["zone_positive"]
        body = self.rule_body("Seated dwell 20 s", zone, 20)
        rule = self.api("POST", "/v1/rules", body)
        rid = rule["id"]
        self.ids["rule_worker_kill"] = rid
        created_ms = utc_ms()
        self.wait("the 20 s rule to become pending", 30, lambda: self.evaluations(rid, "became_pending"))
        time.sleep(self.args.pending_before_kill_s)
        before = self.analysis()["worker"]
        seated_before = self.api("GET", f"/v1/events/{self.ids['event2']}")
        seated_count_before = len(self.events(self.ids["rule_positive"]))
        pids = self.worker_pids()
        if not pids:
            raise Fail("worker.json has no pid")
        kill_ms = utc_ms()
        kill_mono = time.monotonic()
        os.kill(pids[0], signal.SIGTERM if os.name == "nt" else signal.SIGKILL)
        self.log(f"scenario 3: killed worker pid {pids[0]} while the 20 s rule was pending")
        restarted = self.wait("the supervisor to restart the worker", 120, lambda: (lambda w: w if w["state"] == "ready"
                              and w.get("detector_state") == "ready" and w["restarts"] > before["restarts"] else None)(self.analysis()["worker"]))
        ready_s = round(time.monotonic() - kill_mono, 1)
        resumed = self.wait("the dwell to restart", 60, lambda: self.evaluations(rid, "became_pending", kill_ms))
        resume_ms = resumed[0][0]
        event = self.wait("the 20 s rule event", 60, lambda: self.events(rid), interval=1.0)[0]
        unknown = self.query("SELECT utc_ms, note FROM rule_evaluations WHERE rule_id=? AND transition='unknown' AND utc_ms>=? AND utc_ms<=?",
                             (rid, kill_ms - 2000, resume_ms))
        early = [e for e in self.events(rid) if e["opened_utc_ms"] < resume_ms]
        seated_after = self.api("GET", f"/v1/events/{self.ids['event2']}")
        seated_count_after = len(self.events(self.ids["rule_positive"]))
        new_pids = self.worker_pids()
        inherited = None
        if new_pids and os.name != "nt" and shutil.which("lsof"):
            listing = subprocess.run(["lsof", "-p", str(new_pids[0]), "-Fn"], capture_output=True, text=True).stdout
            inherited = [line[1:] for line in listing.splitlines() if line.startswith("n") and line.endswith(".mkv")]
        s3 = {"killed_pid": pids[0], "pending_before_kill_s": self.args.pending_before_kill_s, "restarts": restarted["restarts"],
              "worker_ready_s_after_kill": ready_s, "new_pid": self.worker_pids()[0] if self.worker_pids() else None,
              "unknown_rows_during_outage": len(unknown), "unknown_notes": sorted({n for _, n in unknown}),
              "dwell_restart_after_kill_ms": resume_ms - kill_ms, "events_before_resume": len(early),
              "event_after_resume_ms": event["opened_utc_ms"] - resume_ms, "rule_created_to_event_ms": event["opened_utc_ms"] - created_ms,
              "seated_event_condition": [seated_before["condition"], seated_after["condition"]],
              "seated_events": [seated_count_before, seated_count_after],
              "restarted_worker_open_recordings": None if inherited is None else len(inherited)}
        self.results["scenario3_worker_kill"] = s3
        self.log(f"scenario 3: {s3}")
        self.check(s3["unknown_rows_during_outage"] >= 1, "unknown evaluations recorded during the worker outage")
        self.check(s3["events_before_resume"] == 0, "no event opened during the outage")
        self.check(restarted["restarts"] == before["restarts"] + 1 and s3["new_pid"] != pids[0], "supervisor restarted the worker")
        self.check(s3["event_after_resume_ms"] >= 19_000, f"dwell restarted: event {s3['event_after_resume_ms']} ms after the resumed pending")
        if inherited is not None:
            self.check(not inherited, f"the restarted worker holds no recording file open ({len(inherited)})")
        if seated_before["condition"] == "active":
            self.check(seated_after["condition"] != "cleared" and seated_count_after == seated_count_before,
                       f"the seated event open at the kill stayed open through the outage ({s3['seated_event_condition']}, "
                       f"events {s3['seated_events']})")
        else:
            self.log(f"scenario 3: the seated event was {seated_before['condition']} at the kill; outage check skipped")

    def scenario_analytics_toggle(self) -> None:
        cam = self.cams[0]
        rid = self.ids["rule_positive"]
        restarted_ms = self.restarted_ms
        self.wait("the seated rule to evaluate frames after the restart", 120, lambda: self.evaluations(rid, "became_pending", restarted_ms))
        self.api("PUT", f"/v1/cameras/{cam}", {"analytics_enabled": False})
        time.sleep(1)
        analyzing = any(c["camera_id"] == cam for c in self.analysis()["cameras"])
        detections_status = self.status_of("GET", f"/v1/cameras/{cam}/detections/latest")
        time.sleep(self.args.analytics_off_s)
        enabled_ms = utc_ms()
        self.api("PUT", f"/v1/cameras/{cam}", {"analytics_enabled": True})
        lifecycle = self.wait("the seated rule to evaluate frames again", 45, lambda: self.query(
            "SELECT transition FROM rule_evaluations WHERE rule_id=? AND utc_ms>=? AND quality='known'"
            " AND transition IN ('became_pending','triggered','became_clearing','cleared')", (rid, enabled_ms)))
        stale = self.query("SELECT COUNT(*) FROM rule_evaluations WHERE rule_id=? AND utc_ms>=? AND transition='stale'", (rid, enabled_ms))[0][0]
        s8 = {"analyzing_while_off": analyzing, "detections_status_while_off": detections_status,
              "off_s": self.args.analytics_off_s, "transitions_after_enable": [t for (t,) in lifecycle], "stale_rows_after_enable": stale}
        self.results["scenario8_analytics_toggle"] = s8
        self.log(f"analytics off and on: {s8}")
        self.check(not analyzing and detections_status == 404, "analytics off: camera not analysed, detections 404")
        self.check(bool(lifecycle), "rules evaluate known frames again after analytics is re-enabled")

    def measure(self) -> None:
        cam = self.camera_analysis(self.cams[0])
        worker = self.analysis()["worker"]
        t0, a0 = self.steady_start
        t1, a1 = self.steady_end
        sent = a1["frames_sent"] - a0["frames_sent"]
        known = a1["frames_known"] - a0["frames_known"]
        unknown = a1["frames_unknown"] - a0["frames_unknown"]
        negative_hours = (utc_ms() - self.negative_since_ms) / 3_600_000
        negative_events = len(self.events(self.ids["rule_negative"]))
        coverage = self.query("SELECT COUNT(*), SUM(frames_sent), SUM(frames_known), SUM(frames_unknown) FROM analysis_coverage WHERE camera_id=?",
                              (self.cams[0],))[0]
        m = {"steady_window_s": round(t1 - t0, 1), "steady_detect_fps": round(known / (t1 - t0), 2),
             "steady_unknown_ratio": round(unknown / max(1, sent), 4),
             "run_unknown_ratio": round(cam["unknown_ratio"], 4), "run_frames": {k: cam[k] for k in ("frames_sent", "frames_known", "frames_unknown", "skips")},
             "achieved_fps_last_10s": cam["achieved_fps"], "core_turnaround_ms": cam["turnaround_ms"], "core_request_ms": cam["request_ms"],
             "worker_detector_turnaround_ms": worker.get("detector_turnaround_ms"),
             "negative_zone": {"events": negative_events, "camera_hours": round(negative_hours, 4),
                               "events_per_camera_hour": round(negative_events / negative_hours, 2) if negative_hours > 0 else None},
             "coverage_rows": {"rows": coverage[0], "sent": coverage[1], "known": coverage[2], "unknown": coverage[3]}}
        if len(self.cams) > 1:
            other = self.camera_analysis(self.cams[1])
            m["second_camera"] = {k: other[k] for k in ("achieved_fps", "frames_sent", "frames_known", "frames_unknown", "unknown_ratio",
                                                        "turnaround_ms", "skips")}
        self.results["measurements"] = m
        self.log(f"measurements: {json.dumps(m)}")
        self.check(negative_events == 0, f"no event on the zone nobody enters ({negative_events})")

    def scenario_restart(self) -> None:
        eid = self.ids["event1"]
        open_before = [e["id"] for e in self.api("GET", "/v1/events?limit=1000") if e["condition"] != "cleared"]
        orphan = self.worker_pids()
        self.core_proc.kill()
        self.core_proc.wait(10)
        self.log(f"scenario 7: killed the core without cleanup; worker pid {orphan}")
        deadline = time.monotonic() + 15
        while orphan and pid_alive(orphan[0]) and time.monotonic() < deadline:
            time.sleep(0.2)
        orphan_alive = bool(orphan) and pid_alive(orphan[0])
        self.start_core("2")
        event = self.wait("the API after restart", 30, lambda: self.api("GET", f"/v1/events/{eid}"))
        rules = {r["id"]: r for r in self.api("GET", "/v1/rules")}
        reopened = {e["id"]: e for e in self.api("GET", "/v1/events?limit=1000")}
        audit = self.query("SELECT action, actor FROM audit_log WHERE target=? ORDER BY id", (eid,))
        self.restarted_ms = utc_ms()
        s7 = {"operator_state": event["operator_state"], "review": event["review"],
              "deliveries": [(d["channel"], d["state"]) for d in event["deliveries"]],
              "rule_revisions": {k[:8]: r["revision"] for k, r in rules.items()},
              "open_before_restart": len(open_before), "cleared_on_startup": sum(1 for i in open_before if reopened.get(i, {}).get("condition") == "cleared"),
              "worker_alive_after_core_kill": orphan_alive, "audit": audit}
        self.results["scenario7_restart"] = s7
        self.log(f"scenario 7: {s7}")
        self.check(event["operator_state"] == "acknowledged" and (event["review"] or {}).get("label") == "confirmed"
                   and (event["review"] or {}).get("note") == "seated people", "acknowledge and review survived the restart")
        self.check(all(state == "delivered" for _, state in s7["deliveries"]), "deliveries still delivered after restart")
        self.check(rules.get(self.ids["rule_positive"], {}).get("revision") == 2, "rule revision persisted")
        self.check(s7["open_before_restart"] >= 1 and s7["cleared_on_startup"] == s7["open_before_restart"],
                   f"events left open by the killed core were cleared at startup ({s7['cleared_on_startup']} of {s7['open_before_restart']})")
        self.check(("event.acknowledge", "verify_m3") in audit and ("event.review", "verify_m3") in audit, "operator actions in the audit log")
        self.check(bool(orphan) and not orphan_alive, "the worker exited after the core was killed (stdin closed)")

    def scenario_shutdown(self) -> None:
        self.wait("the worker", 120, lambda: self.analysis()["worker"].get("detector_state") == "ready")
        running = self.worker_pids()
        self.stop_core()
        time.sleep(1)
        state = {"info_file": bool(self.worker_pids()), "alive": bool(running) and pid_alive(running[0])}
        self.results["scenario7_restart"]["worker_after_clean_shutdown"] = state
        self.log(f"clean shutdown: {state}")
        self.check(bool(running) and not state["alive"] and not state["info_file"],
                   "a clean core shutdown stopped the worker and its info file is gone")

    def run(self) -> None:
        self.setup()
        self.scenario_dwell()
        self.scenario_revision()
        self.scenario_review()
        self.scenario_evidence()
        self.scenario_source_stop()
        self.scenario_worker_kill()
        self.measure()
        self.scenario_restart()
        self.scenario_analytics_toggle()
        self.scenario_shutdown()
        if hasattr(os, "getloadavg"):
            self.results["loadavg_end"] = [round(x, 2) for x in os.getloadavg()]

    def cleanup(self) -> None:
        if self.core_proc and self.core_proc.poll() is None:
            self.core_proc.terminate()
            try:
                self.core_proc.wait(20)
            except subprocess.TimeoutExpired:
                self.core_proc.kill()
        for index in list(self.sources):
            self.stop_source(index)
        for pid in self.worker_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
        if not self.args.keep and self.work.exists():
            shutil.rmtree(self.data / "recordings", ignore_errors=True)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--bin-dir", required=True)
    p.add_argument("--flat", action="store_true", help="binaries are all directly in --bin-dir")
    p.add_argument("--seated", required=True, help="video with people staying inside --positive-zone (classroom)")
    p.add_argument("--walking", help="optional second analytics camera (no rules), for detector load")
    p.add_argument("--worker-cmd", help="FOVEA_WORKER_CMD for the core (default: worker/.venv python -m fovea_worker.cli)")
    p.add_argument("--work-dir", default="build/verify-m3-py")
    p.add_argument("--positive-zone", default="0.05,0.55,0.38,0.95", help="x1,y1,x2,y2 normalized; covers the seated people")
    p.add_argument("--negative-zone", default="0.75,0.05,0.95,0.35", help="x1,y1,x2,y2 normalized; nobody's feet go there")
    p.add_argument("--rearm-s", type=int, default=20)
    p.add_argument("--segment-seconds", type=int, default=10)
    p.add_argument("--sample-seconds", type=float, default=6)
    p.add_argument("--source-down-s", type=float, default=8)
    p.add_argument("--pending-before-kill-s", type=float, default=5)
    p.add_argument("--analytics-off-s", type=float, default=8)
    p.add_argument("--min-free-mb", type=int, default=100)
    p.add_argument("--rtsp-port-base", type=int, default=18754)
    p.add_argument("--keep", action="store_true", help="keep recordings under --work-dir")
    p.add_argument("--report", help="JSON report path (the log is written next to it)")
    args = p.parse_args()
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    report = Path(args.report) if args.report else REPO / "docs" / "verification" / f"m3-{stamp}.json"
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
    v.results["ids"] = {k: val[:8] for k, val in v.ids.items()}
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
