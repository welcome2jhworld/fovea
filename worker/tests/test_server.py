import contextlib
import io
import json
import threading
import unittest
import urllib.request

from fovea_worker.backends.dry import DryBackend
from fovea_worker.protocol import DetectionFrame, Job, Result
from fovea_worker.server import serve


class FakeDetector:
    name = "fake-detector"
    version = "0"

    def __init__(self) -> None:
        self.loads = 0
        self.jobs: list[Job] = []

    def load(self) -> None:
        self.loads += 1

    def run(self, job: Job) -> Result:
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
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
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
        frames = [{"frame_id": "f0", "pts_ns": 0, "recv_mono_ns": 0, "capture_utc_ms": 0, "path": "/tmp/x.jpg", "index": 0}]
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


if __name__ == "__main__":
    unittest.main()
