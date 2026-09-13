from __future__ import annotations

import argparse
import contextlib
import json
import os
import secrets
import signal
import sys
import threading
import time
from collections import Counter
from pathlib import Path

from . import bench_serve
from .backends import DETECTOR_BACKENDS, VLM_BACKENDS, load_backend, load_detector
from .protocol import DETECT_DEFAULT_THRESHOLD, Job, Result, validate_result_against_job
from .server import DETECT_LANE, serve
from .stats import percentile

STOP_SIGNALS = ("SIGTERM", "SIGINT", "SIGBREAK")
STOP_POLL_S = 0.2
STOP_GRACE_S = 5.0


def _frames_from_dir(directory: Path, fps: float) -> list[dict]:
    files = sorted(p for p in directory.iterdir() if p.suffix.lower() in {".jpg", ".jpeg", ".png"})
    frames = []
    for i, p in enumerate(files):
        pts_ns = int(i / fps * 1e9)
        frames.append({"frame_id": f"f{i:04d}", "pts_ns": pts_ns, "recv_mono_ns": 0, "capture_utc_ms": 0,
                       "path": str(p), "index": i})
    return frames


def _detect_job(frames: list[dict], args: argparse.Namespace) -> Job:
    classes = [c.strip() for c in args.classes.split(",") if c.strip()] if args.classes else None
    return Job.from_dict({
        "job_id": "cli-" + secrets.token_hex(4),
        "kind": "detect_frames",
        "camera_id": args.camera,
        "session_id": args.session,
        "generation": 1,
        "frames": frames,
        "clip": None,
        "gaps": [],
        "limits": {"max_frames": len(frames), "deadline_ms": 60000},
        "threshold": args.threshold,
        "target_classes": classes,
    })


def _detect_summary(result: Result) -> dict:
    classes: Counter[str] = Counter()
    tracks: dict[str, set[str]] = {}
    frames_with_detections = 0
    for frame in result.frames:
        if frame.detections:
            frames_with_detections += 1
        for det in frame.detections:
            classes[det.cls] += 1
            if det.track_id is not None:
                tracks.setdefault(det.cls, set()).add(det.track_id)
    return {
        "frames": len(result.frames),
        "frames_with_detections": frames_with_detections,
        "detections_by_class": dict(classes),
        "track_ids_by_class": {k: sorted(v, key=lambda s: (len(s), s)) for k, v in tracks.items()},
    }


def _stdout_to_stderr():
    """Model libraries log INFO lines to stdout; keep stdout for the JSON result."""
    return contextlib.redirect_stdout(sys.stderr)


def cmd_analyze(args: argparse.Namespace) -> int:
    frames = _frames_from_dir(Path(args.frames_dir), args.fps)
    if not frames:
        print("no frames found", file=sys.stderr)
        return 2
    job = Job.from_dict({
        "job_id": "cli-" + secrets.token_hex(4),
        "kind": "vlm_clip",
        "camera_id": "cli",
        "generation": 1,
        "frames": frames,
        "clip": {"session_id": "cli", "start_pts_ns": frames[0]["pts_ns"], "end_pts_ns": frames[-1]["pts_ns"],
                 "start_utc_ms": 0, "end_utc_ms": 0},
        "gaps": [],
        "limits": {"max_frames": args.max_frames, "max_new_tokens": args.max_new_tokens, "deadline_ms": 60000},
        "prompt": args.prompt,
        "language": args.language,
        "clip_path": args.clip or "",
    })
    backend = load_backend(args.backend)
    t0 = time.monotonic()
    backend.load()
    load_ms = int((time.monotonic() - t0) * 1000)
    result = backend.run(job)
    out = result.to_dict()
    out["load_ms"] = load_ms
    out["contract_violations"] = validate_result_against_job(result, job)
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if result.status in ("ok", "dry_run") else 1


def cmd_detect(args: argparse.Namespace) -> int:
    frames = _frames_from_dir(Path(args.frames_dir), args.fps)
    if not frames:
        print("no frames found", file=sys.stderr)
        return 2
    job = _detect_job(frames, args)
    detector = load_detector(args.detector)
    with _stdout_to_stderr():
        t0 = time.monotonic()
        detector.load()
        load_ms = int((time.monotonic() - t0) * 1000)
        result = detector.run(job)
    out = result.to_dict()
    out["load_ms"] = load_ms
    out["contract_violations"] = validate_result_against_job(result, job)
    out["summary"] = _detect_summary(result)
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if result.status in ("ok", "dry_run") else 1


def cmd_bench_detect(args: argparse.Namespace) -> int:
    frames = _frames_from_dir(Path(args.frames_dir), args.fps)
    if not frames:
        print("no frames found", file=sys.stderr)
        return 2
    detector = load_detector(args.detector)
    latencies: list[int] = []
    violations: list[str] = []
    summaries = []
    notes: list[str] = []
    with _stdout_to_stderr():
        t0 = time.monotonic()
        detector.load()
        load_ms = int((time.monotonic() - t0) * 1000)
        warm = detector.run(_detect_job(frames[:1], args))
        first_frame_ms = warm.per_frame_ms[0] if warm.per_frame_ms else warm.processing_ms
        for _ in range(args.repeat):
            job = _detect_job(frames, args)
            result = detector.run(job)
            latencies.extend(result.per_frame_ms)
            violations.extend(validate_result_against_job(result, job))
            summaries.append(_detect_summary(result))
            notes = result.notes
    total_ms = sum(latencies)
    out = {
        "model": detector.name,
        "model_version": detector.version,
        "device": getattr(detector, "device", ""),
        "frames_per_pass": len(frames),
        "repeat": args.repeat,
        "frames_total": len(latencies),
        "load_ms": load_ms,
        "first_frame_ms": first_frame_ms,
        "warmup_ms": load_ms + first_frame_ms,
        "p50_ms": percentile(latencies, 50),
        "p95_ms": percentile(latencies, 95),
        "mean_ms": round(total_ms / len(latencies), 1) if latencies else None,
        "fps": round(len(latencies) * 1000 / total_ms, 2) if total_ms else None,
        "contract_violations": violations,
        "passes": summaries,
        "notes": notes,
    }
    print(json.dumps(out, ensure_ascii=False, indent=2))
    return 0 if not violations else 1


def write_info_file(path: Path, info: dict) -> None:
    """Write through a temporary file in the same directory and rename, so readers never see a partial file."""
    tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(info), encoding="utf-8")
    os.replace(tmp, path)


def remove_info_file(path: Path, pid: int) -> None:
    """Remove the info file unless another worker has replaced it since."""
    try:
        if json.loads(path.read_text(encoding="utf-8")).get("pid") == pid:
            path.unlink()
    except (OSError, ValueError):
        pass


def _install_stop_handlers(stop: threading.Event) -> None:
    def handler(signum, frame):
        stop.set()

    for name in STOP_SIGNALS:
        signum = getattr(signal, name, None)
        if signum is not None:
            signal.signal(signum, handler)


def _set_on_stdin_eof(stop: threading.Event) -> None:
    # Reads the raw descriptor: a daemon thread parked inside sys.stdin.buffer holds its lock and aborts
    # interpreter shutdown.
    try:
        fd = sys.stdin.fileno()
        while os.read(fd, 4096):
            pass
    except (AttributeError, OSError, ValueError):
        pass
    stop.set()


def cmd_serve(args: argparse.Namespace) -> int:
    started_utc_ms = int(time.time() * 1000)
    token = args.token or os.environ.get("FOVEA_WORKER_TOKEN") or ""
    if args.token_file:
        token = Path(args.token_file).read_text(encoding="utf-8").strip()
    if not token:
        print("a token is required (--token, --token-file or FOVEA_WORKER_TOKEN)", file=sys.stderr)
        return 2
    stop = threading.Event()
    _install_stop_handlers(stop)
    backend = load_backend(args.backend)
    detector = load_detector(args.detector)
    server = serve(backend, "127.0.0.1", args.port, token, detector=detector)
    threading.Thread(target=server.serve_forever, kwargs={"poll_interval": STOP_POLL_S}, name="http", daemon=True).start()
    pid = os.getpid()
    info = {"port": server.server_address[1], "pid": pid, "backend": backend.name, "model": backend.version,
            "detector": detector.name, "detector_model": detector.version, "started_utc_ms": started_utc_ms}
    info_file = Path(args.info_file) if args.info_file else None
    if info_file is not None:
        write_info_file(info_file, info)
    print(json.dumps(info), flush=True)
    with _stdout_to_stderr():
        if args.exit_on_stdin_eof:
            threading.Thread(target=_set_on_stdin_eof, args=(stop,), name="stdin", daemon=True).start()
        if args.warmup:
            threading.Thread(target=server.state.warm_up, args=(DETECT_LANE,), name="warmup", daemon=True).start()
        while not stop.wait(STOP_POLL_S):
            pass
        server.shutdown()
        server.server_close()
        if not server.state.wait_idle(STOP_GRACE_S):
            print(f"fovea-worker: jobs still running {STOP_GRACE_S:g} s after stop; exiting", file=sys.stderr)
        if info_file is not None:
            remove_info_file(info_file, pid)
    return 0


def _add_detect_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("frames_dir")
    parser.add_argument("--detector", default="rfdetr", choices=DETECTOR_BACKENDS)
    parser.add_argument("--fps", type=float, default=2.0, help="sampling rate used to synthesize pts")
    parser.add_argument("--session", default="cli", help="stream session id; one tracker per camera:session")
    parser.add_argument("--camera", default="cli")
    parser.add_argument("--threshold", type=float, default=DETECT_DEFAULT_THRESHOLD)
    parser.add_argument("--classes", help="comma separated target classes (default person,car,truck,bus,motorcycle,bicycle)")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="fovea-worker")
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("analyze-clip", help="run one VLM job over a directory of frames")
    a.add_argument("frames_dir")
    a.add_argument("--backend", default="dry", choices=VLM_BACKENDS)
    a.add_argument("--fps", type=float, default=2.0)
    a.add_argument("--prompt", default="Describe what happens in these frames. Mention people, vehicles and objects.")
    a.add_argument("--language", default="ko")
    a.add_argument("--max-frames", type=int, default=16)
    a.add_argument("--max-new-tokens", type=int, default=256)
    a.add_argument("--clip", help="optional video file of the same span; backends with a native video path use it")
    a.set_defaults(func=cmd_analyze)
    d = sub.add_parser("detect-frames", help="run one detection+tracking job over a directory of frames")
    _add_detect_args(d)
    d.set_defaults(func=cmd_detect)
    b = sub.add_parser("bench-detect", help="measure per-frame detection latency over repeated passes")
    _add_detect_args(b)
    b.add_argument("--repeat", type=int, default=2)
    b.set_defaults(func=cmd_bench_detect)
    s = sub.add_parser("serve", help="serve the loopback job API until SIGTERM, SIGINT or CTRL_BREAK")
    s.add_argument("--backend", default="dry", choices=VLM_BACKENDS)
    s.add_argument("--detector", default="rfdetr", choices=DETECTOR_BACKENDS)
    s.add_argument("--port", type=int, default=0)
    s.add_argument("--token")
    s.add_argument("--token-file")
    s.add_argument("--info-file", help="written atomically once the socket is bound; removed on a clean exit")
    s.add_argument("--warmup", action="store_true", help="load the detector and run one inference right after binding")
    s.add_argument("--exit-on-stdin-eof", action="store_true",
                   help="stop when stdin reaches end of file, so a parent holding the write end ties our lifetime to its own")
    s.set_defaults(func=cmd_serve)
    bench_serve.add_parser(sub)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
