import unittest

from fovea_worker.backends.detector_rfdetr import RfDetrTracker
from fovea_worker.protocol import FrameRef, Job, validate_result_against_job
from fovea_worker.tracking import (DEFAULT_FPS, RATE_WINDOW, RawDetection, TrackerRegistry, build_detection_frame,
                                  sort_frames)


class FakeClock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


class FakeTracker:
    """Assigns ids by insertion order and remembers every call so tests can inspect it."""

    created = 0
    LOST_WINDOW_NS = 1_500_000_000

    def __init__(self, fps: float) -> None:
        FakeTracker.created += 1
        self.serial = FakeTracker.created
        self.fps = fps
        self.resets = 0
        self.updates: list[list[RawDetection]] = []
        self.thresholds: list[float] = []
        self.next_id = 1

    def update(self, detections):
        self.updates.append(list(detections))
        ids = []
        for _ in detections:
            ids.append(str(self.next_id))
            self.next_id += 1
        return ids

    def reset(self) -> None:
        self.resets += 1
        self.next_id = 1

    def set_fps(self, fps: float) -> None:
        self.fps = fps

    def set_threshold(self, threshold: float) -> None:
        self.thresholds.append(threshold)

    def lost_window_ns(self) -> int:
        return self.LOST_WINDOW_NS


def _frames(n: int, fps: float = 2.0, start_ns: int = 0) -> list[FrameRef]:
    return [FrameRef(f"f{i:04d}", start_ns + int(i / fps * 1e9), 0, 0, f"/img/{i}.jpg", i) for i in range(n)]


class RateTracker(FakeTracker):
    """Lost window of three frames at the current rate, like ByteTrack's max_time_lost + 1 at 2 fps."""

    def lost_window_ns(self) -> int:
        return int(3 * 1e9 / self.fps)


class TrackerRegistryTest(unittest.TestCase):
    def setUp(self):
        FakeTracker.created = 0
        self.clock = FakeClock()
        self.registry = TrackerRegistry(FakeTracker, idle_s=120.0, clock=self.clock)

    def _feed(self, key: str, pts_ns: int) -> str:
        entry, _ = self.registry.acquire(key)
        note = self.registry.advance(entry, pts_ns)
        self.registry.commit(entry, pts_ns)
        return note

    def test_key_combines_camera_and_session(self):
        self.assertEqual(TrackerRegistry.key("cam1", "sess9"), "cam1:sess9")

    def test_same_key_reuses_tracker_and_other_key_gets_its_own(self):
        a, note_a = self.registry.acquire("cam:s1")
        self.assertEqual(note_a, "created")
        self.assertEqual(a.fps, DEFAULT_FPS)
        self.registry.commit(a, 500_000_000)
        again, note_again = self.registry.acquire("cam:s1")
        self.assertIs(a.tracker, again.tracker)
        self.assertEqual(note_again, "")
        self.assertEqual(self.registry.advance(again, 1_000_000_000), "")
        self.assertEqual(a.epoch, again.epoch)
        b, _ = self.registry.acquire("cam:s2")
        self.assertIsNot(a.tracker, b.tracker)
        self.assertNotEqual(a.epoch, b.epoch)
        self.assertEqual(FakeTracker.created, 2)

    def test_pts_going_backwards_resets_tracker(self):
        entry, _ = self.registry.acquire("cam:s1")
        tracker, epoch = entry.tracker, entry.epoch
        self.registry.commit(entry, 7_500_000_000)
        self.assertEqual(self.registry.advance(entry, 0), "reset:pts_backwards")
        self.assertEqual(tracker.resets, 1)
        self.assertNotEqual(entry.epoch, epoch)
        self.registry.commit(entry, 7_500_000_000)
        self.assertEqual(self.registry.advance(entry, 7_500_000_000), "reset:pts_backwards")
        self.assertEqual(self.registry.advance(entry, 8_000_000_000), "")

    def test_forward_pts_gap_beyond_lost_window_resets_tracker(self):
        entry, _ = self.registry.acquire("cam:s1")
        epoch = entry.epoch
        self.registry.commit(entry, 500_000_000)
        self.assertEqual(self.registry.advance(entry, 500_000_000 + FakeTracker.LOST_WINDOW_NS), "")
        self.registry.commit(entry, 2_000_000_000)
        self.assertEqual(self.registry.advance(entry, 92_000_000_000), "reset:pts_gap")
        self.assertEqual(entry.tracker.resets, 1)
        self.assertNotEqual(entry.epoch, epoch)

    def test_gap_the_rules_bridge_keeps_the_tracker(self):
        entry, _ = self.registry.acquire("cam:s1")
        epoch = entry.epoch
        self.registry.commit(entry, 500_000_000)
        self.assertEqual(self.registry.advance(entry, 3_000_000_000, max_gap_ns=5_000_000_000), "")
        self.registry.commit(entry, 3_000_000_000)
        self.assertEqual(self.registry.advance(entry, 8_000_000_001, max_gap_ns=5_000_000_000), "reset:pts_gap")
        self.assertEqual(entry.tracker.resets, 1)
        self.assertNotEqual(entry.epoch, epoch)

    def test_idle_tracker_is_evicted_after_120s(self):
        entry, _ = self.registry.acquire("cam:s1")
        self.registry.commit(entry, 500_000_000)
        self.clock.now += 119.0
        self.assertEqual(self.registry.evict_idle(), [])
        self.clock.now += 2.0
        self.assertEqual(self.registry.evict_idle(), ["cam:s1"])
        fresh, note = self.registry.acquire("cam:s1")
        self.assertIsNot(entry.tracker, fresh.tracker)
        self.assertNotEqual(entry.epoch, fresh.epoch)
        self.assertEqual(note, "created")
        self.assertEqual(fresh.last_pts_ns, -1)

    def test_epochs_differ_between_registries(self):
        other = TrackerRegistry(FakeTracker, idle_s=120.0, clock=self.clock)
        a, _ = self.registry.acquire("cam:s1")
        b, _ = other.acquire("cam:s1")
        self.assertNotEqual(a.epoch, b.epoch)

    def test_acquire_evicts_other_idle_sessions(self):
        self.registry.acquire("cam:old")
        self.clock.now += 200.0
        self.registry.acquire("cam:new")
        self.assertEqual(sorted(self.registry.entries), ["cam:new"])

    def test_rate_from_gaps_between_single_frames_is_passed_to_tracker_without_reset(self):
        notes = [self._feed("cam:s1", i * 250_000_000) for i in range(5)]
        entry = self.registry.entries["cam:s1"]
        self.assertEqual(notes, ["", "", "", "fps:2->4", ""])
        self.assertEqual(entry.fps, 4.0)
        self.assertEqual(entry.tracker.fps, 4.0)
        self.assertEqual(entry.tracker.resets, 0)

    def test_rate_needs_min_samples_and_uses_the_median_of_recent_gaps(self):
        entry, _ = self.registry.acquire("cam:s1")
        self.assertIsNone(entry.sampled_fps(0))
        pts = 0
        for gap_ms in (500, 10_000):
            self.registry.commit(entry, pts)
            pts += gap_ms * 1_000_000
        self.assertIsNone(entry.sampled_fps(pts))
        self.registry.commit(entry, pts)
        self.assertAlmostEqual(entry.sampled_fps(pts + 500_000_000), 2.0)
        for _ in range(RATE_WINDOW):
            pts += 250_000_000
            self.registry.commit(entry, pts)
        self.assertEqual(len(entry.gaps_ns), RATE_WINDOW)
        self.assertAlmostEqual(entry.sampled_fps(pts + 250_000_000), 4.0)

    def test_small_jitter_does_not_retune_the_tracker(self):
        pts = 0
        notes = []
        for i in range(12):
            notes.append(self._feed("cam:s1", pts))
            pts += (500 + (10 if i % 2 else -10)) * 1_000_000
        self.assertEqual([n for n in notes if n], [])
        self.assertEqual(self.registry.entries["cam:s1"].fps, DEFAULT_FPS)

    def test_backwards_step_is_not_a_rate_sample(self):
        for i in range(4):
            self._feed("cam:s1", i * 500_000_000)
        entry = self.registry.entries["cam:s1"]
        gaps = list(entry.gaps_ns)
        self.assertEqual(self._feed("cam:s1", 0), "reset:pts_backwards")
        self.assertEqual(list(entry.gaps_ns), gaps)


class SingleFrameSequenceTest(unittest.TestCase):
    """The core sends one frame per detect job; continuity must match one job carrying the same frames."""

    def setUp(self):
        FakeTracker.created = 0
        self.clock = FakeClock()

    def _detector(self, factory=FakeTracker, outputs=None):
        registry = TrackerRegistry(factory, idle_s=120.0, clock=self.clock)
        return FakeDetector(registry, outputs if outputs is not None else {}), registry

    @staticmethod
    def _one_person(n: int) -> dict:
        return {f"/img/{i}.jpg": [(100 + 5 * i, 50, 160 + 5 * i, 250, 0.9, "person")] for i in range(n)}

    def _single_frame_jobs(self, detector, frames, session="s1"):
        results = []
        for frame in frames:
            job = _detect_job([frame], session=session, job_id=f"j-{frame.frame_id}", limits={"max_frames": 1})
            result = detector.run(job)
            self.assertEqual(result.status, "ok")
            self.assertEqual(validate_result_against_job(result, job), [])
            results.append(result)
            self.clock.now += 0.5
        return results

    def test_single_frame_jobs_keep_one_tracker_epoch_and_timeline(self):
        detector, registry = self._detector(outputs=self._one_person(10))
        results = self._single_frame_jobs(detector, _frames(10))
        entry = registry.entries["cam:s1"]
        self.assertEqual(FakeTracker.created, 1)
        self.assertEqual(entry.resets, 0)
        self.assertEqual(len(entry.tracker.updates), 10)
        self.assertEqual(entry.last_pts_ns, 4_500_000_000)
        self.assertEqual(results[0].notes, ["tracker cam:s1: created"])
        self.assertEqual([r.notes for r in results[1:]], [[]] * 9)
        epochs = {d.track_id.rsplit("-", 1)[0] for r in results for d in r.frames[0].detections}
        self.assertEqual(epochs, {entry.epoch})

    def test_single_frame_jobs_match_one_multi_frame_job(self):
        outputs = self._one_person(8)
        single, single_registry = self._detector(outputs=outputs)
        multi, multi_registry = self._detector(outputs=outputs)
        frames = _frames(8, fps=5.0)
        singles = self._single_frame_jobs(single, frames)
        whole = multi.run(_detect_job(frames))

        def raw_ids(frames_out):
            return [d.track_id.rsplit("-", 1)[1] for f in frames_out for d in f.detections]

        self.assertEqual(raw_ids([r.frames[0] for r in singles]), raw_ids(whole.frames))
        single_entry, multi_entry = single_registry.entries["cam:s1"], multi_registry.entries["cam:s1"]
        self.assertEqual((single_entry.fps, single_entry.resets), (multi_entry.fps, multi_entry.resets))
        self.assertAlmostEqual(single_entry.fps, 5.0)
        self.assertEqual(list(single_entry.gaps_ns), list(multi_entry.gaps_ns))
        self.assertIn("tracker cam:s1: fps:2->5 at f0003", singles[3].notes)
        self.assertIn("tracker cam:s1: fps:2->5 at f0003", whole.notes)

    def test_slow_feed_learns_its_rate_and_stops_resetting(self):
        detector, registry = self._detector(factory=RateTracker, outputs=self._one_person(10))
        results = self._single_frame_jobs(detector, _frames(10, fps=0.5))
        entry = registry.entries["cam:s1"]
        self.assertEqual(results[1].notes, ["tracker cam:s1: reset:pts_gap at f0001"])
        self.assertEqual(results[2].notes, ["tracker cam:s1: reset:pts_gap at f0002"])
        self.assertEqual(results[3].notes, ["tracker cam:s1: fps:2->0.5 at f0003"])
        self.assertEqual([r.notes for r in results[4:]], [[]] * 6)
        self.assertEqual(entry.resets, 2)
        self.assertAlmostEqual(entry.tracker.fps, 0.5)
        later = {d.track_id.rsplit("-", 1)[0] for r in results[2:] for d in r.frames[0].detections}
        self.assertEqual(later, {entry.epoch})

    def test_outage_between_single_frame_jobs_resets_without_changing_the_rate(self):
        detector, registry = self._detector(factory=RateTracker, outputs=self._one_person(12))
        frames = _frames(6) + _frames(6, start_ns=32_500_000_000)
        for i, frame in enumerate(frames):
            frame.frame_id, frame.path = f"f{i:04d}", f"/img/{i}.jpg"
        results = self._single_frame_jobs(detector, frames)
        entry = registry.entries["cam:s1"]
        self.assertEqual(results[6].notes, ["tracker cam:s1: reset:pts_gap at f0006"])
        self.assertEqual([n for r in results[7:] for n in r.notes], [])
        self.assertEqual(entry.resets, 1)
        self.assertEqual(entry.fps, DEFAULT_FPS)
        before = {d.track_id for r in results[:6] for d in r.frames[0].detections}
        after = {d.track_id for r in results[6:] for d in r.frames[0].detections}
        self.assertFalse(before & after)

    def test_replayed_single_frame_resets_and_later_frames_continue(self):
        detector, registry = self._detector(outputs=self._one_person(6))
        frames = _frames(6)
        self._single_frame_jobs(detector, frames[:4])
        replay = self._single_frame_jobs(detector, frames[1:2])[0]
        self.assertEqual(replay.notes, ["tracker cam:s1: reset:pts_backwards at f0001"])
        resumed = self._single_frame_jobs(detector, frames[2:3])[0]
        self.assertEqual(resumed.notes, [])
        self.assertEqual(registry.entries["cam:s1"].resets, 1)

    def test_idle_camera_session_is_forgotten_between_single_frame_jobs(self):
        detector, registry = self._detector(outputs=self._one_person(4))
        first = self._single_frame_jobs(detector, _frames(2))
        self.clock.now += 121.0
        later = self._single_frame_jobs(detector, _frames(4)[2:])
        self.assertEqual(later[0].notes, ["tracker cam:s1: created"])
        self.assertFalse(set(_track_ids(first[0])) & set(_track_ids(later[0])))


class GeometryTest(unittest.TestCase):
    def test_sort_frames_orders_by_pts(self):
        frames = _frames(3)[::-1]
        self.assertEqual([f.frame_id for f in sort_frames(frames)], ["f0000", "f0001", "f0002"])

    def test_normalizes_boxes_and_anchors(self):
        frame = FrameRef("f0", 0, 0, 0, "/img/0.jpg", 0)
        raw = RawDetection(64.0, 36.0, 320.0, 324.0, 0.75, "car")
        out = build_detection_frame(frame, 640, 360, [raw], ["3"])
        det = out.detections[0]
        self.assertEqual(det.track_id, "3")
        self.assertEqual(det.bbox, [0.1, 0.1, 0.5, 0.9])
        self.assertEqual(det.anchor_foot, [0.3, 0.9])
        self.assertEqual(det.anchor_center, [0.3, 0.5])
        self.assertEqual((out.width, out.height), (640, 360))

    def test_clamps_boxes_outside_frame(self):
        frame = FrameRef("f0", 0, 0, 0, "/img/0.jpg", 0)
        raw = RawDetection(-10.0, -5.0, 700.0, 400.0, 1.2, "person")
        det = build_detection_frame(frame, 640, 360, [raw], [None]).detections[0]
        self.assertEqual(det.bbox, [0.0, 0.0, 1.0, 1.0])
        self.assertEqual(det.confidence, 1.0)
        self.assertIsNone(det.track_id)


class FakeMonoClock:
    def __init__(self) -> None:
        self.now_ns = 10_000_000_000

    def __call__(self) -> int:
        return self.now_ns


class FakeDetector(RfDetrTracker):
    """RfDetrTracker with the model and image loader replaced; nothing from torch is touched."""

    def __init__(self, registry, outputs, clock=None):
        super().__init__(variant="nano", device="fake", registry=registry, clock=clock or FakeMonoClock())
        self.outputs = outputs
        self.predicted: list[str] = []
        self.predict_ns = 0

    def load(self) -> None:
        self.device = "fake"
        self.model = object()
        self.class_names = {0: "person", 1: "bicycle", 2: "car", 3: "motorcycle", 5: "bus", 7: "truck", 16: "dog"}

    def _load_image(self, path):
        return path, 640, 360

    def _predict(self, image, threshold):
        self.predicted.append(image)
        self.clock.now_ns += self.predict_ns
        return [RawDetection(*d) for d in self.outputs.get(image, [])]


def _track_ids(result) -> list[str]:
    return [d.track_id for f in result.frames for d in f.detections]


def _detect_job(frames: list[FrameRef], camera="cam", session="s1", job_id="j1", **extra) -> Job:
    d = {"job_id": job_id, "kind": "detect_frames", "camera_id": camera, "session_id": session, "generation": 2,
         "frames": [vars(f) for f in frames], "clip": None, "gaps": [], "limits": {}}
    d.update(extra)
    return Job.from_dict(d)


class FakeDetectorRunTest(unittest.TestCase):
    def setUp(self):
        FakeTracker.created = 0
        self.clock = FakeClock()
        self.registry = TrackerRegistry(FakeTracker, idle_s=120.0, clock=self.clock)
        self.outputs = {
            "/img/0.jpg": [(100, 50, 300, 250, 0.9, "car"), (10, 10, 20, 30, 0.4, "dog")],
            "/img/1.jpg": [(110, 50, 310, 250, 0.85, "car"), (400, 100, 420, 180, 0.6, "person")],
            "/img/2.jpg": [],
        }
        self.detector = FakeDetector(self.registry, self.outputs)

    def test_run_emits_one_frame_per_input_and_filters_classes(self):
        job = _detect_job(_frames(3))
        result = self.detector.run(job)
        self.assertEqual(result.status, "ok")
        self.assertEqual(validate_result_against_job(result, job), [])
        self.assertEqual([f.frame_id for f in result.frames], ["f0000", "f0001", "f0002"])
        self.assertEqual([d.cls for d in result.frames[0].detections], ["car"])
        self.assertEqual(sorted(d.cls for d in result.frames[1].detections), ["car", "person"])
        self.assertEqual(result.frames[2].detections, [])
        self.assertEqual(len(result.per_frame_ms), 3)
        self.assertEqual(result.device, "fake")
        self.assertEqual(result.model_version, "rf-detr-nano")
        self.assertIn("tracker cam:s1: created", result.notes)

    def test_frames_are_fed_in_pts_order(self):
        job = _detect_job(_frames(3)[::-1])
        result = self.detector.run(job)
        self.assertEqual(self.detector.predicted, ["/img/0.jpg", "/img/1.jpg", "/img/2.jpg"])
        self.assertEqual([f.pts_ns for f in result.frames], [0, 500_000_000, 1_000_000_000])

    def test_target_classes_override(self):
        job = _detect_job(_frames(2), target_classes=["person"])
        result = self.detector.run(job)
        self.assertEqual(result.frames[0].detections, [])
        self.assertEqual([d.cls for d in result.frames[1].detections], ["person"])

    def test_job_threshold_and_max_gap_reach_the_tracker(self):
        self.detector.run(_detect_job(_frames(1), threshold=0.2))
        tracker = self.registry.entries["cam:s1"].tracker
        self.assertEqual(tracker.thresholds, [0.2])
        later = self.detector.run(_detect_job(_frames(1, start_ns=4_000_000_000), job_id="j2", max_gap_ns=5_000_000_000))
        self.assertEqual(tracker.resets, 0)
        self.assertFalse(any("reset" in n for n in later.notes))
        self.assertEqual(tracker.thresholds, [0.2, 0.3])

    def test_tracker_only_sees_target_classes(self):
        self.detector.run(_detect_job(_frames(1)))
        tracker = self.registry.entries["cam:s1"].tracker
        self.assertEqual([d.cls for d in tracker.updates[0]], ["car"])

    def test_sessions_are_keyed_and_replay_resets(self):
        first = self.detector.run(_detect_job(_frames(3), session="s1"))
        self.detector.run(_detect_job(_frames(3, start_ns=1_500_000_000), session="s1", job_id="j2"))
        self.assertEqual(self.registry.entries["cam:s1"].tracker.resets, 0)
        replay = self.detector.run(_detect_job(_frames(3), session="s1", job_id="j3"))
        self.assertEqual(self.registry.entries["cam:s1"].tracker.resets, 1)
        self.assertIn("tracker cam:s1: reset:pts_backwards at f0000", replay.notes)
        self.assertNotEqual(_track_ids(first), _track_ids(replay))
        self.assertFalse(set(_track_ids(first)) & set(_track_ids(replay)))
        self.detector.run(_detect_job(_frames(3), session="s2", job_id="j4"))
        self.assertEqual(sorted(self.registry.entries), ["cam:s1", "cam:s2"])
        self.assertEqual(FakeTracker.created, 2)

    def test_idle_session_tracker_is_dropped(self):
        self.detector.run(_detect_job(_frames(3), session="s1"))
        self.clock.now += 121.0
        self.detector.run(_detect_job(_frames(3), session="s2", job_id="j2"))
        self.assertEqual(sorted(self.registry.entries), ["cam:s2"])

    def test_missing_image_is_reported_not_fabricated(self):
        def failing(path):
            if path.endswith("1.jpg"):
                raise OSError("no such file")
            return path, 640, 360
        self.detector._load_image = failing
        job = _detect_job(_frames(3))
        result = self.detector.run(job)
        self.assertEqual(result.status, "partial")
        self.assertIn("f0001: no such file", result.error)
        self.assertEqual([f.frame_id for f in result.frames], ["f0000", "f0002"])
        self.assertEqual(validate_result_against_job(result, job), [])

    def test_track_ids_are_tracker_ids_qualified_by_epoch(self):
        result = self.detector.run(_detect_job(_frames(2)))
        epoch = self.registry.entries["cam:s1"].epoch
        self.assertEqual(result.frames[0].detections[0].track_id, f"{epoch}-1")
        self.assertEqual({d.track_id for d in result.frames[1].detections}, {f"{epoch}-2", f"{epoch}-3"})

    def test_track_ids_differ_after_eviction_with_pts_still_moving_forward(self):
        before = self.detector.run(_detect_job(_frames(2)))
        self.clock.now += 121.0
        after = self.detector.run(_detect_job(_frames(2, start_ns=200_000_000_000), job_id="j2"))
        self.assertIn("tracker cam:s1: created", after.notes)
        self.assertFalse(set(_track_ids(before)) & set(_track_ids(after)))

    def test_forward_pts_gap_resets_tracks_between_and_within_jobs(self):
        before = self.detector.run(_detect_job(_frames(2)))
        after = self.detector.run(_detect_job(_frames(2, start_ns=90_000_000_000), job_id="j2"))
        self.assertIn("tracker cam:s1: reset:pts_gap at f0000", after.notes)
        self.assertFalse(set(_track_ids(before)) & set(_track_ids(after)))
        frames = [_frames(1)[0], FrameRef("f0001", 60_000_000_000, 0, 0, "/img/1.jpg", 1)]
        within = self.detector.run(_detect_job(frames, session="s2", job_id="j3"))
        self.assertIn("tracker cam:s2: reset:pts_gap at f0001", within.notes)
        first, second = (d.track_id.rsplit("-", 1)[0] for d in (within.frames[0].detections[0],
                                                                within.frames[1].detections[0]))
        self.assertNotEqual(first, second)

    def test_deadline_stops_feeding_frames_and_reports_partial(self):
        self.detector.predict_ns = 600_000_000
        job = _detect_job(_frames(3), limits={"deadline_ms": 1000})
        result = self.detector.run(job)
        self.assertEqual(result.status, "partial")
        self.assertEqual([f.frame_id for f in result.frames], ["f0000", "f0001"])
        self.assertIn("deadline 1000 ms exceeded after 2 of 3 frames", result.error)
        self.assertEqual(self.registry.entries["cam:s1"].last_pts_ns, 500_000_000)
        self.assertEqual(validate_result_against_job(result, job), [])

    def test_deadline_counts_from_worker_acceptance(self):
        job = _detect_job(_frames(2), limits={"deadline_ms": 1000})
        job.accepted_mono_ns = self.detector.clock.now_ns - 2_000_000_000
        result = self.detector.run(job)
        self.assertEqual(result.status, "error")
        self.assertEqual(result.frames, [])
        self.assertIn("exceeded after 0 of 2 frames", result.error)

    def test_unknown_target_class_is_an_error_not_an_empty_scene(self):
        job = _detect_job(_frames(2), target_classes=["people", "car"])
        result = self.detector.run(job)
        self.assertEqual(result.status, "error")
        self.assertIn("unknown target classes ['people']", result.error)
        self.assertEqual(result.frames, [])
        self.assertEqual(self.detector.predicted, [])
        self.assertEqual(self.registry.entries, {})

    def test_mps_failure_falls_back_to_cpu_and_keeps_the_frame(self):
        detector = self.detector
        detector.load()
        detector.device = "mps"
        calls = []
        real_predict = detector._predict

        def flaky(image, threshold):
            calls.append(detector.device)
            if detector.device == "mps":
                raise NotImplementedError("op not implemented for mps")
            return real_predict(image, threshold)

        def rebuild(device):
            detector.device = device

        detector._predict = flaky
        detector._build = rebuild
        result = detector.run(_detect_job(_frames(2)))
        self.assertEqual(result.status, "ok")
        self.assertEqual(result.device, "cpu")
        self.assertEqual(calls, ["mps", "cpu", "cpu"])
        self.assertEqual([d.cls for d in result.frames[0].detections], ["car"])
        self.assertTrue(any(n.startswith("mps inference failed, fell back to cpu: NotImplementedError") for n in result.notes))

    def test_cpu_inference_failure_propagates(self):
        def broken(image, threshold):
            raise RuntimeError("out of memory")

        self.detector._predict = broken
        with self.assertRaises(RuntimeError):
            self.detector.run(_detect_job(_frames(1)))


if __name__ == "__main__":
    unittest.main()
