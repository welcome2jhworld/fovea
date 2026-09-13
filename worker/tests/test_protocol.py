import unittest

from fovea_worker.protocol import (Detection, DetectionFrame, Evidence, Job, Observation, Result, sample_frames,
                                   validate_result_against_job)


def _job():
    frames = [{"frame_id": f"f{i}", "pts_ns": i * 500_000_000, "recv_mono_ns": 0, "capture_utc_ms": 0,
               "path": f"/tmp/f{i}.jpg", "index": i} for i in range(4)]
    return Job.from_dict({"job_id": "j1", "kind": "vlm_clip", "camera_id": "c", "generation": 3,
                          "frames": frames, "clip": None, "gaps": [], "limits": {}})


class ProtocolTest(unittest.TestCase):
    def test_rejects_unknown_kind(self):
        with self.assertRaises(ValueError):
            Job.from_dict({"job_id": "x", "kind": "magic", "frames": []})

    def test_accepts_result_citing_real_frames(self):
        job = _job()
        r = Result("j1", 3, "ok", [Observation("a person", Evidence(["f1", "f2"], 500_000_000, 1_000_000_000), "low")], "m", "v", 4, 10)
        self.assertEqual(validate_result_against_job(r, job), [])

    def test_flags_fabricated_frame_and_range(self):
        job = _job()
        r = Result("j1", 3, "ok", [Observation("x", Evidence(["f9"], 0, 9_000_000_000), "low")], "m", "v", 4, 10)
        problems = validate_result_against_job(r, job)
        self.assertTrue(any("unknown frame ids" in p for p in problems))
        self.assertTrue(any("outside clip" in p for p in problems))

    def test_flags_stale_generation(self):
        job = _job()
        r = Result("j1", 2, "ok", [], "m", "v", 4, 10)
        self.assertIn("generation mismatch", validate_result_against_job(r, job))


def _frame_dicts(n):
    return [{"frame_id": f"f{i}", "pts_ns": i * 500_000_000, "recv_mono_ns": 0, "capture_utc_ms": 0,
             "path": f"/tmp/f{i}.jpg", "index": i} for i in range(n)]


def _detect_dict(**overrides):
    d = {"job_id": "d1", "kind": "detect_frames", "camera_id": "cam", "session_id": "s1",
         "generation": 5, "frames": _frame_dicts(3), "clip": None, "gaps": [], "limits": {}}
    d.update(overrides)
    return d


def _detect_job():
    return Job.from_dict(_detect_dict())


def _det(**overrides):
    base = dict(track_id="7", cls="car", confidence=0.8, bbox=[0.1, 0.2, 0.5, 0.9],
                anchor_foot=[0.3, 0.9], anchor_center=[0.3, 0.55])
    base.update(overrides)
    return Detection(**base)


def _detect_result(frames):
    return Result("d1", 5, "ok", [], "rfdetr", "rf-detr-nano", 3, 40, frames=frames)


class DetectContractTest(unittest.TestCase):
    def test_job_defaults(self):
        job = _detect_job()
        self.assertEqual(job.session_id, "s1")
        self.assertEqual(job.threshold, 0.3)
        self.assertEqual(job.target_classes, ["person", "car", "truck", "bus", "motorcycle", "bicycle"])
        vlm = _job()
        self.assertEqual(vlm.session_id, "")

    def test_job_overrides(self):
        job = Job.from_dict(_detect_dict(threshold=0.5, target_classes=["person"]))
        self.assertEqual(job.threshold, 0.5)
        self.assertEqual(job.target_classes, ["person"])

    def test_rejects_detect_job_without_camera_or_session(self):
        for missing in ("camera_id", "session_id"):
            d = _detect_dict()
            del d[missing]
            with self.assertRaisesRegex(ValueError, "camera_id and session_id"):
                Job.from_dict(d)

    def test_rejects_invalid_job_fields(self):
        frame = _frame_dicts(1)[0]
        cases = {
            "duplicate frame ids": _detect_dict(frames=[frame, dict(frame, pts_ns=1)]),
            "has no frames": _detect_dict(frames=[]),
            "threshold": _detect_dict(threshold=7),
            "threshold ": _detect_dict(threshold=-0.1),
            "threshold  ": _detect_dict(threshold=True),
            "limits.max_frames": _detect_dict(limits={"max_frames": -3}),
            "limits.deadline_ms": _detect_dict(limits={"deadline_ms": -1}),
            "limits.max_new_tokens": _detect_dict(limits={"max_new_tokens": 1.5}),
            "target_classes": _detect_dict(target_classes=[]),
            "target_classes ": _detect_dict(target_classes="person"),
            "limits.max_frames is 2": _detect_dict(limits={"max_frames": 2}),
            "differs from session_id": _detect_dict(clip={"session_id": "other", "start_pts_ns": 0, "end_pts_ns": 1,
                                                          "start_utc_ms": 0, "end_utc_ms": 0}),
        }
        for message, d in cases.items():
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message.strip()):
                Job.from_dict(d)

    def test_vlm_job_may_exceed_max_frames_and_limits_accept_integral_floats(self):
        frames = _frame_dicts(20)
        job = Job.from_dict({"job_id": "v", "kind": "vlm_clip", "frames": frames, "limits": {"max_frames": 16.0}})
        self.assertEqual(job.limits.max_frames, 16)
        self.assertIsInstance(job.limits.max_frames, int)
        self.assertEqual(len(job.frames), 20)

    def test_ping_needs_no_frames(self):
        self.assertEqual(Job.from_dict({"job_id": "p", "kind": "ping"}).frames, [])

    def test_accepts_valid_detection_frames(self):
        job = _detect_job()
        frames = [DetectionFrame("f0", 0, 640, 360, [_det()]),
                  DetectionFrame("f1", 500_000_000, 640, 360, [_det(track_id=None)]),
                  DetectionFrame("f2", 1_000_000_000, 640, 360, [])]
        self.assertEqual(validate_result_against_job(_detect_result(frames), job), [])

    def test_flags_unknown_frame_id(self):
        job = _detect_job()
        problems = validate_result_against_job(_detect_result([DetectionFrame("f9", 0, 640, 360, [])]), job)
        self.assertTrue(any("unknown frame id" in p for p in problems))

    def test_flags_pts_mismatch(self):
        job = _detect_job()
        problems = validate_result_against_job(_detect_result([DetectionFrame("f1", 0, 640, 360, [])]), job)
        self.assertTrue(any("differs from job pts" in p for p in problems))

    def test_flags_bbox_and_confidence_out_of_range(self):
        job = _detect_job()
        frames = [DetectionFrame("f0", 0, 640, 360, [_det(bbox=[0.0, 0.0, 1.2, 0.5]), _det(confidence=1.5),
                                                     _det(bbox=[0.6, 0.1, 0.2, 0.5]), _det(anchor_foot=[-0.1, 0.5])])]
        problems = validate_result_against_job(_detect_result(frames), job)
        self.assertTrue(any("detection 0: bbox" in p and "outside" in p for p in problems))
        self.assertTrue(any("detection 1: confidence" in p for p in problems))
        self.assertTrue(any("detection 2: bbox" in p and "inverted" in p for p in problems))
        self.assertTrue(any("detection 3: anchor_foot" in p for p in problems))

    def test_flags_per_frame_ms_length(self):
        job = _detect_job()
        result = _detect_result([DetectionFrame("f0", 0, 640, 360, [])])
        result.per_frame_ms = [1, 2]
        self.assertIn("per_frame_ms length differs from frames", validate_result_against_job(result, job))

    def test_to_dict_nests_detections(self):
        d = _detect_result([DetectionFrame("f0", 0, 640, 360, [_det()])]).to_dict()
        self.assertEqual(d["frames"][0]["detections"][0]["cls"], "car")
        self.assertEqual(d["frames"][0]["detections"][0]["bbox"], [0.1, 0.2, 0.5, 0.9])

    def test_flags_dropped_frames_missing_model_and_off_target_class(self):
        job = Job.from_dict(_detect_dict(frames=_frame_dicts(2), target_classes=["person"]))
        result = Result("d1", 5, "ok", [], "", "", 2, 40, frames=[DetectionFrame("f0", 0, 640, 360, [_det()])])
        problems = validate_result_against_job(result, job)
        self.assertIn("status ok with 1 of 2 frames", problems)
        self.assertIn("model or model_version missing", problems)
        self.assertTrue(any("class 'car' not in target_classes" in p for p in problems))
        result.status = "partial"
        result.model, result.model_version = "rfdetr", "rf-detr-nano"
        result.frames[0].detections = [_det(cls="person")]
        self.assertEqual(validate_result_against_job(result, job), [])

    def test_flags_unknown_status_and_bool_numbers(self):
        job = _detect_job()
        result = _detect_result([DetectionFrame("f0", 0, 640, 360, [_det(confidence=True, bbox=[0.1, 0.2, True, 0.9])]),
                                 DetectionFrame("f1", 500_000_000, 640, 360, []),
                                 DetectionFrame("f2", 1_000_000_000, True, 360, [])])
        result.status = "done"
        problems = validate_result_against_job(result, job)
        self.assertIn("unknown status 'done'", problems)
        self.assertTrue(any("detection 0: confidence" in p for p in problems))
        self.assertTrue(any("detection 0: bbox" in p for p in problems))
        self.assertTrue(any("frame 2: invalid size" in p for p in problems))

    def test_flags_evidence_range_that_does_not_match_cited_frames(self):
        job = _job()
        r = Result("j1", 3, "ok", [Observation("x", Evidence(["f1"], 0, 1_500_000_000), "low")], "m", "v", 4, 10)
        self.assertIn("observation 0: evidence range differs from cited frames", validate_result_against_job(r, job))

    def test_image_vlm_evidence_is_limited_to_sampled_frames(self):
        frames = _frame_dicts(32)
        job = Job.from_dict({"job_id": "v", "kind": "vlm_clip", "generation": 1, "frames": frames,
                             "limits": {"max_frames": 16}})
        shown = {f.frame_id for f in sample_frames(job.frames, 16)}
        unsent = next(f for f in job.frames if f.frame_id not in shown)
        r = Result("v", 1, "ok", [Observation("x", Evidence([unsent.frame_id], unsent.pts_ns, unsent.pts_ns), "low")],
                   "m", "v", 16, 10)
        self.assertTrue(any("unknown frame ids" in p for p in validate_result_against_job(r, job)))
        clip_job = Job.from_dict({"job_id": "v", "kind": "vlm_clip", "generation": 1, "frames": frames,
                                  "limits": {"max_frames": 16}, "clip_path": "/tmp/clip.mp4"})
        self.assertEqual(validate_result_against_job(r, clip_job), [])

    def test_vlm_result_unaffected(self):
        job = _job()
        r = Result("j1", 3, "ok", [Observation("a person", Evidence(["f1"], 500_000_000, 500_000_000), "low")], "m", "v", 4, 10)
        self.assertEqual(validate_result_against_job(r, job), [])


if __name__ == "__main__":
    unittest.main()
