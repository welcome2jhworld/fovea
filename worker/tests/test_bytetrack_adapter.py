"""Track-to-detection mapping of ByteTrackAdapter with a stand-in tracker. Needs numpy only."""
import importlib.util
import unittest

from fovea_worker.tracking import RawDetection

HAVE_NUMPY = importlib.util.find_spec("numpy") is not None


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

    def test_empty_frame_still_advances_tracker(self):
        adapter = self._adapter([[]])
        self.assertEqual(adapter.update([]), [])
        self.assertEqual(adapter.tracker.calls[0].shape, (0, 5))

    def test_set_fps_scales_lost_buffer(self):
        from fovea_worker.backends.detector_rfdetr import LOST_TRACK_BUFFER

        adapter = self._adapter([])
        adapter.set_fps(2.0)
        self.assertEqual(adapter.tracker.max_time_lost, int(2.0 / 30.0 * LOST_TRACK_BUFFER))

    def test_lost_window_spans_one_frame_past_max_time_lost(self):
        adapter = self._adapter([])
        adapter.set_fps(2.0)
        adapter.tracker.max_time_lost = 2
        self.assertEqual(adapter.lost_window_ns(), 1_500_000_000)
        adapter.set_fps(5.0)
        adapter.tracker.max_time_lost = 5
        self.assertEqual(adapter.lost_window_ns(), 1_200_000_000)


if __name__ == "__main__":
    unittest.main()
