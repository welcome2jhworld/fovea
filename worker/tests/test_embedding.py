import base64
import math
import struct
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from embed_fakes import REVISION, descriptor, embed_frames_dict, embed_text_dict, ok_result, unit_vectors
from fovea_worker.embedding import (DESCRIPTOR_KEYS, INDEX_VERSIONS, ModelUnavailable, build_descriptor,
                                    canonical_json, decode_vectors, descriptor_hash, encode_vectors, hf_snapshot,
                                    validate_embed_result, vector_problems)
from fovea_worker.protocol import Job

SIGLIP_REVISION = "75de2d55ec2d0b4efc50b3e9ad70dba96a7b2fa2"


def _siglip_descriptor(**overrides):
    fields = dict(name="siglip2-b16-224", model_id="google/siglip2-base-patch16-224", model_revision=SIGLIP_REVISION,
                  preprocessing={"image_size": "224x224", "text_max_length": 64, "lowercase": True}, dims=768,
                  dtype="float32", sample_interval_ms=1000, prompt_template="query: this is a photo of {text}.")
    fields.update(overrides)
    return build_descriptor(**fields)


class DescriptorHashTest(unittest.TestCase):
    def test_hash_is_pinned_and_matches_the_compact_sorted_json(self):
        d = _siglip_descriptor()
        self.assertEqual(canonical_json(d), (
            '{"dims":768,"dtype":"float32","model_id":"google/siglip2-base-patch16-224",'
            '"model_revision":"75de2d55ec2d0b4efc50b3e9ad70dba96a7b2fa2","name":"siglip2-b16-224",'
            '"preprocessing":{"image_size":"224x224","lowercase":true,"text_max_length":64},'
            '"prompt_template":"query: this is a photo of {text}.","sample_interval_ms":1000}'))
        self.assertEqual(descriptor_hash(d), "06e554c62702")

    def test_hash_ignores_key_order_and_changes_with_every_field(self):
        d = _siglip_descriptor()
        reordered = dict(reversed(list(d.items())))
        reordered["preprocessing"] = dict(reversed(list(d["preprocessing"].items())))
        self.assertEqual(descriptor_hash(reordered), descriptor_hash(d))
        changes = {"name": "qwen3vl-emb-2b-1024", "model_id": "google/other", "model_revision": "1" * 40,
                   "preprocessing": {"image_size": "256x256", "text_max_length": 64, "lowercase": True},
                   "dims": 1024, "dtype": "float16", "sample_interval_ms": 2000, "prompt_template": "{text}"}
        self.assertEqual(sorted(changes), sorted(DESCRIPTOR_KEYS))
        hashes = {descriptor_hash(_siglip_descriptor(**{key: value})) for key, value in changes.items()}
        self.assertEqual(len(hashes), len(changes))
        self.assertNotIn(descriptor_hash(d), hashes)

    def test_descriptor_rejects_floats_and_non_ascii(self):
        with self.assertRaises(ValueError):
            _siglip_descriptor(preprocessing={"image_mean": 0.5})
        with self.assertRaises(ValueError):
            _siglip_descriptor(prompt_template="사진 {text}")


class VectorCodecTest(unittest.TestCase):
    def test_round_trip_and_partial_rows(self):
        encoded = encode_vectors(struct.pack("<4f", 0.6, 0.8, 0.0, 0.0) + struct.pack("<4f", 0.0, 0.0, 1.0, 0.0))
        rows = decode_vectors(encoded, 2)
        self.assertEqual(len(rows), 4)
        rows = decode_vectors(encoded, 4)
        self.assertEqual(len(rows), 2)
        self.assertAlmostEqual(rows[0][1], 0.8, places=6)
        self.assertEqual(vector_problems(rows), [])
        with self.assertRaisesRegex(ValueError, "not a multiple"):
            decode_vectors(encoded, 3)
        with self.assertRaises(ValueError):
            decode_vectors("not base64!", 4)

    def test_problems_flag_non_unit_and_non_finite(self):
        problems = vector_problems([(1.0, 1.0), (math.nan, 0.0), (1.0, 0.0)])
        self.assertEqual(len(problems), 2)
        self.assertIn("norm", problems[0])
        self.assertIn("not finite", problems[1])


class HfSnapshotTest(unittest.TestCase):
    def test_reads_the_main_ref_and_requires_the_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp)
            repo = cache / "models--google--siglip2-base-patch16-224"
            with self.assertRaises(ModelUnavailable):
                hf_snapshot("google/siglip2-base-patch16-224", cache)
            (repo / "refs").mkdir(parents=True)
            (repo / "refs" / "main").write_text(REVISION + "\n", encoding="ascii")
            with self.assertRaisesRegex(ModelUnavailable, "config.json"):
                hf_snapshot("google/siglip2-base-patch16-224", cache)
            snapshot = repo / "snapshots" / REVISION
            snapshot.mkdir(parents=True)
            (snapshot / "config.json").write_text("{}", encoding="ascii")
            self.assertEqual(hf_snapshot("google/siglip2-base-patch16-224", cache), (snapshot, REVISION))


class EmbedJobTest(unittest.TestCase):
    def test_defaults(self):
        frames = Job.from_dict(embed_frames_dict())
        self.assertEqual((frames.priority, frames.sample_interval_ms, frames.texts), ("index", 1000, []))
        text = Job.from_dict(embed_text_dict(sample_interval_ms=2000.0))
        self.assertEqual((text.priority, text.sample_interval_ms, text.texts), ("query", 2000, ["a white car"]))
        self.assertEqual(Job.from_dict(embed_text_dict(priority="index")).priority, "index")
        detect = Job.from_dict({"job_id": "d", "kind": "detect_frames", "camera_id": "c", "session_id": "s",
                                "frames": embed_frames_dict()["frames"]})
        self.assertEqual(detect.priority, "index")

    def test_accepts_the_limits(self):
        self.assertEqual(len(Job.from_dict(embed_frames_dict(32)).frames), 32)
        self.assertEqual(len(Job.from_dict(embed_text_dict(["사람이 앉아 있는 교실"] * 8)).texts), 8)

    def test_rejects_invalid_embed_jobs(self):
        frame = embed_frames_dict(1)["frames"][0]
        cases = {
            "unknown index_version_name": embed_frames_dict(index_version_name="clip-vit"),
            "unknown index_version_name ": embed_text_dict(index_version_name=None),
            "at most 32": embed_frames_dict(33),
            "has no frames": embed_frames_dict(0),
            "duplicate frame ids": embed_frames_dict(frames=[frame, dict(frame, pts_ns=5)]),
            "without path": embed_frames_dict(frames=[dict(frame, path="")]),
            "carries texts": embed_frames_dict(texts=["a car"]),
            "carries frames": embed_text_dict(frames=[frame]),
            "expected 1..8": embed_text_dict(["a"] * 9),
            "expected 1..8 ": embed_text_dict([]),
            "non-empty strings": embed_text_dict(["  "]),
            "non-empty strings ": embed_text_dict([7]),
            "non-empty strings  ": embed_text_dict(texts="a white car"),
            "longer than": embed_text_dict(["x" * 2001]),
            "priority": embed_text_dict(priority="urgent"),
            "sample_interval_ms": embed_frames_dict(sample_interval_ms=0),
            "sample_interval_ms ": embed_frames_dict(sample_interval_ms=1.5),
            "detect_frames job carries texts": {"job_id": "d", "kind": "detect_frames", "camera_id": "c",
                                                "session_id": "s", "frames": [frame], "texts": ["x"]},
        }
        for message, d in cases.items():
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message.strip()):
                Job.from_dict(d)


class EmbedResultContractTest(unittest.TestCase):
    def setUp(self):
        self.job = Job.from_dict(embed_frames_dict(3))
        self.result = ok_result(self.job)

    def test_valid_frame_and_text_results(self):
        self.assertEqual(validate_embed_result(self.result, self.job), [])
        text_job = Job.from_dict(embed_text_dict(["a", "b"]))
        self.assertEqual(validate_embed_result(ok_result(text_job), text_job), [])
        error = replace(self.result, status="error", vectors="", count=0, frame_ids=[], error="decode failed")
        self.assertEqual(validate_embed_result(error, self.job), [])

    def _problems(self, **changes):
        return " | ".join(validate_embed_result(replace(self.result, **changes), self.job))

    def test_flags_frame_ids_count_and_dims(self):
        self.assertIn("frame_ids", self._problems(frame_ids=["f1", "f0", "f2"]))
        self.assertIn("frame_ids", self._problems(frame_ids=["f0", "f1"]))
        self.assertIn("count 2", self._problems(count=2))
        self.assertIn("per_frame_ms", self._problems(per_frame_ms=[1]))
        self.assertIn("2 vectors for 3 inputs", self._problems(vectors=encode_vectors(unit_vectors(2, 768))))
        self.assertIn("not a multiple", self._problems(vectors=encode_vectors(unit_vectors(3, 768)[:-4])))
        self.assertIn("dims 1024 differ", self._problems(dims=1024))

    def test_flags_descriptor_and_hash(self):
        self.assertIn("is not the descriptor hash", self._problems(index_version="000000000000"))
        other = descriptor(sample_interval_ms=2000)
        self.assertIn("sample_interval_ms", self._problems(descriptor=other, index_version=descriptor_hash(other)))
        qwen = descriptor("qwen3vl-emb-2b-1024")
        self.assertIn("descriptor name", self._problems(descriptor=qwen, index_version=descriptor_hash(qwen)))
        wrong_model = dict(self.result.descriptor, model_id="google/other")
        self.assertIn("descriptor model", self._problems(descriptor=wrong_model))
        self.assertIn("descriptor keys", self._problems(descriptor={"name": "siglip2-b16-224"}))
        self.assertEqual(INDEX_VERSIONS["siglip2-b16-224"].dims, 768)

    def test_flags_non_unit_vectors_and_stale_generation(self):
        raw = bytearray(base64.b64decode(self.result.vectors))
        raw[0:4] = struct.pack("<f", 3.0)
        self.assertIn("vector 0: norm", self._problems(vectors=encode_vectors(bytes(raw))))
        self.assertIn("generation mismatch", self._problems(generation=3))
        self.assertIn("error result carries vectors", self._problems(status="error"))


if __name__ == "__main__":
    unittest.main()
