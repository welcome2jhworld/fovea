"""`serve` as a child process, started the way fovea-core starts it. Dry backends only."""
import importlib.util
import json
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from fovea_worker.bench_serve import WorkerClient, request_stop, spawn_serve, wait_for_info
from fovea_worker.cli import remove_info_file, write_info_file

TOKEN = "test-token"
EXIT_TIMEOUT_S = 15.0


class ServeProcessTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.info_path = self.dir / "worker.json"
        self.proc: subprocess.Popen | None = None

    def tearDown(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait()
            for stream in (self.proc.stdin, self.proc.stdout):
                if stream is not None and not stream.closed:
                    stream.close()
        self._tmp.cleanup()

    def _spawn(self, *extra: str) -> dict:
        self.proc = spawn_serve(self.dir, TOKEN, ["--backend", "dry", "--detector", "dry", "--embedder", "dry", *extra],
                                stdout=subprocess.PIPE)
        info = wait_for_info(self.info_path, self.proc, 20.0)
        self.assertIsNotNone(info, f"no info file, exit code {self.proc.poll()}")
        return info

    def _wait_exit(self) -> int:
        try:
            return self.proc.wait(timeout=EXIT_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            self.fail("worker did not exit")

    def test_info_file_is_complete_and_the_port_is_already_listening(self):
        before_ms = int(time.time() * 1000)
        info = self._spawn()
        self.assertEqual(set(info), {"port", "pid", "backend", "model", "detector", "detector_model", "started_utc_ms"})
        self.assertEqual(info["pid"], self.proc.pid)
        self.assertEqual((info["backend"], info["detector"]), ("dry", "dry"))
        self.assertEqual((info["model"], info["detector_model"]), ("0", "0"))
        self.assertLessEqual(abs(info["started_utc_ms"] - before_ms), 60_000)
        code, health = WorkerClient(info["port"], TOKEN).call("GET", "/v1/health", timeout_s=5)
        self.assertEqual(code, 200)
        self.assertEqual(health["detector_state"], "unloaded")
        self.assertEqual(list(self.dir.glob("*.tmp")), [])
        self.assertEqual(json.loads(self.proc.stdout.readline()), info)

    def test_stop_signal_exits_cleanly_and_removes_the_info_file(self):
        self._spawn()
        request_stop(self.proc, "signal")
        self.assertEqual(self._wait_exit(), 0)
        self.assertFalse(self.info_path.exists())

    def test_stop_signal_exits_cleanly_while_stdin_is_still_watched(self):
        self._spawn("--exit-on-stdin-eof")
        request_stop(self.proc, "signal")
        self.assertEqual(self._wait_exit(), 0)

    def test_stdin_eof_stops_the_worker_when_asked_to(self):
        self._spawn("--exit-on-stdin-eof")
        request_stop(self.proc, "stdin")
        self.assertEqual(self._wait_exit(), 0)
        self.assertFalse(self.info_path.exists())

    def test_stdin_eof_is_ignored_without_the_flag(self):
        info = self._spawn()
        self.proc.stdin.close()
        time.sleep(0.5)
        self.assertIsNone(self.proc.poll())
        code, _ = WorkerClient(info["port"], TOKEN).call("GET", "/v1/health", timeout_s=5)
        self.assertEqual(code, 200)

    def test_warmup_flag_makes_the_detector_ready_without_a_job(self):
        info = self._spawn("--warmup")
        client = WorkerClient(info["port"], TOKEN)
        deadline = time.monotonic() + 10
        health: dict = {}
        while time.monotonic() < deadline and health.get("detector_state") != "ready":
            _, health = client.call("GET", "/v1/health", timeout_s=5)
            time.sleep(0.02)
        self.assertEqual(health["detector_state"], "ready")
        self.assertEqual(health["state"], "unloaded")

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "numpy not importable")
    def test_dry_embedder_preloads_and_answers_a_query(self):
        info = self._spawn("--embed-preload", "qwen3vl-emb-2b-1024")
        client = WorkerClient(info["port"], TOKEN)
        deadline = time.monotonic() + 10
        health: dict = {}
        while time.monotonic() < deadline and health.get("embed_state") != "ready":
            _, health = client.call("GET", "/v1/health", timeout_s=5)
            time.sleep(0.02)
        self.assertEqual(health["embed_versions"]["qwen3vl-emb-2b-1024"]["state"], "ready")
        self.assertEqual(health["embed_versions"]["siglip2-b16-224"]["state"], "unloaded")
        code, body = client.call("POST", "/v1/jobs", {"job_id": "q", "kind": "embed_text", "texts": ["빨간 트럭"],
                                                      "index_version_name": "qwen3vl-emb-2b-1024"}, timeout_s=5)
        self.assertEqual((code, body["status"], body["dims"], body["contract_violations"]), (200, "dry_run", 1024, []))

    def test_unknown_preload_is_a_usage_error(self):
        self.proc = spawn_serve(self.dir, TOKEN, ["--backend", "dry", "--detector", "dry", "--embed-preload", "clip"])
        self.assertEqual(self._wait_exit(), 2)


class InfoFileTest(unittest.TestCase):
    def test_write_replaces_atomically_and_remove_respects_the_owner(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "worker.json"
            path.write_text("stale", encoding="utf-8")
            write_info_file(path, {"port": 1, "pid": 42})
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), {"port": 1, "pid": 42})
            self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), ["worker.json"])
            remove_info_file(path, 7)
            self.assertTrue(path.exists())
            remove_info_file(path, 42)
            self.assertFalse(path.exists())
            remove_info_file(path, 42)


if __name__ == "__main__":
    unittest.main()
