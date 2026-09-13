"""Track-to-detection mapping of ByteTrackAdapter with a stand-in tracker. Needs numpy only."""
import importlib.util
import unittest
from unittest import mock

from fovea_worker.tracking import RawDetection

HAVE_NUMPY = importlib.util.find_spec("numpy") is not None
HAVE_SUPERVISION = HAVE_NUMPY and importlib.util.find_spec("supervision") is not None


class StubTrack:
    def __init__(self, track_id: int, tlbr, score: float) -> None:
        self.external_track_id = track_id
        self.tlbr = tlbr
        self.score = score


class StubByteTrack:
    def __init__(self, outputs) -> None:
        self.outputs = outputs
        self.calls = []
        self.max_time_lost = 0
        self.resets = 0
        self.track_activation_threshold = 0.25
        self.det_thresh = 0.35

    def update_with_tensors(self, tensors):
        self.calls.append(tensors.copy())
        return self.outputs.pop(0)

    def reset(self) -> None:
        self.resets += 1


@unittest.skipUnless(HAVE_NUMPY, "numpy not importable")
class ByteTrackAdapterTest(unittest.TestCase):
    def _adapter(self, outputs):
        from fovea_worker.backends.detector_rfdetr import ByteTrackAdapter

        adapter = ByteTrackAdapter.__new__(ByteTrackAdapter)
        adapter.tracker = StubByteTrack(outputs)
        return adapter

    def test_score_equality_maps_track_to_its_detection_despite_smeared_box(self):
        import numpy as np

        det = RawDetection(225, 77, 370, 308, float(np.float32(0.81)), "car")
        smeared = np.array([164, 98, 440, 325], dtype=np.float32)
        adapter = self._adapter([[StubTrack(1, smeared, np.float32(0.81))]])
        self.assertEqual(adapter.update([det]), ["1"])
        self.assertEqual(adapter.tracker.calls[0].shape, (1, 5))

    def test_equal_scores_are_broken_by_iou(self):
        import numpy as np

        a = RawDetection(0, 0, 100, 100, float(np.float32(0.5)), "car")
        b = RawDetection(500, 500, 600, 600, float(np.float32(0.5)), "car")
        tracks = [StubTrack(7, np.array([505, 505, 605, 605], dtype=np.float32), np.float32(0.5)),
                  StubTrack(3, np.array([2, 2, 98, 98], dtype=np.float32), np.float32(0.5))]
        adapter = self._adapter([tracks])
        self.assertEqual(adapter.update([a, b]), ["3", "7"])

    def test_iou_fallback_is_gated_at_matching_threshold(self):
        import numpy as np

        near = RawDetection(0, 0, 100, 100, 0.9, "person")
        far = RawDetection(300, 300, 400, 400, 0.9, "person")
        tracks = [StubTrack(2, np.array([10, 10, 110, 110], dtype=np.float32), np.float32(0.4)),
                  StubTrack(5, np.array([700, 700, 800, 800], dtype=np.float32), np.float32(0.4))]
        adapter = self._adapter([tracks])
        self.assertEqual(adapter.update([near, far]), ["2", None])

    def test_boxes_enter_the_tracker_padded_and_map_back_to_their_rows(self):
        import numpy as np

        from fovea_worker.backends import detector_rfdetr

        det = RawDetection(100, 100, 200, 300, 0.9, "person")
        padded = np.array([70, 40, 230, 360], dtype=np.float32)
        adapter = self._adapter([[StubTrack(4, padded, np.float32(0.5))]])
        with mock.patch.object(detector_rfdetr, "BOX_BUFFER", 0.3):
            self.assertEqual(adapter.update([det]), ["4"])
        np.testing.assert_allclose(adapter.tracker.calls[0][0], [70, 40, 230, 360, 0.9], rtol=1e-6)

    def test_empty_frame_still_advances_tracker(self):
        adapter = self._adapter([[]])
        self.assertEqual(adapter.update([]), [])
        self.assertEqual(adapter.tracker.calls[0].shape, (0, 5))

    def test_set_fps_scales_lost_buffer(self):
        from fovea_worker.backends.detector_rfdetr import LOST_TRACK_BUFFER

        adapter = self._adapter([])
        adapter.set_fps(2.0)
        self.assertEqual(adapter.tracker.max_time_lost, round(2.0 / 30.0 * LOST_TRACK_BUFFER))

    def test_rate_measured_from_jittered_pts_keeps_the_nominal_lost_buffer(self):
        from fovea_worker.backends.detector_rfdetr import LOST_TRACK_BUFFER

        adapter = self._adapter([])
        nominal = round(2.0 / 30.0 * LOST_TRACK_BUFFER)
        for measured in (1.873, 1.998, 2.141):
            adapter.set_fps(measured)
            self.assertEqual(adapter.tracker.max_time_lost, nominal, measured)

    def test_set_threshold_lets_every_reported_detection_start_a_track(self):
        adapter = self._adapter([])
        adapter.set_threshold(0.3)
        self.assertEqual((adapter.tracker.track_activation_threshold, adapter.tracker.det_thresh), (0.25, 0.3))
        adapter.set_threshold(0.12)
        self.assertEqual((adapter.tracker.track_activation_threshold, adapter.tracker.det_thresh), (0.12, 0.12))

    def test_lost_window_spans_one_frame_past_max_time_lost(self):
        adapter = self._adapter([])
        adapter.set_fps(2.0)
        adapter.tracker.max_time_lost = 2
        self.assertEqual(adapter.lost_window_ns(), 1_500_000_000)
        adapter.set_fps(5.0)
        adapter.tracker.max_time_lost = 5
        self.assertEqual(adapter.lost_window_ns(), 1_200_000_000)


@unittest.skipUnless(HAVE_SUPERVISION, "supervision not importable")
class ByteTrackThresholdTest(unittest.TestCase):
    def test_detection_just_above_the_job_threshold_gets_a_track_id(self):
        from fovea_worker.backends.detector_rfdetr import ByteTrackAdapter

        steady = RawDetection(100, 100, 200, 400, 0.9, "person")
        faint = RawDetection(400, 100, 500, 400, 0.32, "person")
        fresh = ByteTrackAdapter(2.0)
        fresh.set_threshold(0.3)
        self.assertIsNotNone(fresh.update([faint])[0])
        running = ByteTrackAdapter(2.0)
        running.set_threshold(0.3)
        running.update([steady])
        ids = [running.update([steady, faint]) for _ in range(3)]
        self.assertIsNotNone(ids[-1][1])


if __name__ == "__main__":
    unittest.main()
