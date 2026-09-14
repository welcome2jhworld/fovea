# Search evaluation: siglip2-b16-224

Question set `questions.tune.json` (split tune): 17 answered, 0 skipped (video not provided). Index version `b63a3a358815`, google/siglip2-base-patch16-224 at revision `75de2d55ec2d`, 768 dims, one sample per 1000 ms. Search limit 50, min_gap_ms 3000, candidate_fraction 0.3; a hit overlaps ground truth by at least 1 s. Percentiles are interpolated between the neighbouring values, so the median of an even count is the midpoint of the two middle ones. Relevance is embedding similarity for ranking only, not verified. Platform darwin arm64.

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
| headline (not ambiguous) | 12 | 0.833 | 1.000 | 1.000 | 0.250 | 0.125 | 0.709 / 0.084 | 10.780 / 7.880 |
| ko | 5 | 0.600 | 1.000 | 1.000 | 0.280 | 0.140 | 0.717 / 3.916 | 9.120 / 9.747 |
| en | 7 | 1.000 | 1.000 | 1.000 | 0.229 | 0.114 | 0.700 / 0.001 | 13.020 / 2.528 |
| action | 3 | 0.667 | 1.000 | 1.000 | 0.267 | 0.133 | 5.800 / 8.167 | 21.400 / 10.273 |
| attribute | 2 | 0.500 | 1.000 | 1.000 | 0.200 | 0.100 | 0.690 / 0.340 | 0.698 / 0.612 |
| count | 2 | 1.000 | 1.000 | 1.000 | 0.300 | 0.150 | 2.756 / 0.084 | 4.418 / 0.150 |
| presence | 2 | 1.000 | 1.000 | 1.000 | 0.300 | 0.150 | 6.025 / 1.958 | 10.271 / 3.525 |
| scene | 3 | 1.000 | 1.000 | 1.000 | 0.200 | 0.100 | 0.700 / 0.000 | 0.714 / 0.001 |
| ambiguous | 1 | 1.000 | 1.000 | 1.000 | 0.200 | 0.100 | 0.833 / 4.416 | 0.833 / 4.416 |

Precision@K divides by K, and a query returned 7.59 ranges on average (13 at most), so P@10 is bounded by a tenth of the ranges returned rather than by how many of them are right.

Misses in the headline set. Not retrieved means no overlapping range among those returned, with limit 50 and the candidates capped at candidate_fraction of the samples scanned:

| K | not retrieved | ranked low |
| --- | --- | --- |
| @1 | 0 | 2 |
| @5 | 0 | 0 |
| @10 | 0 | 0 |

## Negative questions

4 negatives. Top relevance min / median / max: 0.089 / 0.118 / 0.130; positives (headline): 0.081 / 0.134 / 0.205. Negatives whose top relevance is above the median positive top relevance: 0.000.

## Latency and index cost

| Latency ms | p50 | p95 |
| --- | --- | --- |
| embed | 16.000 | 28.600 |
| scan | 1.000 | 1.600 |
| total | 18.000 | 31.400 |
| client_wall | 19.300 | 32.720 |

Indexed 152.53 s of footage (152 samples) with 9.6 s of compute: 226.500 compute s per footage hour. Stored per footage hour: vectors 11080438 B, thumbnails 44117642 B, row payload 1650246 B (column bytes, without SQLite page and index overhead).

## Questions

| id | lang | category | query | first hit rank | top relevance | top result |
| --- | --- | --- | --- | --- | --- | --- |
| t001 | en | scene | a meeting room with blue chairs and white desks | 1 | 0.132 | classroom 0.7-32.8 s |
| t002 | en | scene | snow-covered mountains on a sunny day | 1 | 0.102 | skiing 0.68-14.12 s |
| t003 | ko | scene | 노천 카페 파라솔이 있는 광장 | 1 | 0.114 | market-square 0.72-7.9 s |
| t004 | en | count | a crowd of people crossing a wide plaza | 1 | 0.168 | people-walking 0.68-13.64 s |
| t005 | ko | presence | 주차장에 있는 사람 | 1 | 0.163 | lot-walking 28.67-53.92 s |
| t006 | en | presence | a bicycle in a crowded square | 1 | 0.156 | market-square 0.72-7.9 s |
| t007 | ko | attribute | 안경을 쓰고 남색 티셔츠를 입은 남자 | 2 | 0.081 | lot-walking 1.67-7.67 s |
| t008 | en | attribute | a white sports car | 1 | 0.114 | lot-whitecar 25.68-28.68 s |
| t009 | ko | attribute (ambiguous) | 민트색 자동차 | 1 | 0.130 | lot-walking 42.67-53.92 s |
| t010 | ko | action | 주차장을 가로질러 걸어가는 사람 | 1 | 0.177 | lot-walking 0.67-15.67 s |
| t011 | en | count | a cyclist and a car in the same parking lot | 1 | 0.205 | lot-walking 39.67-49.67 s |
| t012 | ko | action | 뒷줄에 앉아 있던 사람이 자리에서 일어나는 장면 | 4 | 0.098 | market-square 0.72-7.9 s |
| t013 | en | action | a person in the back row sits back down | 1 | 0.136 | classroom 0.7-32.8 s |
| t014 | ko | negative | 트럭 | negative | 0.089 | lot-whitecar 0.64-30.16 s |
| t015 | en | negative | a person riding a motorcycle | negative | 0.123 | lot-walking 19.67-31.67 s |
| t016 | en | negative | an empty classroom with nobody in it | negative | 0.113 | classroom 0.7-32.8 s |
| t017 | ko | negative | 비 오는 날 우산을 쓰고 걷는 사람 | negative | 0.130 | lot-walking 40.67-44.67 s |
