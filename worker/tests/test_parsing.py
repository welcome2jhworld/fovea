import unittest
from dataclasses import replace

from fovea_worker.backends.mlx_vlm_backend import MlxVlmBackend
from fovea_worker.parsing import parse_json_observations, parse_marlin_caption, strip_thinking
from fovea_worker.protocol import Job, sample_frames, validate_result_against_job


def _job(n=16, fps=2.0, start_ns=0, clip=False, **extra):
    frames = [{"frame_id": f"f{i:04d}", "pts_ns": start_ns + int(i / fps * 1e9), "recv_mono_ns": 0,
               "capture_utc_ms": 0, "path": f"/tmp/f{i}.jpg", "index": i} for i in range(n)]
    clip_range = {"session_id": "s", "start_pts_ns": start_ns, "end_pts_ns": frames[-1]["pts_ns"],
                  "start_utc_ms": 0, "end_utc_ms": 0} if clip else None
    d = {"job_id": "j", "kind": "vlm_clip", "camera_id": "c", "generation": 1,
         "frames": frames, "clip": clip_range, "gaps": [], "limits": {}}
    d.update(extra)
    return Job.from_dict(d)


class ParsingTest(unittest.TestCase):
    def test_strip_thinking(self):
        self.assertEqual(strip_thinking("<think>hmm</think>\nScene: x"), "Scene: x")
        self.assertEqual(strip_thinking("reasoning opened in the prompt</think>\nScene: x"), "Scene: x")

    def test_truncated_thinking_is_empty_output(self):
        self.assertEqual(strip_thinking("<think>maybe a person holding a knife"), "")
        outcome = parse_json_observations("<think>maybe a person holding a knife", _job())
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.error, "empty model output")
        self.assertEqual(outcome.status, "unparsed")

    def test_marlin_events_map_to_frames(self):
        text = ("Scene: A parking lot.\n\nEvents:\n<0.0 - 2.5> The white car reverses.\n"
                "<2.5 - 3.5> The car drives forward.\n<3.5 - 7.5> The lot is empty.\n")
        outcome = parse_marlin_caption(text, _job())
        self.assertEqual(outcome.error, "")
        self.assertEqual(outcome.status, "ok")
        obs = outcome.observations
        self.assertEqual(len(obs), 4)
        self.assertEqual(obs[0].labels, ["scene"])
        self.assertEqual(obs[1].evidence.frame_ids, ["f0000", "f0001", "f0002", "f0003", "f0004", "f0005"])
        self.assertEqual(obs[1].evidence.start_pts_ns, 0)
        self.assertEqual(obs[1].evidence.end_pts_ns, 2_500_000_000)
        self.assertEqual(obs[3].evidence.frame_ids[0], "f0007")
        self.assertEqual(obs[3].evidence.frame_ids[-1], "f0015")

    def test_marlin_event_times_are_relative_to_clip_start(self):
        job = _job(start_ns=5_000_000_000, clip=True)
        outcome = parse_marlin_caption("Events:\n<0.0 - 2.5> A car reverses.\n<5.0 - 7.5> The lot is empty.", job)
        self.assertEqual(outcome.error, "")
        first, second = outcome.observations
        self.assertEqual(first.evidence.frame_ids, ["f0000", "f0001", "f0002", "f0003", "f0004", "f0005"])
        self.assertEqual(first.evidence.start_pts_ns, 5_000_000_000)
        self.assertEqual(second.evidence.frame_ids, ["f0010", "f0011", "f0012", "f0013", "f0014", "f0015"])
        self.assertEqual(outcome.notes, [])

    def test_marlin_event_outside_clip_is_dropped_not_given_every_frame(self):
        outcome = parse_marlin_caption("Scene: A lot.\nEvents:\n<20.0 - 25.0> something", _job())
        self.assertEqual([o.labels for o in outcome.observations], [["scene"]])
        self.assertEqual(outcome.status, "partial")
        self.assertIn("event <20.0 - 25.0> matches no frame; dropped", outcome.notes)

    def test_marlin_unparsable_caption_has_no_observations(self):
        outcome = parse_marlin_caption("Events:\n<20.0 - 25.0> something", _job())
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.error, "no localisable observations")
        outcome = parse_marlin_caption("just prose", _job())
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.status, "unparsed")

    def test_json_with_frame_ids(self):
        text = '{"observations": [{"text": "person", "frame_ids": ["f0002", "f0003"], "uncertainty": "low"}]}'
        outcome = parse_json_observations(text, _job())
        self.assertEqual(outcome.error, "")
        self.assertEqual(outcome.observations[0].evidence.frame_ids, ["f0002", "f0003"])
        self.assertEqual(outcome.observations[0].uncertainty, "low")

    def test_json_with_ranges_only(self):
        text = '{"observations": [{"text": "car", "frame_ids": [], "start_s": 1.0, "end_s": 2.0}]}'
        outcome = parse_json_observations(text, _job())
        self.assertEqual(outcome.observations[0].evidence.frame_ids, ["f0002", "f0003", "f0004"])

    def test_json_empty_observations_is_an_empty_ok_result(self):
        outcome = parse_json_observations('{"observations": []}', _job(4))
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.error, "")
        self.assertEqual(outcome.status, "ok")

    def test_json_citing_invented_frame_ids_is_rejected_whole(self):
        text = ('{"observations": [{"text": "a car", "frame_ids": ["f0001"]},'
                ' {"text": "a person", "frame_ids": ["f0099"]}]}')
        outcome = parse_json_observations(text, _job(4))
        self.assertEqual(outcome.observations, [])
        self.assertTrue(outcome.error.startswith("no localisable observations"))
        self.assertIn("f0099", outcome.error)

    def test_json_bad_item_is_dropped_without_losing_valid_items(self):
        text = ('{"observations": [{"text": "a car", "frame_ids": ["f0001"]}, {"text": "x", "frame_ids": 3},'
                ' "junk", {"text": "late", "start_s": 90, "end_s": 95}]}')
        outcome = parse_json_observations(text, _job(4))
        self.assertEqual([o.text for o in outcome.observations], ["a car"])
        self.assertEqual(outcome.status, "partial")
        self.assertEqual(len(outcome.notes), 3)

    def test_json_with_no_localisable_item_is_unparsed(self):
        outcome = parse_json_observations('{"observations": [{"text": "late", "start_s": 90, "end_s": 95}]}', _job(4))
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.error, "no localisable observations")

    def test_prose_yields_no_observation(self):
        outcome = parse_json_observations("<think>x</think>흰색 차가 주차합니다.", _job())
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.status, "unparsed")

    def test_garbage_json_yields_no_observation(self):
        outcome = parse_json_observations('{"observations": ["frame_ids": [0.0]]}', _job())
        self.assertEqual(outcome.observations, [])
        self.assertEqual(outcome.error, "no JSON object in model output")

    def test_zero_frame_job_does_not_crash(self):
        job = replace(_job(2), frames=[])
        self.assertEqual(parse_marlin_caption("Scene: x", job).error, "job has no frames")
        self.assertEqual(parse_json_observations('{"observations": [{"text": "x", "start_s": 0, "end_s": 1}]}',
                                                 job).error, "no localisable observations")


class SampledFramesTest(unittest.TestCase):
    def test_sample_frames_spreads_over_the_job(self):
        frames = _job(32).frames
        sampled = sample_frames(frames, 16)
        self.assertEqual(len(sampled), 16)
        self.assertEqual(len({f.frame_id for f in sampled}), 16)
        self.assertEqual((sampled[0].frame_id, sampled[-1].frame_id), ("f0000", "f0031"))
        self.assertEqual(sample_frames(frames[:3], 16), frames[:3])
        self.assertEqual(sample_frames(frames, 1), frames[:1])

    def test_image_backend_cannot_cite_frames_it_did_not_send(self):
        class StubMlx(MlxVlmBackend):
            def __init__(self, reply):
                super().__init__(model_id="stub-vlm")
                self.reply = reply
                self.sent = []

            def _run_images(self, job):
                self.sent = [f.frame_id for f in job.frames]
                return self.reply, ""

        job = _job(32, limits={"max_frames": 16})
        unsent = next(f.frame_id for f in job.frames if f not in sample_frames(job.frames, 16))
        backend = StubMlx('{"observations": [{"text": "a person", "frame_ids": ["%s"]}]}' % unsent)
        result = backend.run(job)
        self.assertEqual(len(backend.sent), 16)
        self.assertNotIn(unsent, backend.sent)
        self.assertEqual(result.input_frames, 16)
        self.assertEqual(result.status, "unparsed")
        self.assertEqual(result.observations, [])
        self.assertEqual(result.device, "mlx")
        self.assertEqual(validate_result_against_job(result, job), [])

        backend = StubMlx('{"observations": [{"text": "a person", "start_s": 0, "end_s": 20}]}')
        result = backend.run(job)
        cited = result.observations[0].evidence.frame_ids
        self.assertEqual(cited, backend.sent)
        self.assertEqual(validate_result_against_job(result, job), [])


if __name__ == "__main__":
    unittest.main()
