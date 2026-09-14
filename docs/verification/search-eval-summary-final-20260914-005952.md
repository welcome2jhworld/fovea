# Search evaluation summary: siglip2-b16-224 vs qwen3vl-emb-2b-1024

Both index versions were evaluated with `scripts/eval_search.py` on the same corpus and the same final question set (`eval/search/questions.json`, 36 questions: 24 headline, 4 ambiguous, 8 negative), one sample per 1000 ms, search limit 50, min_gap_ms 3000, candidate_fraction 0.3, hit = overlap of at least 1 s, percentiles interpolated. Reports: `search-eval-siglip2-b16-224-final-cf0.3-20260914-005638.{json,md}` and `search-eval-qwen3vl-emb-2b-1024-final-cf0.3-20260914-005653.{json,md}`. Platform: macOS arm64 (Apple M4, 16 GB, MPS); both runs shared the machine with this session's own work. Corpus: 6 videos, 152.5 s of footage, 152 samples. Written 2026-09-14 00:59 UTC.

The corpus is small (about two and a half minutes) and the tune and final splits describe the same footage, so these numbers separate the two models on this footage only; they are not an estimate of field recall.

## Index versions

| | siglip2-b16-224 | qwen3vl-emb-2b-1024 |
| --- | --- | --- |
| Model | google/siglip2-base-patch16-224 | Qwen/Qwen3-VL-Embedding-2B |
| Revision | `75de2d55ec2d` | `9f2f7e710d6d` |
| Index version hash | `b63a3a358815` | `086fb0b0080e` |
| Dims / dtype | 768 / float32 | 1024 / float16 |

## Retrieval (headline set, 24 questions that are not ambiguous)

| Metric | siglip2-b16-224 | qwen3vl-emb-2b-1024 |
| --- | --- | --- |
| Recall@1 | 0.917 | 0.875 |
| Recall@5 (the deciding metric) | **1.000** | **0.917** |
| Recall@10 | 1.000 | 0.958 |
| Precision@5 | 0.317 | 0.317 |
| Precision@10 | 0.171 | 0.196 |
| Temporal error median s (start / end) | 0.709 / 0.169 | 1.833 / 2.167 |
| Temporal error p90 s (start / end) | 26.210 / 15.250 | 13.776 / 20.788 |
| Misses @5 (not retrieved / ranked low) | 0 / 0 | 1 / 1 |
| Misses @10 (not retrieved / ranked low) | 0 / 0 | 1 / 0 |

## By language (headline set)

| Set | siglip2-b16-224 R@1 / R@5 / R@10 | qwen3vl-emb-2b-1024 R@1 / R@5 / R@10 |
| --- | --- | --- |
| ko (13) | 0.923 / 1.000 / 1.000 | 0.769 / 0.846 / 0.923 |
| en (11) | 0.909 / 1.000 / 1.000 | 1.000 / 1.000 / 1.000 |

## Negative questions (8)

| Metric | siglip2-b16-224 | qwen3vl-emb-2b-1024 |
| --- | --- | --- |
| Negative top relevance min / median / max | 0.058 / 0.094 / 0.132 | 0.354 / 0.458 / 0.544 |
| Positive top relevance min / median / max | 0.066 / 0.123 / 0.212 | 0.341 / 0.573 / 0.747 |
| Negatives above the median positive top score | 0.125 | 0.000 |

## Latency and index cost

| Metric | siglip2-b16-224 | qwen3vl-emb-2b-1024 |
| --- | --- | --- |
| Query embed ms p50 / p95 | 13.0 / 19.0 | 84.0 / 111.5 |
| Scan ms p50 / p95 | 1.0 / 1.0 | 1.0 / 1.0 |
| Total ms p50 / p95 | 15.0 / 22.75 | 86.5 / 114.5 |
| Index compute s per footage hour | 226.5 | 3176.5 |
| Vector bytes per footage hour | 11 080 438 | 14 754 029 |
| Thumbnail bytes per footage hour | 44 117 642 | 44 117 642 |

## Decision

The rule, fixed before either evaluation ran and recorded in `docs/M4_DESIGN.md` and `docs/DECISIONS.md` (D11): the default index version is the one with the higher Recall@5 on the final set; on a tie, the cheaper one to index. Recall@5 is 1.000 for siglip2-b16-224 and 0.917 for qwen3vl-emb-2b-1024, so **`siglip2-b16-224` stays the default** (`search.active_index_version`), at 226 compute s per footage hour against 3176. Nothing was tuned on the final set; candidate_fraction was chosen on `questions.tune.json`, whose five runs are in this directory (`search-eval-siglip2-b16-224-tune-cf*`) and summarised in `docs/M4_DESIGN.md`.

What the numbers do not settle: qwen3vl-emb-2b-1024 keeps negative queries below the positives by score (0.000 of them score above the median positive top score, against 0.125 for siglip2-b16-224), which matters once a score threshold or the M5 re-check is added, and it costs about 14x more compute per hour of footage on this Mac, which is 0.88x real time for one camera at one sample per second and misses the throughput target of `docs/M4_DESIGN.md`. Both versions remain selectable per query (`index_version`) and the other one can be made active with `PUT /v1/index/active`.
