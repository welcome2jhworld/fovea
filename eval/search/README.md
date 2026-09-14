# Search evaluation set

Question sets for the M4 retrieval evaluation (`docs/M4_DESIGN.md`, "Evaluation").
They measure how well natural-language search over indexed footage finds the
right time ranges, in Korean and English, including queries that should find
nothing.

| File | Split | Use |
| --- | --- | --- |
| `questions.tune.json` | tune (17 questions) | Pick thresholds, sampling intervals, prompt templates |
| `questions.json` | final (36 questions) | Final report only; never used to choose any setting |

No question text appears in both files. The corpus is small, so both splits
share the same footage and some visual concepts; only the question texts and
the ordering pairs are disjoint.

## File format

```json
{
  "schema_version": 1,
  "split": "final",
  "videos": [{"name", "sha256", "duration_s", "width", "height", "fps", "source", "license", "url"}],
  "questions": [{
    "id": "f006",
    "query": "a car",
    "language": "en",
    "category": "presence",
    "video_sha256": "d31e0ebf...",
    "ranges": [{"start_s": 4.5, "end_s": 8.5}],
    "also_matches": [{"video_sha256": "452b11b7...", "ranges": [{"start_s": 14.0, "end_s": 19.5}]}],
    "negative": false,
    "ambiguous": false,
    "note": "",
    "pair": null
  }]
}
```

- `id`, `query`, `language` (`ko` | `en`), `video_sha256`, `ranges`, `negative`
  are the fields named in `docs/M4_DESIGN.md`. The rest are additions.
- `ranges` are seconds from the start of the file (first frame pts is 0 for
  every corpus video) and list every occurrence in that video.
- `also_matches` lists every other corpus video where the query is also true,
  with all of its occurrences. Search runs over all corpus cameras, so the
  ground truth of a positive question is `video_sha256`/`ranges` plus every
  `also_matches` entry. Scoring against `video_sha256` alone would count
  correct results in other videos as false positives.
- `negative: true` means no corpus video matches: `video_sha256` is `null`
  and `ranges` and `also_matches` are empty.
- `ambiguous: true` marks questions whose ground truth depends on
  interpretation (colour names, partly visible objects, arriving vs leaving in
  top-down footage). `note` says why. Report them separately from the headline
  metrics.
- `category`: `scene`, `presence`, `count`, `attribute`, `action`, `negative`.
- `pair` links two ordering-sensitive questions (standing up vs sitting down,
  a car entering vs leaving). Single frames of the two events look alike, so
  embedding-only search is not expected to separate them; they are there to
  show that limit, not to be tuned on.

## Corpus and sha256 matching

Footage is not stored in the repository. The evaluation script receives video
files by path, computes each file's sha256 and matches it against the `videos`
manifest, which is identical in both files. A file whose hash is not in the
manifest is not part of this corpus. Questions whose `video_sha256` is not
among the provided files cannot be scored and must be counted as skipped;
`also_matches` entries for missing videos are dropped from the ground truth.
Negative questions stay valid for any subset of the corpus.

| Name | sha256 | Duration | Resolution | fps | Source / license | Public URL |
| --- | --- | --- | --- | --- | --- | --- |
| classroom | `a69bd5e39ff0e74286d13bcbfdadddd307b85decd2d9853db7518e0028785e31` | 32.80 s | 1920x1080 | 30 | private, not redistributable | private |
| lot-walking | `452b11b7e0efbd019f1d9570d0c790e90416ad4ad29eec6003872d08443140ef` | 53.92 s | 768x432 | 12 | private, not redistributable | private |
| lot-whitecar | `d31e0ebf194cc16e50eea784e7c3b0b1bb7976a4fb9b3fec26cb92a6704cca34` | 30.16 s | 768x432 | 12.5 | private, not redistributable | private |
| people-walking | `822671fb20ed13ef9ead91d1f3678863b5aa078dd3b2ed45953fda39a61c979d` | 13.64 s | 1920x1080 | 25 | Pexels video 853889 "Black And White Video Of People" by Coverr; Pexels License | https://media.roboflow.com/supervision/video-examples/people-walking.mp4 |
| market-square | `21684157ced29570f2c33781c7bfe60ac04301a73c61e0778afd93c14bb70390` | 7.90 s | 2160x3840 | 60 | Pexels video 3687560; Pexels License | https://media.roboflow.com/supervision/video-examples/market-square.mp4 |
| skiing | `81ffb0d3670805a82d4b8354ad6c87e9701a6f5d71757cc89fdc133f76559ab5` | 14.12 s | 1920x1080 | 25 | Pexels video 4274798 by Adrien JACTA; Pexels License | https://media.roboflow.com/supervision/video-examples/skiing.mp4 |

What the videos show:

- classroom: fixed camera in a training room, four people at white desks. The
  man by the window stands up (3-5 s), stands by the flip chart, sits down
  (15-16.5 s); a person in the back row raises a hand (17-20 s), stands up
  (20.5-22 s) and sits down (26-27.5 s).
- lot-walking: high overhead view of an empty parking lot. A person in dark
  clothes walks through (1-7.5 s), a white SUV drives through (14-19.5 s), a
  cyclist (24.5-29.5 s), a person in a light top (40-46.5 s), a pale
  blue-green car (43.5-49.5 s) and a second cyclist (44.5-50 s).
- lot-whitecar: top-down view of asphalt. A white hatchback (4.5-8.5 s), a
  silver car and a red car passing each other (14.5-19 s), a white low sedan
  (25-28 s).
- people-walking: black-and-white overhead shot of a crowd crossing a plaza.
- market-square: portrait high-angle shot of a town square with a fountain,
  cafe parasols, a crowd, a person in a yellow jacket, a bicycle in the crowd
  and bicycles parked at the top-left edge.
- skiing: camera following a skier in a turquoise jacket with a red backpack
  down a sunny slope; a chairlift appears near the end.

### Provenance of the public videos

The three public videos are `supervision` package assets
(`supervision.assets.VideoAssets`, downloaded with
`supervision.assets.download_assets`, md5 checked against the package
catalog). The `supervision` code and docs state no license for the video
assets (the package's MIT license covers its code), so each used asset was
traced to its original source:

- skiing: the supervision pull request that added it (roboflow/supervision
  #1657) credits Pexels video 4274798 by Adrien JACTA,
  https://www.pexels.com/video/ski-montagne-skier-piste-de-ski-4274798/
- people-walking: Pexels video 853889 by Coverr,
  https://www.pexels.com/video/black-and-white-video-of-people-853889/
- market-square: Pexels video 3687560; the Pexels page could not be loaded
  from the annotation machine (bot protection), so the creator name is not
  recorded. File used for the match:
  https://videos.pexels.com/video-files/3687560/3687560-hd_1080_1920_30fps.mp4

For all three, frames of the supervision file were compared with the Pexels
file at the same timestamps: mean absolute grey-level difference 0.3-0.6
(people-walking, market-square) and 1.3-2.4 (skiing, a lower-resolution
Pexels rendition) out of 255, against 15-35 at a 2-2.5 s offset, with equal
durations. The Pexels License (https://www.pexels.com/license/) allows free use
and modification without attribution; it forbids redistributing the files on
other stock platforms. This repository stores only hashes and annotations.

Not used: `vehicles.mp4`, `grocery-store.mp4` and `subway.mp4`. The
supervision code, docs and the pull request that added them
(roboflow/supervision #476) give no source or license, and no original source
was found, so their license is unclear. The remaining supervision video assets
were not considered. As a result the corpus has no store-aisle, subway or
highway footage; questions about those scenes are negatives.

Getting the public videos (keep them outside the repository):

```sh
worker/.venv/bin/python -c "
from supervision.assets import download_assets, VideoAssets as V
for asset in (V.PEOPLE_WALKING, V.MARKET_SQUARE, V.SKIING):
    print(download_assets(asset, '<corpus-dir>'))
"
shasum -a 256 <corpus-dir>/*.mp4
```

The private videos are available only to their owner; without them, 29 of the
41 positive questions (those whose `video_sha256` is a private video) are
skipped.

## Question counts

| | ko | en | total |
| --- | --- | --- | --- |
| final (`questions.json`) | 20 | 16 | 36 |
| tune (`questions.tune.json`) | 8 | 9 | 17 |
| all | 28 (53 %) | 25 | 53 |

| Category | final | tune |
| --- | --- | --- |
| scene | 5 | 3 |
| presence | 4 | 2 |
| count | 4 | 2 |
| attribute | 7 | 3 |
| action | 8 | 3 |
| negative | 8 | 4 |

Ambiguous: 4 in final, 1 in tune. Ordering pairs: `classroom-front-stand-sit`
and `car-enter-leave` (final), `classroom-back-stand-sit` (tune).

## Annotation method

- Each video was rendered into 1 fps contact sheets with ffmpeg (`fps=1`,
  `tile`) whose tiles carry a timestamp burnt in by `drawtext` (`%{pts\:flt}`).
  In the sheets used here `drawtext` ran after `fps`, so a label is the sample
  slot and the frame shown is the last source frame before label + 1/fps, about
  0.45 s later. Put `drawtext` before `fps` to label the frame itself.
- Every sheet was inspected by eye. Around each event boundary (an object
  entering or leaving the frame, a person standing up or sitting down) 2 fps
  sheets were rendered and inspected, plus crops where objects are small
  (the market-square bicycles, the chairlift, the frame corners of the
  parking-lot videos).
- A range starts between the last frame without the object and the first
  frame with it, and ends between the last frame with it and the first frame
  without it, rounded to 0.5 s. Because the sheets label slots rather than
  frames, every boundary was then re-read on exact single frames
  (`ffmpeg -ss T -frames:v 1`) in 0.25 s steps; that second pass is what makes
  the boundaries accurate to about 0.5 s, and to about 1 s where an object
  enters as a few pixels at the frame edge.
- "Visible" includes partly visible objects at the frame edge. Very small
  objects in crowds (market-square, people-walking) were not counted, which is
  why no question asks about them.
- Negatives were checked against every sheet of every video. Hard negatives
  (falling snow vs the ski slope, umbrellas vs cafe parasols, a motorcycle
  rider vs the cyclists, an empty classroom) say so in `note`.
- Descriptions of people are limited to clothing and pose; no question
  identifies anyone.

## Running the evaluation

`scripts/eval_search.py` (see `docs/M4_DESIGN.md`, "Evaluation", and
`scripts/eval_search.py --help` for its flags) takes the corpus files by path,
matches them to the manifest by sha256, imports each matched file into its own
camera through `POST /v1/imports`, waits for indexing, runs every question as
`POST /v1/search` over all corpus cameras and scores the results against the
ground truth above. Run it with `questions.tune.json` while choosing settings,
then once with `questions.json` for the report in `docs/verification/`, using
the same index versions for both.

## Editing rules

- Never move a question between files or reuse a query text in the other
  file; add a new id instead.
- Keep ids stable once a report has been published with them.
- Re-check `ranges` and `also_matches` for every corpus video when adding a
  question, and update the counts above.
