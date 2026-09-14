"""Embed lane routing, lane separation and query priority with fake backends. Standard library only."""
import contextlib
import io
import json
import threading
import time
import unittest
import urllib.error
import urllib.request

from embed_fakes import embed_frames_dict, embed_text_dict, ok_result
from fovea_worker.backends.dry import DryBackend
from fovea_worker.embedding import EmbedResult, ModelUnavailable
from fovea_worker.protocol import DetectionFrame, Result
from fovea_worker.server import LaneLock, serve

WAIT_S = 5


class FakeDetector:
    name = "fake-detector"
    version = "0"
    device = "fake"

    def load(self):
        return None

    def run(self, job):
        frames = [DetectionFrame(f.frame_id, f.pts_ns, 640, 360, []) for f in job.frames]
        return Result(job.job_id, job.generation, "ok", [], self.name, self.version, len(frames), 1, frames=frames)


class FakeEmbedBackend:
    name = "fake-embed"
    version = "siglip2-b16-224"
    device = "fake"

    def __init__(self):
        self.events: list[str] = []
        self.gates: dict[str, threading.Event] = {}
        self.started: dict[str, threading.Event] = {}
        self.load_error: Exception | None = None
        self.run_error: Exception | None = None
        self.batch_size = 32

    def load(self):
        return None

    def versions_health(self):
        return {"siglip2-b16-224": {"model_id": "google/siglip2-base-patch16-224", "dims": 768, "state": "ready",
                                    "load_ms": 1, "load_error": "", "device": "fake"}}

    def _step(self, label):
        self.started.setdefault(label, threading.Event()).set()
        gate = self.gates.get(label)
        if gate is not None:
            gate.wait(WAIT_S)
        self.events.append(label)

    def run(self, job, between_batches=None):
        if self.load_error is not None:
            raise self.load_error
        if self.run_error is not None:
            raise self.run_error
        count = len(job.frames) or len(job.texts)
        for batch, start in enumerate(range(0, count, self.batch_size)):
            if start and between_batches is not None:
                between_batches()
            self._step(f"{job.job_id}:{batch}")
        return ok_result(job)


class LaneLockTest(unittest.TestCase):
    def _waiter(self, lock, priority, order, label, timeout=WAIT_S):
        def body():
            if lock.acquire(timeout=timeout, priority=priority):
                order.append(label)
                lock.release()
            else:
                order.append(f"{label}:timeout")
        thread = threading.Thread(target=body)
        thread.start()
        return thread

    def _wait_for_waiters(self, lock, n):
        deadline = time.monotonic() + WAIT_S
        while len(lock._waiting) < n and time.monotonic() < deadline:
            time.sleep(0.005)
        self.assertEqual(len(lock._waiting), n)

    def test_query_waiters_go_before_earlier_index_waiters(self):
        lock = LaneLock()
        order: list[str] = []
        lock.acquire()
        threads = [self._waiter(lock, "index", order, "index-1")]
        self._wait_for_waiters(lock, 1)
        threads.append(self._waiter(lock, "index", order, "index-2"))
        self._wait_for_waiters(lock, 2)
        threads.append(self._waiter(lock, "query", order, "query"))
        self._wait_for_waiters(lock, 3)
        lock.release()
        for t in threads:
            t.join()
        self.assertEqual(order, ["query", "index-1", "index-2"])

    def test_a_timed_out_head_does_not_block_later_waiters(self):
        lock = LaneLock()
        order: list[str] = []
        lock.acquire()
        query = self._waiter(lock, "query", order, "query", timeout=0.1)
        self._wait_for_waiters(lock, 1)
        index = self._waiter(lock, "index", order, "index")
        query.join()
        self.assertEqual(order, ["query:timeout"])
        lock.release()
        index.join()
        self.assertEqual(order, ["query:timeout", "index"])
        self.assertTrue(lock.acquire(timeout=0))

    def test_yield_hands_the_lane_to_a_query_and_resumes_before_other_index_jobs(self):
        lock = LaneLock()
        order: list[str] = []
        lock.acquire(priority="index")
        self.assertFalse(lock.yield_to_query("index"))
        threads = [self._waiter(lock, "index", order, "index-later")]
        self._wait_for_waiters(lock, 1)
        self.assertFalse(lock.yield_to_query("index"))
        threads.append(self._waiter(lock, "query", order, "query"))
        self._wait_for_waiters(lock, 2)
        self.assertFalse(lock.yield_to_query("query"))
        self.assertTrue(lock.yield_to_query("index"))
        order.append("holder-resumed")
        lock.release()
        for t in threads:
            t.join()
        self.assertEqual(order, ["query", "holder-resumed", "index-later"])


class EmbedLaneServerTest(unittest.TestCase):
    def setUp(self):
        self.embedder = FakeEmbedBackend()
        self.server = serve(DryBackend(), "127.0.0.1", 0, "tok", detector=FakeDetector(), embedder=self.embedder)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()

    def tearDown(self):
        for gate in self.embedder.gates.values():
            gate.set()
        self.server.shutdown()
        self.server.server_close()

    def _call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=data, method=method,
                                     headers={"Authorization": "Bearer tok", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=WAIT_S * 2) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def _post_async(self, body, results, key):
        thread = threading.Thread(target=lambda: results.__setitem__(key, self._call("POST", "/v1/jobs", body)))
        thread.start()
        return thread

    def _gate(self, label):
        self.embedder.gates[label] = threading.Event()
        self.embedder.started[label] = threading.Event()
        return self.embedder.gates[label]

    def _wait_started(self, label):
        self.assertTrue(self.embedder.started[label].wait(WAIT_S), f"{label} never started")

    def _wait_lane_waiters(self, n):
        lock = self.server.state.locks["embed"]
        deadline = time.monotonic() + WAIT_S
        while len(lock._waiting) < n and time.monotonic() < deadline:
            time.sleep(0.005)
        self.assertEqual(len(lock._waiting), n)

    def test_embed_jobs_return_vectors_with_a_valid_contract(self):
        code, body = self._call("POST", "/v1/jobs", embed_frames_dict(3))
        self.assertEqual(code, 200)
        self.assertEqual((body["status"], body["count"], body["dims"]), ("ok", 3, 768))
        self.assertEqual(body["frame_ids"], ["f0", "f1", "f2"])
        self.assertEqual(body["contract_violations"], [])
        self.assertEqual(len(body["index_version"]), 12)
        code, text = self._call("POST", "/v1/jobs", embed_text_dict(["a", "b"]))
        self.assertEqual((code, text["count"], text["frame_ids"]), (200, 2, []))
        self.assertEqual(text["index_version"], body["index_version"])
        self.assertEqual(text["contract_violations"], [])
        _, health = self._call("GET", "/v1/health")
        self.assertEqual((health["embedder"], health["embed_state"]), ("fake-embed", "ready"))
        self.assertEqual(health["embed_turnaround_ms"]["jobs"], 1, "the job that loaded the lane is excluded")
        self.assertEqual(health["embed_versions"]["siglip2-b16-224"]["dims"], 768)
        self.assertEqual(health["detector_turnaround_ms"]["jobs"], 0)

    def test_detection_never_waits_for_a_running_embedding(self):
        gate = self._gate("e1:0")
        results: dict = {}
        embed = self._post_async(embed_frames_dict(2), results, "embed")
        self._wait_started("e1:0")
        detect = {"job_id": "d", "kind": "detect_frames", "camera_id": "c", "session_id": "s", "generation": 1,
                  "frames": embed_frames_dict(1)["frames"], "limits": {"deadline_ms": 1000}}
        code, body = self._call("POST", "/v1/jobs", detect)
        self.assertEqual((code, body["status"]), (200, "ok"))
        self.assertNotIn("embed", results)
        gate.set()
        embed.join()
        self.assertEqual(results["embed"][0], 200)

    def test_a_waiting_query_starts_before_waiting_index_jobs(self):
        gate = self._gate("running:0")
        results: dict = {}
        threads = [self._post_async(embed_frames_dict(job_id="running"), results, "running")]
        self._wait_started("running:0")
        threads.append(self._post_async(embed_frames_dict(job_id="index-queued"), results, "index"))
        self._wait_lane_waiters(1)
        threads.append(self._post_async(embed_text_dict(job_id="query"), results, "query"))
        self._wait_lane_waiters(2)
        gate.set()
        for t in threads:
            t.join()
        self.assertEqual(self.embedder.events, ["running:0", "query:0", "index-queued:0"])
        self.assertEqual({k: v[0] for k, v in results.items()}, {"running": 200, "index": 200, "query": 200})

    def test_a_query_runs_between_the_batches_of_a_long_index_job(self):
        self.embedder.batch_size = 2
        gate = self._gate("long:0")
        results: dict = {}
        long_job = self._post_async(embed_frames_dict(6, job_id="long"), results, "long")
        self._wait_started("long:0")
        query = self._post_async(embed_text_dict(job_id="query", limits={"deadline_ms": 3000}), results, "query")
        self._wait_lane_waiters(1)
        gate.set()
        query.join()
        long_job.join()
        self.assertEqual(self.embedder.events, ["long:0", "query:0", "long:1", "long:2"])
        self.assertEqual(results["query"][0], 200)
        self.assertEqual(results["long"][1]["contract_violations"], [])

    def test_model_load_failure_is_503_and_backend_errors_are_error_results(self):
        self.embedder.load_error = ModelUnavailable("qwen3vl-emb-2b-1024: weights missing at rtsp://u:secret@h/x")
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            code, body = self._call("POST", "/v1/jobs", embed_text_dict())
        self.assertEqual((code, body["error"]["code"]), (503, "backend_load_failed"))
        self.assertNotIn("secret", body["error"]["message"] + stderr.getvalue())
        self.embedder.load_error = None
        self.embedder.run_error = RuntimeError("MPS out of memory")
        with contextlib.redirect_stderr(io.StringIO()):
            code, body = self._call("POST", "/v1/jobs", embed_frames_dict())
        self.assertEqual((code, body["status"], body["model"]), (200, "error", "fake-embed"))
        self.assertEqual(body["error"], "RuntimeError: MPS out of memory")
        self.assertEqual(body["contract_violations"], [])
        self.assertEqual(set(body), set(EmbedResult("", 0, "", "", "").to_dict()) | {"contract_violations"})

    def test_query_waiting_past_its_deadline_is_refused(self):
        with self.server.state.locks["embed"]:
            code, body = self._call("POST", "/v1/jobs", embed_text_dict(limits={"deadline_ms": 50}))
        self.assertEqual((code, body["error"]["code"]), (503, "deadline_exceeded"))
        self.assertEqual(self.embedder.events, [])

    def test_bad_embed_job_is_400(self):
        code, body = self._call("POST", "/v1/jobs", embed_frames_dict(33))
        self.assertEqual(code, 400)
        self.assertIn("at most 32", body["error"]["message"])


if __name__ == "__main__":
    unittest.main()
