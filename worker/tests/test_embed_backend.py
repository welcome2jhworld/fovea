"""EmbedBackend batching, packing and load bookkeeping with fake embedders. Needs numpy, not torch."""
import importlib.util
import tempfile
import time
import unittest
from pathlib import Path

from embed_fakes import embed_frames_dict, embed_text_dict
from fovea_worker.embedding import (INDEX_VERSIONS, ModelUnavailable, build_descriptor, decode_vectors,
                                    descriptor_hash, validate_embed_result)
from fovea_worker.protocol import Job

HAVE_NUMPY = importlib.util.find_spec("numpy") is not None


class FakeEmbedder:
    dry_run = False
    lane_batch_size = 2
    loads = 0

    def __init__(self, name, scale=1.0, shape_error=False):
        self.spec = INDEX_VERSIONS[name]
        self.device = "fake"
        self.scale = scale
        self.shape_error = shape_error
        self.calls: list[list[str]] = []

    def load(self):
        FakeEmbedder.loads += 1

    def _rows(self, items):
        import numpy as np

        self.calls.append(list(items))
        rows = np.zeros((len(items) + (1 if self.shape_error else 0), self.spec.dims), dtype=np.float32)
        for i, item in enumerate(items):
            rows[i, sum(map(ord, item)) % self.spec.dims] = self.scale
        return rows

    def embed_images(self, paths):
        return self._rows(paths)

    def embed_text(self, texts):
        return self._rows(texts)

    def descriptor(self, sample_interval_ms):
        return build_descriptor(self.spec.name, self.spec.model_id, "f" * 40, {"fake": True}, self.spec.dims,
                                "float32", sample_interval_ms, "{text}")


@unittest.skipUnless(HAVE_NUMPY, "numpy not importable")
class EmbedBackendTest(unittest.TestCase):
    def setUp(self):
        from fovea_worker.backends.embed import EmbedBackend

        FakeEmbedder.loads = 0
        self.created: dict[str, FakeEmbedder] = {}

        def factory(name):
            self.created[name] = FakeEmbedder(name)
            return self.created[name]

        self.backend = EmbedBackend(factory)

    def test_frames_are_batched_packed_in_order_and_valid(self):
        job = Job.from_dict(embed_frames_dict(5, sample_interval_ms=2000))
        yields: list[int] = []
        result = self.backend.run(job, between_batches=lambda: yields.append(1))
        self.assertEqual(validate_embed_result(result, job), [])
        embedder = self.created["siglip2-b16-224"]
        self.assertEqual([len(c) for c in embedder.calls], [2, 2, 1])
        self.assertEqual(len(yields), 2)
        self.assertEqual(result.frame_ids, ["f0", "f1", "f2", "f3", "f4"])
        self.assertEqual(result.descriptor["sample_interval_ms"], 2000)
        self.assertEqual(result.index_version, descriptor_hash(result.descriptor))
        self.assertEqual(result.model_version, "google/siglip2-base-patch16-224@" + "f" * 40)
        rows = decode_vectors(result.vectors, 768)
        for path, row in zip([f.path for f in job.frames], rows):
            self.assertEqual(row.index(1.0), sum(map(ord, path)) % 768)

    def test_models_load_once_per_index_version_and_report_state(self):
        health = self.backend.versions_health()
        self.assertEqual({v["state"] for v in health.values()}, {"unloaded"})
        first = self.backend.run(Job.from_dict(embed_text_dict()))
        self.backend.run(Job.from_dict(embed_frames_dict()))
        qwen = self.backend.run(Job.from_dict(embed_text_dict(index_version_name="qwen3vl-emb-2b-1024")))
        self.assertEqual(FakeEmbedder.loads, 2)
        self.assertEqual((qwen.dims, qwen.count, qwen.status), (1024, 1, "ok"))
        self.assertNotEqual(first.index_version, qwen.index_version)
        health = self.backend.versions_health()
        self.assertEqual({v["state"] for v in health.values()}, {"ready"})
        self.assertEqual(health["qwen3vl-emb-2b-1024"]["device"], "fake")

    def test_invalid_vectors_become_error_results(self):
        self.backend._factory = lambda name: FakeEmbedder(name, scale=2.0)
        job = Job.from_dict(embed_text_dict())
        result = self.backend.run(job)
        self.assertEqual((result.status, result.vectors), ("error", ""))
        self.assertIn("norms", result.error)
        self.assertEqual(validate_embed_result(result, job), [])
        backend = type(self.backend)(lambda name: FakeEmbedder(name, shape_error=True))
        result = backend.run(Job.from_dict(embed_frames_dict()))
        self.assertIn("shape", result.error)

    def test_load_failure_raises_and_is_reported(self):
        from fovea_worker.backends.embed import EmbedBackend

        def factory(name):
            raise OSError("no weights at rtsp://user:secret@cam/1")

        backend = EmbedBackend(factory)
        with self.assertRaises(ModelUnavailable) as caught:
            backend.run(Job.from_dict(embed_text_dict()))
        self.assertNotIn("secret", str(caught.exception))
        status = backend.versions_health()["siglip2-b16-224"]
        self.assertEqual(status["state"], "failed")
        self.assertIn("no weights", status["load_error"])

    def test_disabled_and_unknown_versions(self):
        from fovea_worker.backends.embed import EmbedBackend

        backend = EmbedBackend(FakeEmbedder, names=("siglip2-b16-224",))
        with self.assertRaisesRegex(ModelUnavailable, "not enabled"):
            backend.run(Job.from_dict(embed_text_dict(index_version_name="qwen3vl-emb-2b-1024")))
        with self.assertRaises(ValueError):
            EmbedBackend(FakeEmbedder, names=("siglip2-b16-224",), preload=("qwen3vl-emb-2b-1024",))
        with self.assertRaises(ValueError):
            EmbedBackend(FakeEmbedder, names=("clip",))


@unittest.skipUnless(HAVE_NUMPY, "numpy not importable")
class ModelLoadLockTest(unittest.TestCase):
    """Two model loads at once broke the torch and transformers imports, so they are serialised."""

    def test_loads_never_overlap(self):
        import threading

        from fovea_worker.backends.embed import EmbedBackend
        from fovea_worker.modelload import MODEL_LOAD_LOCK

        overlaps = []
        loading = threading.Event()
        inside = threading.Lock()
        busy = False

        class SlowEmbedder(FakeEmbedder):
            def load(self):
                nonlocal busy
                with inside:
                    overlaps.append(busy)
                    busy = True
                loading.set()
                time.sleep(0.2)
                with inside:
                    busy = False

        backend = EmbedBackend(lambda name: SlowEmbedder(name))
        errors: list[Exception] = []

        def run(name):
            try:
                backend.run(Job.from_dict(embed_text_dict(index_version_name=name)))
            except Exception as e:  # noqa: BLE001 - reported through errors
                errors.append(e)

        first = threading.Thread(target=run, args=("siglip2-b16-224",))
        first.start()
        self.assertTrue(loading.wait(5))
        # The detector warm-up takes the same lock, so a second load waits here.
        with MODEL_LOAD_LOCK:
            self.assertFalse(busy, "a model was still loading while the lock was held")
        second = threading.Thread(target=run, args=("qwen3vl-emb-2b-1024",))
        second.start()
        first.join(10)
        second.join(10)
        self.assertEqual(errors, [])
        self.assertEqual(overlaps, [False, False])


class DryEmbedderTest(unittest.TestCase):
    def test_vectors_are_deterministic_unit_vectors_marked_dry_run(self):
        from fovea_worker.backends.embed import DryEmbedder, EmbedBackend

        backend = EmbedBackend(DryEmbedder)
        with tempfile.TemporaryDirectory() as tmp:
            frames = embed_frames_dict(3)["frames"]
            for i, frame in enumerate(frames):
                frame["path"] = str(Path(tmp) / f"{i}.jpg")
                Path(frame["path"]).write_bytes(b"same" if i < 2 else b"other")
            job = Job.from_dict(embed_frames_dict(frames=frames, index_version_name="qwen3vl-emb-2b-1024"))
            result = backend.run(job)
        self.assertEqual(validate_embed_result(result, job), [])
        self.assertEqual((result.status, result.dims, result.descriptor["model_revision"]), ("dry_run", 1024, "dry"))
        rows = decode_vectors(result.vectors, 1024)
        self.assertEqual(rows[0], rows[1])
        self.assertNotEqual(rows[0], rows[2])
        text_job = Job.from_dict(embed_text_dict(["a white car"] * 2, index_version_name="qwen3vl-emb-2b-1024"))
        text = backend.run(text_job)
        self.assertEqual(text.index_version, result.index_version)
        text_rows = decode_vectors(text.vectors, 1024)
        self.assertEqual(text_rows[0], text_rows[1])


if __name__ == "__main__":
    unittest.main()
