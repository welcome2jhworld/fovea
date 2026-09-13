"""Runs the real RF-DETR model. Skipped unless torch, rfdetr, supervision and PIL import.

FOVEA_TEST_FRAMES_DIR may point at a directory of CCTV frames containing a car;
the tracking assertion then runs on real detections.
"""
import importlib
import os
import tempfile
import unittest
from pathlib import Path

from fovea_worker.protocol import Job, validate_result_against_job


def _importable(*names: str) -> bool:
    for name in names:
        try:
            importlib.import_module(name)
        except Exception:
            return False
    return True


HAVE_STACK = _importable("torch", "rfdetr", "supervision", "PIL")
FRAMES_DIR = os.environ.get("FOVEA_TEST_FRAMES_DIR", "")


def _job(paths: list[str], session="it", fps=2.0) -> Job:
    frames = [{"frame_id": f"f{i}", "pts_ns": int(i / fps * 1e9), "recv_mono_ns": 0, "capture_utc_ms": 0,
               "path": p, "index": i} for i, p in enumerate(paths)]
    return Job.from_dict({"job_id": "it", "kind": "detect_frames", "camera_id": "cam", "session_id": session,
                          "generation": 1, "frames": frames, "clip": None, "gaps": [], "limits": {}})


@unittest.skipUnless(HAVE_STACK, "torch/rfdetr/supervision/PIL not importable")
class RfDetrIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from fovea_worker.backends.detector_rfdetr import RfDetrTracker

        cls.detector = RfDetrTracker()
        cls.detector.load()

    def test_blank_frame_yields_valid_empty_result(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as d:
            path = str(Path(d) / "blank.jpg")
            Image.new("RGB", (640, 360), (90, 90, 90)).save(path)
            job = _job([path], session="blank")
            result = self.detector.run(job)
        self.assertEqual(result.status, "ok")
        self.assertEqual(validate_result_against_job(result, job), [])
        self.assertIn(result.device, ("cuda", "mps", "cpu"))
        self.assertEqual(len(result.frames), 1)
        self.assertEqual((result.frames[0].width, result.frames[0].height), (640, 360))
        self.assertEqual(len(result.per_frame_ms), 1)
        if os.environ.get("FOVEA_DETECTOR_TRACE", "1") != "0":
            self.assertTrue(self.detector.model._is_optimized_for_inference, result.notes)

    @unittest.skipUnless(FRAMES_DIR and Path(FRAMES_DIR).is_dir(), "FOVEA_TEST_FRAMES_DIR not set")
    def test_real_frames_detect_car_with_stable_track(self):
        paths = sorted(str(p) for p in Path(FRAMES_DIR).iterdir() if p.suffix.lower() in {".jpg", ".png"})[:5]
        job = _job(paths, session="real")
        result = self.detector.run(job)
        self.assertEqual(result.status, "ok")
        self.assertEqual(validate_result_against_job(result, job), [])
        cars = [d for f in result.frames for d in f.detections if d.cls == "car"]
        self.assertGreaterEqual(len(cars), 4, "expected a car in at least four of the first five frames")
        track_ids = [d.track_id for d in cars]
        self.assertEqual(set(track_ids), {track_ids[0]}, f"expected one stable car track, got {track_ids}")
        self.assertIsNotNone(track_ids[0])


if __name__ == "__main__":
    unittest.main()
