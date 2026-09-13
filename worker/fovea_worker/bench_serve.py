"""bench-serve: start `serve` the way fovea-core does and time single-frame detect jobs over HTTP.

The child runs with --port 0 --token-file --info-file --warmup --exit-on-stdin-eof.
Jobs carry one frame each and are sent one at a time (the core caps detector
requests in flight at 1), round robin over --cameras cameras at --fps per
camera, paced in real time or back to back. Round trips are measured here
around the HTTP request; the worker's own turnaround comes from /v1/health.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import secrets
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from collections import Counter
from pathlib import Path

from .backends import DETECTOR_BACKENDS
from .clock import mono_ns
from .protocol import DETECT_DEFAULT_THRESHOLD
from .stats import percentile

IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png"}
JOB_DEADLINE_MS = 3000
STOP_TIMEOUT_S = 15.0


class WorkerClient:
    def __init__(self, port: int, token: str) -> None:
        self.base = f"http://127.0.0.1:{port}"
        self.token = token
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def call(self, method: str, path: str, body: dict | None = None, timeout_s: float = 30.0) -> tuple[int, dict]:
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.base + path, data=data, method=method, headers={
            "Authorization": f"Bearer {self.token}", "Content-Type": "application/json"})
        try:
            with self.opener.open(request, timeout=timeout_s) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")


def spawn_serve(directory: Path, token: str, extra_args: list[str], stdout=subprocess.DEVNULL) -> subprocess.Popen:
    """Start `serve` with a token file and info file inside directory; stdin is a pipe the caller owns."""
    token_file = directory / "worker.token"
    token_file.write_text(token, encoding="utf-8")
    env = dict(os.environ)
    package_parent = str(Path(__file__).resolve().parent.parent)
    env["PYTHONPATH"] = os.pathsep.join(p for p in (package_parent, env.get("PYTHONPATH", "")) if p)
    cmd = [sys.executable, "-m", "fovea_worker.cli", "serve", "--port", "0", "--token-file", str(token_file),
           "--info-file", str(directory / "worker.json"), *extra_args]
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=stdout, env=env, creationflags=flags)


def request_stop(proc: subprocess.Popen, how: str) -> None:
    """how: "signal" (SIGTERM, CTRL_BREAK on Windows) or "stdin" (close the pipe)."""
    if how == "stdin":
        proc.stdin.close()
    elif sys.platform == "win32":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGTERM)


def wait_for_info(path: Path, proc: subprocess.Popen, timeout_s: float) -> dict | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline and proc.poll() is None:
        if path.exists():
            return json.loads(path.read_text(encoding="utf-8"))
        time.sleep(0.02)
    return None


def _load_average() -> list[float] | None:
    return [round(v, 2) for v in os.getloadavg()] if hasattr(os, "getloadavg") else None


def _summary(values: list[float]) -> dict:
    return {
        "p50": percentile(values, 50),
        "p95": percentile(values, 95),
        "max": max(values, default=None),
        "mean": round(sum(values) / len(values), 1) if values else None,
    }


def _job(camera: int, index: int, path: Path, fps: float, args: argparse.Namespace) -> dict:
    classes = [c.strip() for c in args.classes.split(",") if c.strip()] if args.classes else None
    return {
        "job_id": f"bench-c{camera}-{index}",
        "kind": "detect_frames",
        "camera_id": f"cam{camera}",
        "session_id": "bench",
        "generation": 1,
        "frames": [{"frame_id": f"c{camera}-f{index:05d}", "pts_ns": int(index * 1e9 / fps),
                    "recv_mono_ns": mono_ns(), "capture_utc_ms": 0, "path": str(path), "index": 0}],
        "clip": None,
        "gaps": [],
        "limits": {"max_frames": 1, "deadline_ms": JOB_DEADLINE_MS},
        "threshold": args.threshold,
        "target_classes": classes,
    }


def _wait_ready(client: WorkerClient, proc: subprocess.Popen, timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    health: dict = {}
    while time.monotonic() < deadline and proc.poll() is None:
        _, health = client.call("GET", "/v1/health")
        if health.get("detector_state") in ("ready", "failed"):
            break
        time.sleep(0.1)
    return health


def _run_jobs(client: WorkerClient, frames: list[Path], args: argparse.Namespace) -> dict:
    interval_s = 1.0 / (args.fps * args.cameras)
    sent = [0] * args.cameras
    round_trips: list[float] = []
    per_frame: list[float] = []
    statuses: Counter[str] = Counter()
    violations: list[str] = []
    cameras = [{"tracks_by_class": {}, "frames_with_detections": 0, "notes": []} for _ in range(args.cameras)]
    image_sizes: set[str] = set()
    start = time.monotonic()
    for j in range(args.jobs):
        if args.pace == "realtime":
            delay = start + j * interval_s - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        camera = j % args.cameras
        index = sent[camera]
        sent[camera] += 1
        t0 = mono_ns()
        code, body = client.call("POST", "/v1/jobs", _job(camera, index, frames[index % len(frames)], args.fps, args))
        round_trips.append(round((mono_ns() - t0) / 1e6, 1))
        if code != 200:
            statuses[f"{code}:{body.get('error', {}).get('code', '')}"] += 1
            continue
        statuses[f"{code}:{body['status']}"] += 1
        violations.extend(body["contract_violations"])
        per_frame.extend(body["per_frame_ms"])
        summary = cameras[camera]
        summary["notes"].extend(body["notes"])
        for frame in body["frames"]:
            image_sizes.add(f"{frame['width']}x{frame['height']}")
            if frame["detections"]:
                summary["frames_with_detections"] += 1
            for det in frame["detections"]:
                summary["tracks_by_class"].setdefault(det["cls"], Counter())[str(det["track_id"])] += 1
    wall_s = time.monotonic() - start
    for summary in cameras:
        summary["tracks_by_class"] = {cls: dict(ids.most_common()) for cls, ids in summary["tracks_by_class"].items()}
    return {
        "jobs": args.jobs,
        "wall_s": round(wall_s, 2),
        "achieved_jobs_per_s": round(args.jobs / wall_s, 2) if wall_s > 0 else None,
        "image_sizes": sorted(image_sizes),
        "statuses": dict(statuses),
        "contract_violations": violations,
        "first_round_trip_ms": round_trips[0] if round_trips else None,
        "round_trip_ms": _summary(round_trips),
        "round_trip_ms_after_first": _summary(round_trips[1:]),
        "worker_per_frame_ms": _summary(per_frame),
        "per_camera": cameras,
    }


def run(args: argparse.Namespace) -> int:
    frames = sorted(p for p in Path(args.frames_dir).iterdir() if p.suffix.lower() in IMAGE_SUFFIXES)
    if not frames:
        print("no frames found", file=sys.stderr)
        return 2
    token = secrets.token_hex(16)
    out: dict = {"machine": platform.platform(), "python": platform.python_version(), "detector": args.detector,
                 "cpu_count": os.cpu_count(), "frames": len(frames), "cameras": args.cameras, "fps_per_camera": args.fps, "pace": args.pace,
                 "stop": args.stop}
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        info_path = directory / "worker.json"
        t_spawn = time.monotonic()
        proc = spawn_serve(directory, token, ["--backend", "dry", "--detector", args.detector, "--warmup",
                                              "--exit-on-stdin-eof"])
        try:
            info = wait_for_info(info_path, proc, 30.0)
            if info is None:
                print(f"worker exited or wrote no info file (exit {proc.poll()})", file=sys.stderr)
                return 1
            out["spawn_to_info_ms"] = int((time.monotonic() - t_spawn) * 1000)
            out["info"] = info
            client = WorkerClient(info["port"], token)
            health = _wait_ready(client, proc, args.ready_timeout_s)
            out["spawn_to_ready_ms"] = int((time.monotonic() - t_spawn) * 1000)
            out["health_ready"] = health
            if health.get("detector_state") != "ready":
                print(f"detector not ready: {health}", file=sys.stderr)
                return 1
            out["load_average_before_jobs"] = _load_average()
            out.update(_run_jobs(client, frames, args))
            out["load_average_after_jobs"] = _load_average()
            out["health_after"] = client.call("GET", "/v1/health")[1]
            t_stop = time.monotonic()
            request_stop(proc, args.stop)
            try:
                out["exit_code"] = proc.wait(timeout=STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                out["exit_code"] = None
            out["stop_ms"] = int((time.monotonic() - t_stop) * 1000)
            out["info_file_removed"] = not info_path.exists()
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            if proc.stdin and not proc.stdin.closed:
                proc.stdin.close()
    p95 = out["round_trip_ms"]["p95"]
    out["budget_p95_ms"] = args.budget_p95_ms
    out["within_budget"] = None if args.budget_p95_ms is None or p95 is None else p95 <= args.budget_p95_ms
    print(json.dumps(out, indent=1))
    all_ok = set(out["statuses"]) == {"200:ok"} and not out["contract_violations"]
    return 0 if all_ok and out["exit_code"] == 0 and out["within_budget"] is not False else 1


def add_parser(sub) -> None:
    b = sub.add_parser("bench-serve", help="start serve like fovea-core and time single-frame detect jobs over HTTP")
    b.add_argument("frames_dir")
    b.add_argument("--detector", default="rfdetr", choices=DETECTOR_BACKENDS)
    b.add_argument("--jobs", type=int, default=120)
    b.add_argument("--cameras", type=int, default=1)
    b.add_argument("--fps", type=float, default=2.0, help="jobs per second per camera; also spaces the synthetic pts")
    b.add_argument("--pace", choices=("realtime", "max"), default="realtime")
    b.add_argument("--stop", choices=("signal", "stdin"), default="signal", help="how the worker is stopped at the end")
    b.add_argument("--threshold", type=float, default=DETECT_DEFAULT_THRESHOLD)
    b.add_argument("--classes", help="comma separated target classes")
    b.add_argument("--budget-p95-ms", type=float, help="exit 1 when the round-trip p95 exceeds this")
    b.add_argument("--ready-timeout-s", type=float, default=120.0)
    b.set_defaults(func=run)
