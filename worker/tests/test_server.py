import contextlib
import io
import json
import threading
import time
import unittest
import urllib.error
import urllib.request

from fovea_worker.backends.dry import DryBackend
from fovea_worker.protocol import DetectionFrame, Job, Result
from fovea_worker.server import serve


class FakeDetector:
    name = "fake-detector"
    version = "0"

    def __init__(self) -> None:
        self.loads = 0
        self.warmups = 0
        self.device = ""
        self.jobs: list[Job] = []
        self.load_gate: threading.Event | None = None
        self.run_gate: threading.Event | None = None
        self.load_error: Exception | None = None

    def load(self) -> None:
        if self.load_gate is not None:
            self.load_gate.wait(5)
        if self.load_error is not None:
            raise self.load_error
        self.loads += 1
        self.device = "fake-gpu"

    def warmup(self) -> None:
        self.warmups += 1

    def run(self, job: Job) -> Result:
        if self.run_gate is not None:
            self.run_gate.wait(5)
        self.jobs.append(job)
        frames = [DetectionFrame(f.frame_id, f.pts_ns, 640, 360, []) for f in job.frames]
        return Result(job.job_id, job.generation, "ok", [], self.name, self.version, len(frames), 1,
                      frames=frames, per_frame_ms=[1] * len(frames), device="fake")


class RaisingDetector(FakeDetector):
    name = "raising-detector"

    def run(self, job: Job) -> Result:
        raise RuntimeError("inference failed at rtsp://admin:secret@10.0.0.5/stream")


class ServerRoutingTest(unittest.TestCase):
    def setUp(self):
        self.detector = FakeDetector()
        self.vlm = DryBackend()
        self._start()

    def _start(self):
        self.server = serve(self.vlm, "127.0.0.1", 0, "tok", detector=self.detector)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def _call(self, method, path, body=None, token="tok"):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data, method=method,
                                     headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def _job(self, kind, **extra):
        frames = [{"frame_id": "f0", "pts_ns": 0, "recv_mono_ns": 0, "capture_utc_ms": 0, "path": "x.jpg", "index": 0}]
        d = {"job_id": "j", "kind": kind, "camera_id": "c", "session_id": "s", "generation": 1,
             "frames": frames, "clip": None, "gaps": [], "limits": {}}
        d.update(extra)
        return d

    def _restart_with_detector(self, detector):
        self.tearDown()
        self.detector = detector
        self._start()

    def test_detect_jobs_go_to_detector_and_vlm_jobs_to_backend(self):
        code, body = self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual(code, 200)
        self.assertEqual(body["model"], "fake-detector")
        self.assertEqual(body["frames"][0]["frame_id"], "f0")
        self.assertEqual(body["contract_violations"], [])
        self.assertEqual(self.detector.jobs[0].session_id, "s")
        code, body = self._call("POST", "/v1/jobs", self._job("vlm_clip"))
        self.assertEqual(code, 200)
        self.assertEqual(body["model"], "dry")
        self.assertEqual(body["status"], "dry_run")
        self.assertEqual(len(self.detector.jobs), 1)

    def test_detector_loads_lazily_once(self):
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector"], "fake-detector")
        self.assertFalse(health["detector_loaded"])
        self.assertFalse(health["loaded"])
        self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual(self.detector.loads, 1)
        _, health = self._call("GET", "/v1/health")
        self.assertTrue(health["detector_loaded"])
        self.assertFalse(health["loaded"])

    def test_rejects_bad_token_and_unknown_kind(self):
        code, _ = self._call("GET", "/v1/health", token="nope")
        self.assertEqual(code, 401)
        code, body = self._call("POST", "/v1/jobs", self._job("magic"))
        self.assertEqual(code, 400)

    def test_rejects_detect_job_without_session(self):
        job = self._job("detect_frames")
        del job["session_id"]
        code, body = self._call("POST", "/v1/jobs", job)
        self.assertEqual(code, 400)
        self.assertIn("session_id", body["error"]["message"])
        self.assertEqual(self.detector.jobs, [])

    def test_backend_exception_returns_error_result_with_model(self):
        self._restart_with_detector(RaisingDetector())
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code, body = self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual(code, 200)
        self.assertEqual(body["status"], "error")
        self.assertEqual(body["model"], "raising-detector")
        self.assertEqual(body["model_version"], "0")
        self.assertEqual(body["input_frames"], 1)
        self.assertEqual(body["error"], "RuntimeError: inference failed at rtsp://***@10.0.0.5/stream")
        self.assertEqual(body["contract_violations"], [])
        self.assertNotIn("secret", stderr.getvalue())
        self.assertIn("RuntimeError", stderr.getvalue())

    def test_ping_is_answered_while_the_vlm_lane_is_busy(self):
        with self.server.state.locks["vlm"]:
            code, body = self._call("POST", "/v1/jobs", self._job("ping", frames=[]))
        self.assertEqual(code, 200)
        self.assertEqual(body["status"], "ok")
        self.assertEqual(body["model"], "dry")
        self.assertEqual(body["contract_violations"], [])

    def test_embed_frames_is_not_implemented(self):
        code, body = self._call("POST", "/v1/jobs", self._job("embed_frames"))
        self.assertEqual(code, 501)
        self.assertEqual(body["error"]["code"], "not_implemented")

    def test_job_waiting_past_its_deadline_is_refused(self):
        with self.server.state.locks["detect"]:
            code, body = self._call("POST", "/v1/jobs", self._job("detect_frames", limits={"deadline_ms": 50}))
        self.assertEqual(code, 503)
        self.assertEqual(body["error"]["code"], "deadline_exceeded")
        self.assertEqual(self.detector.jobs, [])
        code, _ = self._call("POST", "/v1/jobs", self._job("detect_frames", limits={"deadline_ms": 50}))
        self.assertEqual(code, 200)
        self.assertGreater(self.detector.jobs[0].accepted_mono_ns, 0)


    def test_health_reports_lane_state_device_and_turnaround(self):
        _, health = self._call("GET", "/v1/health")
        self.assertEqual((health["detector_state"], health["detector_device"]), ("unloaded", ""))
        self.assertEqual(health["detector_turnaround_ms"], {"jobs": 0, "p50": None, "p95": None})
        self.assertEqual(health["state"], "unloaded")
        for _ in range(3):
            self._call("POST", "/v1/jobs", self._job("detect_frames"))
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_state"], "ready")
        self.assertTrue(health["detector_loaded"])
        self.assertEqual(health["detector_device"], "fake-gpu")
        self.assertIsInstance(health["detector_load_ms"], int)
        self.assertEqual(health["detector_load_error"], "")
        turnaround = health["detector_turnaround_ms"]
        self.assertEqual(turnaround["jobs"], 2, "the job that loaded the model is excluded")
        self.assertLessEqual(turnaround["p50"], turnaround["p95"])
        self.assertEqual(health["turnaround_ms"]["jobs"], 0)

    def test_turnaround_includes_lane_wait(self):
        self._call("POST", "/v1/jobs", self._job("detect_frames"))
        with self.server.state.locks["detect"]:
            worker = threading.Thread(target=self._call, args=("POST", "/v1/jobs", self._job("detect_frames")))
            worker.start()
            time.sleep(0.2)
        worker.join()
        _, health = self._call("GET", "/v1/health")
        # The request is accepted a few ms after the lock is taken, so the measured wait is a little under 200 ms.
        self.assertGreaterEqual(health["detector_turnaround_ms"]["p50"], 150)

    def test_warm_up_loads_and_warms_once_before_any_job(self):
        self.assertTrue(self.server.state.warm_up("detect"))
        self.assertEqual((self.detector.loads, self.detector.warmups), (1, 1))
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_state"], "ready")
        self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual((self.detector.loads, self.detector.warmups), (1, 1))
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_turnaround_ms"]["jobs"], 1)
        self.assertFalse(health["loaded"])

    def test_lazy_load_does_not_run_the_warmup_inference(self):
        self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual((self.detector.loads, self.detector.warmups), (1, 0))

    def test_health_answers_while_the_detector_is_loading(self):
        self.detector.load_gate = threading.Event()
        warm = threading.Thread(target=self.server.state.warm_up, args=("detect",))
        warm.start()
        try:
            deadline = time.monotonic() + 5
            state = ""
            while time.monotonic() < deadline and state != "loading":
                _, health = self._call("GET", "/v1/health")
                state = health["detector_state"]
            self.assertEqual(state, "loading")
            self.assertFalse(health["detector_loaded"])
        finally:
            self.detector.load_gate.set()
            warm.join()
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_state"], "ready")

    def test_failed_load_is_reported_and_retried_by_the_next_job(self):
        self.detector.load_error = OSError("weights missing at rtsp://admin:secret@cam/x")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(self.server.state.warm_up("detect"))
            code, body = self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual(code, 503)
        self.assertEqual(body["error"]["code"], "backend_load_failed")
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_state"], "failed")
        self.assertIn("weights missing", health["detector_load_error"])
        self.assertNotIn("secret", health["detector_load_error"])
        self.detector.load_error = None
        code, _ = self._call("POST", "/v1/jobs", self._job("detect_frames"))
        self.assertEqual(code, 200)
        _, health = self._call("GET", "/v1/health")
        self.assertEqual((health["detector_state"], health["detector_load_error"]), ("ready", ""))

    def test_health_reports_how_long_the_detector_lane_is_busy(self):
        self._call("POST", "/v1/jobs", self._job("detect_frames"))
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_busy_ms"], 0)
        self.detector.run_gate = threading.Event()
        worker = threading.Thread(target=self._call, args=("POST", "/v1/jobs", self._job("detect_frames")))
        worker.start()
        try:
            time.sleep(0.3)
            _, health = self._call("GET", "/v1/health")
        finally:
            self.detector.run_gate.set()
            worker.join()
        self.assertGreaterEqual(health["detector_busy_ms"], 250)
        self.assertEqual(health["busy_ms"], 0)
        _, health = self._call("GET", "/v1/health")
        self.assertEqual(health["detector_busy_ms"], 0)

    def test_wait_idle_waits_for_running_jobs(self):
        self.detector.run_gate = threading.Event()
        worker = threading.Thread(target=self._call, args=("POST", "/v1/jobs", self._job("detect_frames")))
        worker.start()
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and self.server.state.in_flight == 0:
                time.sleep(0.01)
            self.assertFalse(self.server.state.wait_idle(0.05))
        finally:
            self.detector.run_gate.set()
        self.assertTrue(self.server.state.wait_idle(5))
        worker.join()


if __name__ == "__main__":
    unittest.main()
