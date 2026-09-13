"""Loopback JSON API for fovea-core. Standard library only.

POST /v1/jobs      run one job synchronously; kind detect_frames goes to the
                   detector lane, vlm_clip to the VLM lane. Each lane
                   serializes its own backend, so a detection job does not wait
                   behind a VLM clip and both models stay resident. A job that
                   cannot start within limits.deadline_ms gets 503
                   deadline_exceeded. ping is answered without entering a lane;
                   embed_frames is 501 until a backend implements it.
GET  /v1/health    backend names, models, loaded flags
"""
from __future__ import annotations

import json
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .clock import mono_ns
from .protocol import Job, Result, validate_result_against_job
from .redact import redact_text

VLM_LANE = "vlm"
DETECT_LANE = "detect"


def _error(code: str, message: str) -> dict:
    return {"error": {"code": code, "message": message}}


def _log(message: str) -> None:
    print(f"fovea-worker: {redact_text(message)}", file=sys.stderr, flush=True)


class JobRefused(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code


class WorkerState:
    def __init__(self, backend, token: str, detector=None) -> None:
        self.backends = {VLM_LANE: backend, DETECT_LANE: detector}
        self.token = token
        self.locks = {VLM_LANE: threading.Lock(), DETECT_LANE: threading.Lock()}
        self.loaded = {VLM_LANE: False, DETECT_LANE: False}

    @property
    def backend(self):
        return self.backends[VLM_LANE]

    @property
    def detector(self):
        return self.backends[DETECT_LANE]

    @staticmethod
    def lane_for(kind: str) -> str:
        return DETECT_LANE if kind == "detect_frames" else VLM_LANE

    def health(self) -> dict:
        body = {"backend": self.backend.name, "model": self.backend.version, "loaded": self.loaded[VLM_LANE]}
        detector = self.detector
        body["detector"] = detector.name if detector is not None else None
        body["detector_model"] = detector.version if detector is not None else None
        body["detector_loaded"] = self.loaded[DETECT_LANE]
        return body

    def run(self, job: Job) -> Result:
        """Run job in its lane. Raises JobRefused when no result can be produced."""
        if job.kind == "ping":
            return Result(job.job_id, job.generation, "ok", [], self.backend.name, self.backend.version, 0, 0)
        if job.kind == "embed_frames":
            raise JobRefused(501, "not_implemented", "no backend implements embed_frames")
        lane = self.lane_for(job.kind)
        backend = self.backends[lane]
        if backend is None:
            raise JobRefused(503, "backend_unavailable", f"no backend for {job.kind}")
        now = mono_ns()
        wait_s = max(0.0, (job.deadline_mono_ns(now) - now) / 1e9)
        if not self.locks[lane].acquire(timeout=wait_s):
            raise JobRefused(503, "deadline_exceeded", f"{lane} lane busy past the {job.limits.deadline_ms} ms deadline")
        try:
            if not self.loaded[lane]:
                try:
                    backend.load()
                except Exception as e:
                    message = redact_text(f"{type(e).__name__}: {e}")
                    _log(f"{backend.name} load failed: {message}")
                    raise JobRefused(503, "backend_load_failed", message) from e
                self.loaded[lane] = True
            t0 = time.monotonic()
            try:
                return backend.run(job)
            except Exception as e:
                message = redact_text(f"{type(e).__name__}: {e}")
                _log(f"job {job.job_id} ({job.kind}) failed in {backend.name}: {message}")
                return Result(job.job_id, job.generation, "error", [], backend.name, backend.version,
                              len(job.frames), int((time.monotonic() - t0) * 1000), error=message)
        finally:
            self.locks[lane].release()


def make_handler(state: WorkerState):
    class Handler(BaseHTTPRequestHandler):
        def _authorized(self) -> bool:
            auth = self.headers.get("Authorization", "")
            return auth.startswith("Bearer ") and secrets.compare_digest(auth[7:], state.token)

        def _send(self, code: int, body: dict) -> None:
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *args):
            return None

        def do_GET(self):
            if not self._authorized():
                return self._send(401, _error("unauthorized", "bad token"))
            if self.path == "/v1/health":
                return self._send(200, state.health())
            return self._send(404, _error("not_found", self.path))

        def do_POST(self):
            accepted_ns = mono_ns()
            if not self._authorized():
                return self._send(401, _error("unauthorized", "bad token"))
            if self.path != "/v1/jobs":
                return self._send(404, _error("not_found", self.path))
            try:
                length = int(self.headers.get("Content-Length", "0"))
                job = Job.from_dict(json.loads(self.rfile.read(length) or b"{}"))
            except (ValueError, KeyError, TypeError, AttributeError) as e:
                return self._send(400, _error("bad_request", str(e)))
            job.accepted_mono_ns = accepted_ns
            try:
                result = state.run(job)
            except JobRefused as e:
                return self._send(e.status, _error(e.code, str(e)))
            body = result.to_dict()
            body["contract_violations"] = validate_result_against_job(result, job)
            return self._send(200, body)

    return Handler


class WorkerServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], state: WorkerState) -> None:
        self.state = state
        super().__init__(address, make_handler(state))


def serve(backend, host: str, port: int, token: str, detector=None) -> WorkerServer:
    return WorkerServer((host, port), WorkerState(backend, token, detector))
