# Search evaluation: qwen3vl-emb-2b-1024

Question set `questions.json` (split final): 36 answered, 0 skipped (video not provided). Index version `086fb0b0080e`, Qwen/Qwen3-VL-Embedding-2B at revision `9f2f7e710d6d`, 1024 dims, one sample per 1000 ms. Search limit 50, min_gap_ms 3000, candidate_fraction 0.3; a hit overlaps ground truth by at least 1 s. Percentiles are interpolated between the neighbouring values, so the median of an even count is the midpoint of the two middle ones. Relevance is embedding similarity for ranking only, not verified. Platform darwin arm64.

The corpus is 153 s of footage over 6 videos and the tune and final question sets describe the same footage, so these numbers compare index versions on this footage; they are not an estimate of recall in the field.

## Corpus

| Video | sha256 | Duration s | Segments |
| --- | --- | --- | --- |
| classroom | `a69bd5e39ff0` | 32.8 | 4 |
| lot-walking | `452b11b7e0ef` | 53.92 | 6 |
| lot-whitecar | `d31e0ebf194c` | 30.16 | 4 |
| market-square | `21684157ced2` | 7.9 | 1 |
| people-walking | `822671fb20ed` | 13.64 | 2 |
| skiing | `81ffb0d36708` | 14.12 | 2 |

## Retrieval

| Set | Questions | R@1 | R@5 | R@10 | P@5 | P@10 | Temporal error median s (start / end) | p90 s (start / end) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| headline (not ambiguous) | 24 | 0.875 | 0.917 | 0.958 | 0.317 | 0.196 | 1.833 / 2.167 | 13.776 / 20.788 |
| ko | 13 | 0.769 | 0.846 | 0.923 | 0.354 | 0.246 | 1.266 / 3.313 | 6.291 / 27.186 |
| en | 11 | 1.000 | 1.000 | 1.000 | 0.273 | 0.136 | 6.820 / 2.167 | 20.833 / 12.800 |
| action | 6 | 0.833 | 1.000 | 1.000 | 0.200 | 0.100 | 5.800 / 14.550 | 17.567 / 22.550 |
| attribute | 6 | 0.833 | 0.833 | 0.833 | 0.233 | 0.117 | 0.680 / 2.167 | 5.936 / 17.460 |
| count | 4 | 1.000 | 1.000 | 1.000 | 0.450 | 0.275 | 3.520 / 2.833 | 20.668 / 9.862 |
| presence | 3 | 1.000 | 1.000 | 1.000 | 0.533 | 0.267 | 1.833 / 0.667 | 5.823 / 9.061 |
| scene | 5 | 0.800 | 0.800 | 1.000 | 0.320 | 0.300 | 0.717 / 0.001 | 9.275 / 23.336 |
| ambiguous | 4 | 0.750 | 1.000 | 1.000 | 0.450 | 0.250 | 6.340 / 2.650 | 9.270 / 9.104 |

Precision@K divides by K, and a query returned 10.36 ranges on average (18 at most), so P@10 is bounded by a tenth of the ranges returned rather than by how many of them are right.

Misses in the headline set. Not retrieved means no overlapping range among those returned, with limit 50 and the candidates capped at candidate_fraction of the samples scanned:

| K | not retrieved | ranked low |
| --- | --- | --- |
| @1 | 1 | 2 |
| @5 | 1 | 1 |
| @10 | 1 | 0 |

## Negative questions

8 negatives. Top relevance min / median / max: 0.354 / 0.458 / 0.544; positives (headline): 0.341 / 0.573 / 0.747. Negatives whose top relevance is above the median positive top relevance: 0.000.

## Latency and index cost

| Latency ms | p50 | p95 |
| --- | --- | --- |
| embed | 84.000 | 111.500 |
| scan | 1.000 | 1.000 |
| total | 86.500 | 114.500 |
| client_wall | 88.150 | 116.125 |

Indexed 152.53 s of footage (152 samples) with 134.59 s of compute: 3176.500 compute s per footage hour. Stored per footage hour: vectors 14754029 B, thumbnails 44117642 B, row payload 1650246 B (column bytes, without SQLite page and index overhead).

## Questions

| id | lang | category | query | first hit rank | top relevance | top result |
| --- | --- | --- | --- | --- | --- | --- |
| f001 | ko | scene | 교실에 학생들이 앉아 있는 장면 | 1 | 0.534 | classroom 0.7-32.8 s |
| f002 | ko | scene | 위에서 내려다본 주차장 | 1 | 0.618 | lot-walking 5.67-15.67 s |
| f003 | ko | scene | 스키장 슬로프 | 1 | 0.616 | skiing 0.68-14.12 s |
| f004 | en | scene | a town square with a fountain in the middle | 1 | 0.662 | market-square 0.72-7.9 s |
| f005 | ko | scene | 흑백으로 촬영된 영상 | 6 | 0.508 | lot-whitecar 18.64-30.16 s |
| f006 | en | presence | a car | 1 | 0.574 | lot-whitecar 7.68-30.16 s |
| f007 | ko | presence | 자전거 | 1 | 0.519 | lot-walking 42.67-50.67 s |
| f008 | en | presence | a flip chart standing next to a window | 1 | 0.482 | classroom 0.7-32.8 s |
| f009 | ko | presence (ambiguous) | 스키 리프트 | 1 | 0.539 | skiing 0.68-14.12 s |
| f010 | ko | count | 여러 사람이 걸어가는 장면 | 1 | 0.576 | people-walking 0.68-13.64 s |
| f011 | en | count | four people sitting in a room | 1 | 0.556 | classroom 0.7-32.8 s |
| f012 | ko | count | 차 두 대가 서로 반대 방향으로 지나가는 장면 | 1 | 0.540 | lot-whitecar 8.64-30.16 s |
| f013 | ko | count | 텅 빈 주차장 | 1 | 0.658 | lot-walking 29.67-45.67 s |
| f014 | ko | attribute | 흰색 자동차 | 1 | 0.606 | lot-whitecar 0.64-30.16 s |
| f015 | en | attribute | a red car | 1 | 0.645 | lot-whitecar 7.68-30.16 s |
| f016 | en | attribute | a person in dark clothes walking across a parking lot | 1 | 0.744 | lot-walking 0.67-9.67 s |
| f017 | ko | attribute | 노란 재킷을 입은 사람 | - | 0.418 | lot-walking 1.67-7.67 s |
| f018 | en | attribute | a skier wearing a turquoise jacket and a red backpack | 1 | 0.668 | skiing 0.68-14.12 s |
| f019 | ko | attribute (ambiguous) | 은색 자동차 | 1 | 0.559 | lot-whitecar 7.68-30.16 s |
| f020 | en | attribute | a white SUV | 1 | 0.572 | lot-walking 14.67-19.67 s |
| f021 | en | action | a student raising a hand | 1 | 0.498 | classroom 8.7-32.8 s |
| f022 | ko | action | 스키를 타고 내려가는 사람 | 1 | 0.627 | skiing 0.68-14.12 s |
| f023 | en | action | someone riding a bicycle across a parking lot | 1 | 0.747 | lot-walking 23.67-53.92 s |
| f024 | ko | action | 창가 앞자리에 앉아 있던 남자가 자리에서 일어나는 장면 | 3 | 0.341 | lot-whitecar 17.68-30.16 s |
| f025 | en | action | the man by the window sits back down at his desk | 1 | 0.422 | classroom 0.7-32.8 s |
| f026 | en | action (ambiguous) | a car entering the parking lot | 2 | 0.639 | lot-whitecar 25.68-30.16 s |
| f027 | ko | action (ambiguous) | 차가 주차장을 빠져나가는 장면 | 1 | 0.589 | lot-whitecar 21.68-30.16 s |
| f028 | ko | action | 교실 앞에 서 있는 남자 | 1 | 0.479 | classroom 0.7-32.8 s |
| f029 | ko | negative | 소방차 | negative | 0.462 | lot-whitecar 8.64-30.16 s |
| f030 | en | negative | a dog running on the grass | negative | 0.354 | lot-walking 33.67-45.67 s |
| f031 | ko | negative | 눈이 내리는 거리 | negative | 0.544 | lot-whitecar 8.64-30.16 s |
| f032 | en | negative | a bus stopped at a bus stop | negative | 0.365 | lot-whitecar 8.64-15.64 s |
| f033 | ko | negative | 마트 진열대 사이 통로 | negative | 0.403 | people-walking 0.68-13.64 s |
| f034 | en | negative | people waiting on a subway platform | negative | 0.530 | people-walking 0.68-13.64 s |
| f035 | ko | negative | 밤에 촬영된 도로 | negative | 0.487 | lot-whitecar 8.64-30.16 s |
| f036 | en | negative | a person lying on the floor | negative | 0.454 | lot-whitecar 19.68-30.16 s |
