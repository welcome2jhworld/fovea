# M5 work plan

`docs/M5_DESIGN.md` is the contract: what M5 must do and what it must never do.
This file is the order to build it in and what each step has to show before it
counts as done. Nothing here overrides the design; where the two disagree, the
design wins and this file is wrong.

State at the time of writing: M4 shipped (search over recordings, tag
`v0.3.0-dev.1`). No M5 code exists. `vlm_role` already exists on a rule
revision and the API rejects anything but `none`
(`src/core/src/Analytics.cpp`), which is the seam M5 opens.

## The one rule that shapes everything

A model answer is never a fact. Every VLM output is an observation with
evidence frame ids, a time range, an uncertainty and a status, and the core
validates the evidence against the frames it sent before anything is stored or
shown. `uncertain`, `unparsed`, `timeout` and a worker outage are all
"unknown", and unknown never opens an event and never marks a search result
verified. Build the validation before the feature that needs it.

## Step 1: the VLM job contract in the worker

Extend `vlm_clip` to the request and response of the design ("VLM job
contract"): tasks `verify_query`, `describe`, `verify_rule`, `order_events`;
`verdict`, `observations[]` with evidence frame ids and times, `uncertainty`,
bounded `raw_text`, model id and revision, frames used, tokens, processing ms.

- The prompt states each frame's time and asks for JSON. Text inside frames is
  data; say so in the prompt and never let model output reach the core as an
  instruction.
- Missing JSON, an unknown frame id or a range outside the clip becomes
  `unparsed` or `uncertain`. Never `match`.
- Reference backend on the development Mac: `Qwen/Qwen3-VL-2B-Instruct`
  (bf16 on MPS). It is a stand-in for the contract, not the quality target; the
  target model runs on the Windows box.
- Model loads take the process-wide lock in `fovea_worker/modelload.py`.

Done when: unit tests with a fake backend cover every rejection path, and one
skip-unless-weights test runs the real model on a handful of frames and gets a
contract-valid answer.

## Step 2: temporal grounding calibration

`fovea-worker calibrate-time` generates the clips of the design (12 s, 640x360,
burnt-in frame counter, a coloured square visible over a known interval, three
placements, 2 fps), asks the backend for the interval, and records
`temporal_grounding: verified | unverified` with the measured errors in the
backend health.

Until a backend passes, the core ignores time ranges it reports and uses the
candidate range it sent. Write that path first so an unverified backend is the
default, not an afterthought.

Done when: the calibration runs on the Mac reference model and its result
appears in `GET /v1/health` of the worker and in `/v1/analysis` of the core,
with the errors, whatever the outcome is.

## Step 3: the VLM lane in the core

A `VlmClient` next to `EmbedClient`, and a scheduler with the priorities of the
design: `realtime` (rule verification), then `search` (a user is waiting), then
`background`. One request at a time overall.

- A realtime task past its deadline is dropped and recorded as `expired`, which
  makes the dependent rule observation `unknown`.
- Round robin across cameras: a camera never waits behind more than one task of
  another camera.
- `/v1/analysis` reports the load estimate `N x T / I` and the deadline hit
  rate. The hit rate is what the report states; the estimate alone proves
  nothing.

Done when: unit tests drive the queue with fake replies and show ordering,
expiry, the per-camera share and that a late answer for an old generation is
ignored.

## Step 4: search re-check

`POST /v1/search {..., recheck: {enabled, max_candidates, budget_ms}}` as in the
design: pad the best candidates by 2 s on each side, clamp to the segment,
sample at 2 fps up to 24 frames, list gaps, run `verify_query` sequentially
until the budget runs out, reorder match then uncertain then no_match, and keep
unchecked results in relevance order.

- The response carries `recheck_coverage` and never labels an unchecked result
  as verified.
- Order-sensitive questions use `order_events` and match only when the
  sub-event order matches the question and each sub-event has evidence frames.

Done when: `scripts/verify_m5.py` shows precision@5 before and after re-check
on `eval/search/questions.json`, negatives answered `no_match`, the misread vs
not-retrieved split, and a budget stop that leaves the rest unchecked rather
than wrong.

## Step 5: rules with a VLM role

`describe` and `verify` per the design. `verify` holds the trigger in
`pending_verification`, and only a `match` with valid evidence opens the event;
`no_match` is a rejected trigger visible in the rule activity, not an alert;
anything unknown freezes the timers per the M3 evaluator contract and retries
after `recheck_interval_ms` while the base condition still holds.

Done when: the verification script opens exactly one event for a verify
question that is true of the clip, zero events and rejected triggers for one
that is false, and unknown (no event) during a forced worker outage. Semantic
rules are reported separately from detector rules: trigger precision, rejected
triggers, unknown ratio, added latency.

## Step 6: natural-language rule drafting

`POST /v1/rules/draft` returns a draft, never a rule. The model fills a strict
schema of supported fields only; a deterministic validator then produces the
four lists of the design: `interpreted` (with the text span each value came
from), `unsupported`, `ambiguous` and `missing`. "Suspicious behaviour" is
ambiguous and is never turned into a verify question on its own. A rule saved
from a draft starts disabled.

Done when: `eval/rules/drafts.json` (Korean and English, including unsupported
and ambiguous cases) is scored for exact-field accuracy, unsupported and
ambiguous detection, and the count of drafts that would have produced a wrong
enabled rule, which must be zero.

## Step 7: follow-up questions

`POST /v1/search/{session_id}/follow-up`. The planner answers with exactly one
tool call from the fixed list (`refine`, `search`, `filter_time`,
`filter_cameras`, `select`); anything else is rejected. Arguments are validated
against the session: ids must exist, times must lie inside retained footage,
cameras must exist. At most 3 tool calls and 10 VLM checks per follow-up.

The planner sees ids, times and camera names. No SQL, no shell, no file paths,
no free-form code. Text read from frames or from earlier model output is passed
as quoted data.

Done when: a two-turn conversation on the evaluation corpus produces the
expected tool calls and a result subset whose evidence validates.

## Step 8: console

Search: re-check toggle, a verdict chip per result (verified match, no match,
uncertain, not checked) naming the model, evidence frames marked on the player
timeline, a follow-up input under the results that shows the tool call that
ran. Rules: the `vlm_role` selector, the verify question, rejected triggers in
the activity, and the "Describe the rule" draft panel with the four lists.
Alert detail: the description or verification result with its evidence.

The console keeps the M4 discipline: a relevance or a verdict is never shown as
a probability, and an unchecked result never looks checked.

## Order and parallelism

Steps 1 to 3 are the foundation and are worth doing in order. Steps 4, 5 and 6
are independent of each other once 3 exists. Step 7 needs 4. Step 8 can start
against the stub core as soon as the API shapes of the steps it renders are
fixed.

## What "done" means for the milestone

`scripts/verify_m5.py` passes on the development Mac with a report in
`docs/verification/`, the evaluation numbers are written down with the corpus
they came from, `docs/STATUS.md` has an M5 section with its "not verified"
list, and `docs/M5_DESIGN.md` carries an "As implemented" section for every
deviation. Then the milestone is published the way M4 was.
