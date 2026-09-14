# Search evaluation: siglip2-b16-224

Question set `questions.json` (split final): 36 answered, 0 skipped (video not provided). Index version `b63a3a358815`, google/siglip2-base-patch16-224 at revision `75de2d55ec2d`, 768 dims, one sample per 1000 ms. Search limit 50, min_gap_ms 3000, candidate_fraction 0.3; a hit overlaps ground truth by at least 1 s. Percentiles are interpolated between the neighbouring values, so the median of an even count is the midpoint of the two middle ones. Relevance is embedding similarity for ranking only, not verified. Platform darwin arm64.

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
| headline (not ambiguous) | 24 | 0.917 | 1.000 | 1.000 | 0.317 | 0.171 | 0.709 / 0.169 | 26.210 / 15.250 |
| ko | 13 | 0.923 | 1.000 | 1.000 | 0.354 | 0.200 | 0.680 / 0.004 | 20.426 / 14.868 |
| en | 11 | 0.909 | 1.000 | 1.000 | 0.273 | 0.136 | 2.333 / 4.640 | 26.800 / 12.800 |
| action | 6 | 1.000 | 1.000 | 1.000 | 0.200 | 0.100 | 8.550 / 14.550 | 21.817 / 22.550 |
| attribute | 6 | 0.667 | 1.000 | 1.000 | 0.267 | 0.133 | 0.680 / 3.140 | 4.025 / 7.167 |
| count | 4 | 1.000 | 1.000 | 1.000 | 0.350 | 0.175 | 0.520 / 0.002 | 18.964 / 3.599 |
| presence | 3 | 1.000 | 1.000 | 1.000 | 0.533 | 0.267 | 2.333 / 3.916 | 20.333 / 4.917 |
| scene | 5 | 1.000 | 1.000 | 1.000 | 0.360 | 0.240 | 0.700 / 0.000 | 30.087 / 0.003 |
| ambiguous | 4 | 0.750 | 1.000 | 1.000 | 0.550 | 0.275 | 6.846 / 5.917 | 10.182 / 10.312 |

Precision@K divides by K, and a query returned 8.47 ranges on average (15 at most), so P@10 is bounded by a tenth of the ranges returned rather than by how many of them are right.

Misses in the headline set. Not retrieved means no overlapping range among those returned, with limit 50 and the candidates capped at candidate_fraction of the samples scanned:

| K | not retrieved | ranked low |
| --- | --- | --- |
| @1 | 0 | 2 |
| @5 | 0 | 0 |
| @10 | 0 | 0 |

## Negative questions

8 negatives. Top relevance min / median / max: 0.058 / 0.094 / 0.132; positives (headline): 0.066 / 0.123 / 0.212. Negatives whose top relevance is above the median positive top relevance: 0.125.

## Latency and index cost

| Latency ms | p50 | p95 |
| --- | --- | --- |
| embed | 13.000 | 19.000 |
| scan | 1.000 | 1.000 |
| total | 15.000 | 22.750 |
| client_wall | 16.650 | 24.675 |

Indexed 152.53 s of footage (152 samples) with 9.6 s of compute: 226.500 compute s per footage hour. Stored per footage hour: vectors 11080438 B, thumbnails 44117642 B, row payload 1650246 B (column bytes, without SQLite page and index overhead).

## Questions

| id | lang | category | query | first hit rank | top relevance | top result |
| --- | --- | --- | --- | --- | --- | --- |
| f001 | ko | scene | 교실에 학생들이 앉아 있는 장면 | 1 | 0.147 | classroom 0.7-32.8 s |
| f002 | ko | scene | 위에서 내려다본 주차장 | 1 | 0.161 | lot-walking 49.67-53.92 s |
| f003 | ko | scene | 스키장 슬로프 | 1 | 0.116 | skiing 0.68-14.12 s |
| f004 | en | scene | a town square with a fountain in the middle | 1 | 0.165 | market-square 0.72-7.9 s |
| f005 | ko | scene | 흑백으로 촬영된 영상 | 1 | 0.137 | people-walking 0.68-13.64 s |
| f006 | en | presence | a car | 1 | 0.105 | lot-walking 11.67-24.67 s |
| f007 | ko | presence | 자전거 | 1 | 0.119 | lot-walking 19.67-53.92 s |
| f008 | en | presence | a flip chart standing next to a window | 1 | 0.066 | classroom 0.7-32.8 s |
| f009 | ko | presence (ambiguous) | 스키 리프트 | 1 | 0.116 | skiing 0.68-14.12 s |
| f010 | ko | count | 여러 사람이 걸어가는 장면 | 1 | 0.166 | people-walking 0.68-13.64 s |
| f011 | en | count | four people sitting in a room | 1 | 0.167 | classroom 0.7-32.8 s |
| f012 | ko | count | 차 두 대가 서로 반대 방향으로 지나가는 장면 | 1 | 0.147 | lot-whitecar 14.64-23.64 s |
| f013 | ko | count | 텅 빈 주차장 | 1 | 0.164 | lot-walking 49.67-53.92 s |
| f014 | ko | attribute | 흰색 자동차 | 1 | 0.133 | lot-whitecar 25.68-29.64 s |
| f015 | en | attribute | a red car | 1 | 0.118 | lot-whitecar 14.64-23.64 s |
| f016 | en | attribute | a person in dark clothes walking across a parking lot | 1 | 0.212 | lot-walking 0.67-14.67 s |
| f017 | ko | attribute | 노란 재킷을 입은 사람 | 3 | 0.079 | lot-walking 1.67-7.67 s |
| f018 | en | attribute | a skier wearing a turquoise jacket and a red backpack | 1 | 0.117 | skiing 0.68-14.12 s |
| f019 | ko | attribute (ambiguous) | 은색 자동차 | 1 | 0.123 | lot-whitecar 4.64-30.16 s |
| f020 | en | attribute | a white SUV | 2 | 0.113 | lot-whitecar 4.64-8.68 s |
| f021 | en | action | a student raising a hand | 1 | 0.089 | classroom 0.7-32.8 s |
| f022 | ko | action | 스키를 타고 내려가는 사람 | 1 | 0.126 | skiing 0.68-14.12 s |
| f023 | en | action | someone riding a bicycle across a parking lot | 1 | 0.204 | lot-walking 17.67-49.67 s |
| f024 | ko | action | 창가 앞자리에 앉아 있던 남자가 자리에서 일어나는 장면 | 1 | 0.067 | classroom 0.7-32.8 s |
| f025 | en | action | the man by the window sits back down at his desk | 1 | 0.094 | classroom 0.7-32.8 s |
| f026 | en | action (ambiguous) | a car entering the parking lot | 2 | 0.162 | lot-walking 14.67-26.67 s |
| f027 | ko | action (ambiguous) | 차가 주차장을 빠져나가는 장면 | 1 | 0.144 | lot-walking 14.67-26.67 s |
| f028 | ko | action | 교실 앞에 서 있는 남자 | 1 | 0.112 | classroom 0.7-32.8 s |
| f029 | ko | negative | 소방차 | negative | 0.077 | lot-whitecar 0.64-15.64 s |
| f030 | en | negative | a dog running on the grass | negative | 0.058 | lot-walking 24.67-29.67 s |
| f031 | ko | negative | 눈이 내리는 거리 | negative | 0.117 | lot-whitecar 18.64-30.16 s |
| f032 | en | negative | a bus stopped at a bus stop | negative | 0.062 | lot-walking 44.67-49.67 s |
| f033 | ko | negative | 마트 진열대 사이 통로 | negative | 0.062 | lot-whitecar 28.64-30.16 s |
| f034 | en | negative | people waiting on a subway platform | negative | 0.132 | people-walking 0.68-13.64 s |
| f035 | ko | negative | 밤에 촬영된 도로 | negative | 0.112 | lot-whitecar 19.68-30.16 s |
| f036 | en | negative | a person lying on the floor | negative | 0.112 | lot-walking 1.67-9.67 s |
