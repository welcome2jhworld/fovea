"""Runs google/siglip2-base-patch16-224 through the embed lane backend.

Skipped unless torch, transformers and PIL import and the model is in the
Hugging Face cache (nothing is downloaded).
"""
import importlib
import tempfile
import unittest
from pathlib import Path

from embed_fakes import embed_frames_dict, embed_text_dict
from fovea_worker.embedding import ModelUnavailable, decode_vectors, hf_snapshot, validate_embed_result
from fovea_worker.protocol import Job

MODEL_ID = "google/siglip2-base-patch16-224"


def _weights_available() -> bool:
    try:
        for name in ("torch", "transformers", "PIL", "numpy"):
            importlib.import_module(name)
        hf_snapshot(MODEL_ID)
    except (ImportError, ModelUnavailable):
        return False
    return True


@unittest.skipUnless(_weights_available(), f"torch/transformers/PIL or cached {MODEL_ID} weights not available")
class Siglip2IntegrationTest(unittest.TestCase):
    def test_two_frames_and_one_text_share_a_valid_index_version(self):
        from PIL import Image

        from fovea_worker.backends.embed import EmbedBackend, create_embedder

        backend = EmbedBackend(create_embedder, names=("siglip2-b16-224",))
        with tempfile.TemporaryDirectory() as tmp:
            frames = embed_frames_dict(2)["frames"]
            for frame, color in zip(frames, ((200, 30, 30), (30, 30, 200))):
                frame["path"] = str(Path(tmp) / f"{frame['frame_id']}.jpg")
                Image.new("RGB", (960, 540), color).save(frame["path"], quality=90)
            frames_job = Job.from_dict(embed_frames_dict(frames=frames))
            frames_result = backend.run(frames_job)
            again = backend.run(frames_job)
        text_job = Job.from_dict(embed_text_dict(["a red image"]))
        text_result = backend.run(text_job)

        self.assertEqual(frames_result.status, "ok", frames_result.error)
        self.assertEqual(validate_embed_result(frames_result, frames_job), [])
        self.assertEqual(validate_embed_result(text_result, text_job), [])
        self.assertIn(frames_result.device, ("cuda", "mps", "cpu"))
        self.assertGreater(frames_result.load_ms, 0)
        self.assertEqual(again.load_ms, 0)
        self.assertEqual(frames_result.dims, 768)
        self.assertEqual(frames_result.descriptor["model_revision"], hf_snapshot(MODEL_ID)[1])
        self.assertEqual(text_result.index_version, frames_result.index_version)
        red, blue = decode_vectors(frames_result.vectors, 768)
        red_again, _ = decode_vectors(again.vectors, 768)
        (query,) = decode_vectors(text_result.vectors, 768)
        self.assertGreater(_dot(red, red_again), 0.999)
        self.assertLess(_dot(red, blue), 0.99)
        self.assertGreater(_dot(query, red), _dot(query, blue))
        self.assertEqual(len(frames_result.per_frame_ms), 2)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


if __name__ == "__main__":
    unittest.main()
