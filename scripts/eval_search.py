#!/usr/bin/env python3
"""Search evaluation over the real pipeline (stdlib only), docs/M4_DESIGN.md
"Evaluation".

The corpus videos are passed by path (--video, repeated); each file's sha256
is matched against the question set's manifest. The script starts fovea-core
with the real model worker, imports every matched video into its own camera
(code EVAL-<sha256 prefix>) through POST /v1/imports, makes
--index-version-name the active index version, waits until every corpus
camera is fully indexed, runs every question as POST /v1/search over all
corpus cameras with that version, and scores the ranked ranges:

  - Recall@1/5/10: a positive question is a hit at K when one of the top K
    ranges overlaps a ground-truth range (video_sha256 plus also_matches) by
    at least 1 s (or all of a ground-truth range shorter than 1 s)
  - Precision@5/10: overlapping ranges among the top K, divided by K
  - temporal error of the best-ranked overlapping range against the
    ground-truth range it overlaps most: |start - gt start| and |end - gt end|
  - latency: embed, scan and total ms reported by the core, p50 and p95
  - index cost: compute seconds and stored bytes (vectors, thumbnails, row
    payload) per hour of footage, from GET /v1/index?storage=1
  - negatives: top relevance of negative against positive questions, and the
    fraction of negatives whose top relevance exceeds the median positive one
  - misses at K split into "not retrieved" (no overlapping range in the top
    --limit, default 50) and "ranked low" (first overlap beyond K)
Ambiguous questions are reported separately and excluded from the headline.
Questions whose video was not provided are skipped; also_matches entries of
missing videos are dropped. Relevance is a ranking score, not a probability.

A --work-dir that already holds the corpus cameras (for example from a run
with the other index version) is reused without importing again, so both
versions are scored on the same footage and question set.

Usage:
  python3 scripts/eval_search.py --bin-dir build/macos-dev --questions eval/search/questions.json \\
      --video a.mp4 --video b.mp4 --index-version-name siglip2-b16-224 --work-dir /tmp/fovea-eval
Writes docs/verification/search-eval-<version>-<split>-cf<candidate fraction>-<UTC stamp>.{json,md}.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

EXE = ".exe" if os.name == "nt" else ""
REPO = Path(__file__).resolve().parent.parent
HOUR_MS = 3_600_000
KS = (1, 5, 10)
PRECISION_KS = (5, 10)
MIN_OVERLAP_MS = 1000


class Fail(Exception):
    pass


def utc_ms() -> int:
    return int(time.time() * 1000)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def percentile(values: list[float], q: float) -> float | None:
    """The q-th percentile, interpolated between the neighbouring order statistics (q=50 is the median)."""
    if not values:
        return None
    ordered = sorted(values)
    position = q / 100 * (len(ordered) - 1)
    lower = int(math.floor(position))
    upper = min(lower + 1, len(ordered) - 1)
    value = ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)
    return round(value, 4)


def ratio(numerator: float, denominator: float) -> float | None:
    return round(numerator / denominator, 4) if denominator else None


def overlap_ms(a_start: int, a_end: int, b_start: int, b_end: int) -> int:
    return min(a_end, b_end) - max(a_start, b_start)


class Evaluation:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        bin_dir = Path(args.bin_dir).resolve()
        self.core = next((p for p in (bin_dir / "src" / "core" / f"fovea-core{EXE}", bin_dir / f"fovea-core{EXE}") if p.is_file()),
                         bin_dir / f"fovea-core{EXE}")
        self.work = Path(args.work_dir).resolve() if args.work_dir else Path(tempfile.gettempdir()) / "fovea-eval-search"
        self.data = self.work / "data"
        self.questions_path = Path(args.questions).resolve()
        self.set = json.loads(self.questions_path.read_text(encoding="utf-8"))
        self.manifest = {v["sha256"]: v for v in self.set["videos"]}
        self.core_proc: subprocess.Popen | None = None
        self.api_port = 0
        self.token = ""
        self.videos: dict[str, dict] = {}
        self.camera_video: dict[str, str] = {}
        self.log_lines: list[str] = []

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        self.log_lines.append(line)

    def request(self, method: str, path: str, body: dict | None = None, timeout: float = 60.0) -> tuple[int, dict | list | None]:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.api_port}{path}", data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
                return r.status, json.loads(payload) if payload else None
        except urllib.error.HTTPError as e:
            payload = e.read()
            try:
                return e.code, json.loads(payload)
            except ValueError:
                return e.code, {"error": {"message": payload[:200].decode(errors="replace")}}
        except (urllib.error.URLError, OSError) as e:
            raise Fail(f"{method} {path} -> {e}") from e

    def api(self, method: str, path: str, body: dict | None = None, timeout: float = 60.0):
        status, payload = self.request(method, path, body, timeout)
        if status >= 300:
            raise Fail(f"{method} {path} -> {status} {payload}")
        return payload

    def wait(self, what: str, timeout: float, probe, interval: float = 1.0):
        deadline = time.monotonic() + timeout
        last_error = ""
        while time.monotonic() < deadline:
            try:
                value = probe()
                if value:
                    return value
            except Fail as e:
                last_error = str(e)
            time.sleep(interval)
        raise Fail(f"timeout after {timeout:.0f} s waiting for {what}" + (f" ({last_error})" if last_error else ""))

    # core

    def worker_cmd(self) -> str:
        if self.args.worker_cmd:
            return self.args.worker_cmd
        python = REPO / "worker" / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        return f'"{python}" -m fovea_worker.cli'

    def start_core(self) -> None:
        if not self.core.is_file():
            raise Fail(f"missing {self.core.name} under --bin-dir")
        self.data.mkdir(parents=True, exist_ok=True)
        info = self.data / "core.json"
        if info.exists():
            info.unlink()
        env = dict(os.environ, FOVEA_WORKER_CMD=self.worker_cmd(), FOVEA_MIN_FREE_MB=str(self.args.min_free_mb))
        env.pop("FOVEA_RETENTION_SECONDS", None)
        out = open(self.work / "core.log", "ab")
        self.core_proc = subprocess.Popen([str(self.core), "--data-dir", str(self.data), "--port", "0"], stdout=out,
                                          stderr=subprocess.STDOUT, env=env)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if info.exists():
                try:
                    self.api_port = int(json.loads(info.read_text())["port"])
                    self.token = (self.data / "core.token").read_text().strip()
                    return
                except (ValueError, KeyError, json.JSONDecodeError):
                    pass
            if self.core_proc.poll() is not None:
                raise Fail(f"fovea-core exited with {self.core_proc.returncode}; see core.log in the work dir")
            time.sleep(0.1)
        raise Fail("core.json not written within 20 s")

    def stop_core(self) -> None:
        if not self.core_proc or self.core_proc.poll() is not None:
            return
        try:
            self.api("POST", "/v1/service/shutdown")
            self.core_proc.wait(60)
        except (Fail, subprocess.TimeoutExpired):
            self.core_proc.kill()

    # corpus

    def match_videos(self) -> None:
        for arg in self.args.video:
            path = Path(arg)
            if not path.is_file():
                raise Fail(f"no video file at --video #{self.args.video.index(arg) + 1}")
            digest = sha256_of(path)
            if digest not in self.manifest:
                self.log(f"--video #{self.args.video.index(arg) + 1} (sha256 {digest[:12]}) is not in the manifest; ignored")
                continue
            self.videos[digest] = {"path": path.resolve(), "name": self.manifest[digest]["name"]}
        if not self.videos:
            raise Fail("no --video matches the question set manifest")
        self.log("corpus: " + ", ".join(sorted(v["name"] for v in self.videos.values())))

    def import_corpus(self) -> None:
        cameras = {c["code"]: c for c in self.api("GET", "/v1/cameras")}
        imports = {r["camera_id"]: r for r in self.api("GET", "/v1/imports?limit=1000") if r["state"] == "done"}
        base = utc_ms() - 12 * HOUR_MS
        pending = {}
        for i, (digest, video) in enumerate(sorted(self.videos.items(), key=lambda kv: kv[1]["name"])):
            code = f"EVAL-{digest[:8]}"
            cam = cameras.get(code) or self.api("POST", "/v1/cameras", {
                "code": code, "name": f"eval {video['name']}", "group_name": "eval", "kind": "file", "main_url": video["path"].name,
                "enabled": False, "segment_seconds": self.args.segment_seconds})
            video["camera_id"] = cam["id"]
            self.camera_video[cam["id"]] = digest
            if cam["id"] in imports:
                video["start_utc_ms"] = imports[cam["id"]]["start_utc_ms"]
                video["import"] = imports[cam["id"]]
                continue
            start = base + i * HOUR_MS
            record = self.api("POST", "/v1/imports", {"path": str(video["path"]), "camera_id": cam["id"], "start_utc_ms": start}, timeout=120)
            video["start_utc_ms"] = start
            pending[digest] = record["id"]
        for digest, import_id in pending.items():
            record = self.wait(f"import of {self.videos[digest]['name']}", 600,
                               lambda i=import_id: (lambda r: r if r["state"] in ("done", "failed") else None)(self.api("GET", f"/v1/imports/{i}")))
            if record["state"] != "done":
                raise Fail(f"import of {self.videos[digest]['name']} failed: {record['error']}")
            self.videos[digest]["import"] = record
        self.log("imports: " + ", ".join(f"{v['name']} {v['import']['segments']} segments" for v in self.videos.values()))

    def index_version(self) -> dict:
        name = self.args.index_version_name
        status = self.api("GET", "/v1/index")
        if name not in status["known_index_version_names"]:
            raise Fail(f"unknown index version {name}; the core knows {status['known_index_version_names']}")
        if status["active_index_version"] != name:
            self.api("PUT", "/v1/index/active", {"index_version_name": name})
            self.log(f"active index version set to {name}")
        cameras = {v["camera_id"] for v in self.videos.values()}
        started = time.monotonic()

        def covered() -> dict | None:
            s = self.api("GET", "/v1/index")
            if s["active_index_version"] != name or not s["index_version"]:
                return None
            if s["queue"]["failed"]:
                raise Fail(f"index jobs failed: {s['recent_failures'][:1]}")
            cams = {c["camera_id"]: c for c in s["cameras"] if c["camera_id"] in cameras}
            if len(cams) == len(cameras) and all(c["frames_expected"] > 0 and c["coverage_ratio"] >= 1.0 for c in cams.values()) \
                    and not s["queue"]["queued"] and not s["queue"]["running"] and s["state"] == "idle":
                return s
            return None

        self.wait(f"full coverage of the corpus by {name}", self.args.index_timeout, covered, interval=2.0)
        self.log(f"corpus indexed by {name} after {time.monotonic() - started:.0f} s")
        return self.api("GET", "/v1/index?storage=1")

    # scoring

    def ground_truth(self, q: dict) -> list[tuple[str, int, int]]:
        spans = []
        entries = [{"video_sha256": q["video_sha256"], "ranges": q["ranges"]}] + list(q.get("also_matches") or [])
        for entry in entries:
            video = self.videos.get(entry["video_sha256"])
            if not video:
                continue
            for r in entry["ranges"]:
                spans.append((video["camera_id"], video["start_utc_ms"] + round(r["start_s"] * 1000),
                              video["start_utc_ms"] + round(r["end_s"] * 1000)))
        return spans

    @staticmethod
    def overlapping(result: dict, spans: list[tuple[str, int, int]]) -> tuple[str, int, int] | None:
        best, best_overlap = None, 0
        for cam, start, end in spans:
            if cam != result["camera_id"]:
                continue
            o = overlap_ms(result["start_utc_ms"], result["end_utc_ms"], start, end)
            if o >= min(MIN_OVERLAP_MS, end - start) and o > best_overlap:
                best, best_overlap = (cam, start, end), o
        return best

    def run_questions(self) -> list[dict]:
        name = self.args.index_version_name
        rows = []
        for q in self.set["questions"]:
            row = {"id": q["id"], "query": q["query"], "language": q["language"], "category": q.get("category", ""),
                   "negative": bool(q["negative"]), "ambiguous": bool(q.get("ambiguous")), "pair": q.get("pair")}
            if not q["negative"] and q["video_sha256"] not in self.videos:
                row["skipped"] = "video not provided"
                rows.append(row)
                continue
            spans = [] if q["negative"] else self.ground_truth(q)
            t0 = time.monotonic()
            body = {"query": q["query"], "limit": self.args.limit, "min_gap_ms": self.args.min_gap_ms, "index_version": name}
            if self.args.candidate_fraction is not None:
                body["candidate_fraction"] = self.args.candidate_fraction
            response = self.api("POST", "/v1/search", body, timeout=120)
            row["candidate_fraction"] = response["filters"]["candidate_fraction"]
            row["wall_ms"] = round((time.monotonic() - t0) * 1000, 1)
            if response["index_version_name"] != name:
                raise Fail(f"question {q['id']} was answered by {response['index_version_name']}")
            results = response["results"]
            row["stats"] = {k: response["stats"][k] for k in ("embed_ms", "scan_ms", "total_ms", "coverage_ratio", "samples_scanned")}
            row["top_relevance"] = results[0]["relevance"] if results else None
            row["results"] = len(results)
            row["top"] = [self.describe(r) for r in results[:5]]
            if not q["negative"]:
                matches = [self.overlapping(r, spans) for r in results]
                ranks = [i + 1 for i, m in enumerate(matches) if m]
                row["first_hit_rank"] = ranks[0] if ranks else None
                for k in KS:
                    row[f"hit@{k}"] = bool(ranks) and ranks[0] <= k
                for k in PRECISION_KS:
                    row[f"precision@{k}"] = sum(1 for m in matches[:k] if m) / k
                if ranks:
                    best = results[ranks[0] - 1]
                    _, gt_start, gt_end = matches[ranks[0] - 1]
                    row["start_error_s"] = abs(best["start_utc_ms"] - gt_start) / 1000
                    row["end_error_s"] = abs(best["end_utc_ms"] - gt_end) / 1000
            rows.append(row)
            self.log(f"{q['id']} [{q['language']}] {'NEG' if q['negative'] else 'rank ' + str(row.get('first_hit_rank'))} "
                     f"top {row['top_relevance']} {row['stats']['total_ms']} ms: {q['query']}")
        return rows

    def describe(self, result: dict) -> dict:
        digest = self.camera_video.get(result["camera_id"], "")
        video = self.videos.get(digest)
        start = video["start_utc_ms"] if video else 0
        return {"video": video["name"] if video else "?", "start_s": round((result["start_utc_ms"] - start) / 1000, 2),
                "end_s": round((result["end_utc_ms"] - start) / 1000, 2), "relevance": result["relevance"],
                "samples": len(result["samples"])}

    @staticmethod
    def summarize(rows: list[dict], limit: int) -> dict:
        positives = [r for r in rows if not r["negative"] and "skipped" not in r]
        out: dict = {"questions": len(positives)}
        if not positives:
            return out
        for k in KS:
            out[f"recall@{k}"] = ratio(sum(1 for r in positives if r[f"hit@{k}"]), len(positives))
        for k in PRECISION_KS:
            out[f"precision@{k}"] = ratio(sum(r[f"precision@{k}"] for r in positives), len(positives))
        starts = [r["start_error_s"] for r in positives if "start_error_s" in r]
        ends = [r["end_error_s"] for r in positives if "end_error_s" in r]
        out["temporal_error_s"] = {"start_median": percentile(starts, 50), "start_p90": percentile(starts, 90),
                                   "end_median": percentile(ends, 50), "end_p90": percentile(ends, 90), "n": len(starts)}
        misses = {}
        for k in KS:
            missed = [r for r in positives if not r[f"hit@{k}"]]
            misses[f"@{k}"] = {"not_retrieved": sum(1 for r in missed if r["first_hit_rank"] is None),
                               "ranked_low": sum(1 for r in missed if r["first_hit_rank"] is not None)}
        out["misses"] = misses
        out["not_retrieved_means"] = (f"no overlapping range among those returned, with limit {limit} and the candidates "
                                      f"capped at candidate_fraction of the samples scanned")
        return out

    def report(self, rows: list[dict], status: dict, started_utc: int) -> dict:
        name = self.args.index_version_name
        version = next((v for v in status["versions"] if v["hash"] == status["index_version"]), {})
        answered = [r for r in rows if "skipped" not in r]
        headline = [r for r in answered if not r["ambiguous"]]
        positives = [r for r in headline if not r["negative"]]
        negatives = [r for r in answered if r["negative"]]
        positive_tops = [r["top_relevance"] for r in positives if r["top_relevance"] is not None]
        negative_tops = [r["top_relevance"] for r in negatives if r["top_relevance"] is not None]
        median_positive = percentile(positive_tops, 50)
        footage_h = version.get("footage_ms", 0) / HOUR_MS
        storage = version.get("storage", {})

        def latency(key: str) -> dict:
            values = [r["stats"][key] for r in answered]
            return {"p50": percentile(values, 50), "p95": percentile(values, 95)}

        return {
            "generated_utc_ms": started_utc,
            "platform": sys.platform, "machine": platform.machine(), "cpu_count": os.cpu_count(),
            "question_set": {"file": self.questions_path.name, "split": self.set.get("split"), "questions": len(self.set["questions"]),
                             "answered": len(answered), "skipped": len(rows) - len(answered)},
            "corpus": [{"name": v["name"], "sha256": d[:12], "duration_s": self.manifest[d]["duration_s"],
                        "segments": v["import"]["segments"]} for d, v in sorted(self.videos.items(), key=lambda kv: kv[1]["name"])],
            "index_version": {"name": name, "hash": status["index_version"], "model": version.get("model_id"),
                              "model_revision": version.get("model_revision"), "dims": version.get("dims"),
                              "sample_interval_ms": version.get("sample_interval_ms"), "descriptor": version.get("descriptor")},
            "settings": {"limit": self.args.limit, "min_gap_ms": self.args.min_gap_ms, "segment_seconds": self.args.segment_seconds,
                         "candidate_fraction": answered[0]["candidate_fraction"] if answered else None, "min_overlap_ms": MIN_OVERLAP_MS},
            "returned_ranges": {"mean": round(sum(r["results"] for r in answered) / len(answered), 2) if answered else None,
                                "max": max((r["results"] for r in answered), default=0)},
            "headline": self.summarize(headline, self.args.limit),
            "by_language": {lang: self.summarize([r for r in headline if r["language"] == lang], self.args.limit) for lang in ("ko", "en")},
            "by_category": {cat: self.summarize([r for r in headline if r["category"] == cat], self.args.limit)
                            for cat in sorted({r["category"] for r in headline if not r["negative"]})},
            "ambiguous": self.summarize([r for r in answered if r["ambiguous"]], self.args.limit),
            "negatives": {"questions": len(negatives), "top_relevance": {"min": min(negative_tops, default=None),
                                                                        "median": percentile(negative_tops, 50),
                                                                        "max": max(negative_tops, default=None)},
                          "positive_top_relevance": {"min": min(positive_tops, default=None), "median": median_positive,
                                                     "max": max(positive_tops, default=None)},
                          "above_median_positive": ratio(sum(1 for t in negative_tops if median_positive is not None and t > median_positive),
                                                         len(negative_tops))},
            "latency_ms": {"embed": latency("embed_ms"), "scan": latency("scan_ms"), "total": latency("total_ms"),
                           "client_wall": {"p50": percentile([r["wall_ms"] for r in answered], 50),
                                           "p95": percentile([r["wall_ms"] for r in answered], 95)}},
            "index_cost": {"footage_s": round(version.get("footage_ms", 0) / 1000, 2), "compute_s": round(version.get("compute_ms", 0) / 1000, 2),
                           "compute_s_per_footage_hour": round(version.get("compute_ms", 0) / 1000 / footage_h, 1) if footage_h else None,
                           "vector_bytes_per_hour": round(storage.get("vector_bytes", 0) / footage_h) if footage_h else None,
                           "thumbnail_bytes_per_hour": round(storage.get("thumbnail_bytes", 0) / footage_h) if footage_h else None,
                           "row_payload_bytes_per_hour": round(storage.get("row_payload_bytes", 0) / footage_h) if footage_h else None,
                           "records": storage.get("rows"), "frames_indexed": version.get("frames_indexed")},
            "questions": rows,
        }

    @staticmethod
    def markdown(r: dict) -> str:
        def fmt(v) -> str:
            return "-" if v is None else (f"{v:.3f}" if isinstance(v, float) else str(v))

        def metric_rows(title: str, s: dict) -> str:
            if not s.get("questions"):
                return f"| {title} | 0 | - | - | - | - | - | - | - |\n"
            t = s["temporal_error_s"]
            return (f"| {title} | {s['questions']} | {fmt(s['recall@1'])} | {fmt(s['recall@5'])} | {fmt(s['recall@10'])} | "
                    f"{fmt(s['precision@5'])} | {fmt(s['precision@10'])} | {fmt(t['start_median'])} / {fmt(t['end_median'])} | "
                    f"{fmt(t['start_p90'])} / {fmt(t['end_p90'])} |\n")

        v = r["index_version"]
        lines = [f"# Search evaluation: {v['name']}\n",
                 f"Question set `{r['question_set']['file']}` (split {r['question_set']['split']}): {r['question_set']['answered']} answered, "
                 f"{r['question_set']['skipped']} skipped (video not provided). Index version `{v['hash']}`, {v['model']} at revision "
                 f"`{str(v['model_revision'])[:12]}`, {v['dims']} dims, one sample per {v['sample_interval_ms']} ms. Search limit "
                 f"{r['settings']['limit']}, min_gap_ms {r['settings']['min_gap_ms']}, candidate_fraction "
                 f"{r['settings']['candidate_fraction']}; a hit overlaps ground truth by at least 1 s. Percentiles are "
                 f"interpolated between the neighbouring values, so the median of an even count is the midpoint of the two "
                 f"middle ones. Relevance is embedding similarity for ranking only, not verified. "
                 f"Platform {r['platform']} {r['machine']}.\n",
                 f"The corpus is {sum(c['duration_s'] for c in r['corpus']):.0f} s of footage over "
                 f"{len(r['corpus'])} videos and the tune and final question sets describe the same footage, so these "
                 f"numbers compare index versions on this footage; they are not an estimate of recall in the field.\n",
                 "## Corpus\n", "| Video | sha256 | Duration s | Segments |", "| --- | --- | --- | --- |"]
        lines += [f"| {c['name']} | `{c['sha256']}` | {c['duration_s']} | {c['segments']} |" for c in r["corpus"]]
        header = ("| Set | Questions | R@1 | R@5 | R@10 | P@5 | P@10 | Temporal error median s (start / end) | p90 s (start / end) |\n"
                  "| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
        table = header + metric_rows("headline (not ambiguous)", r["headline"])
        table += "".join(metric_rows(lang, s) for lang, s in r["by_language"].items())
        table += "".join(metric_rows(cat, s) for cat, s in r["by_category"].items())
        table += metric_rows("ambiguous", r["ambiguous"])
        lines += ["", "## Retrieval\n", table]
        returned = r.get("returned_ranges", {})
        if returned.get("mean") is not None:
            lines += [f"Precision@K divides by K, and a query returned {returned['mean']} ranges on average "
                      f"({returned['max']} at most), so P@10 is bounded by a tenth of the ranges returned rather than by "
                      f"how many of them are right.\n"]
        misses = r["headline"].get("misses", {})
        if misses:
            lines += ["Misses in the headline set. Not retrieved means " + r["headline"]["not_retrieved_means"] + ":\n",
                      "| K | not retrieved | ranked low |", "| --- | --- | --- |"]
            lines += [f"| {k} | {m['not_retrieved']} | {m['ranked_low']} |" for k, m in misses.items()]
        n = r["negatives"]
        lines += ["", "## Negative questions\n",
                  f"{n['questions']} negatives. Top relevance min / median / max: {fmt(n['top_relevance']['min'])} / "
                  f"{fmt(n['top_relevance']['median'])} / {fmt(n['top_relevance']['max'])}; positives (headline): "
                  f"{fmt(n['positive_top_relevance']['min'])} / {fmt(n['positive_top_relevance']['median'])} / "
                  f"{fmt(n['positive_top_relevance']['max'])}. Negatives whose top relevance is above the median positive top relevance: "
                  f"{fmt(n['above_median_positive'])}.\n"]
        lat = r["latency_ms"]
        cost = r["index_cost"]
        lines += ["## Latency and index cost\n", "| Latency ms | p50 | p95 |", "| --- | --- | --- |"]
        lines += [f"| {k} | {fmt(lat[k]['p50'])} | {fmt(lat[k]['p95'])} |" for k in ("embed", "scan", "total", "client_wall")]
        lines += ["", f"Indexed {cost['footage_s']} s of footage ({cost['frames_indexed']} samples) with {cost['compute_s']} s of compute: "
                      f"{fmt(cost['compute_s_per_footage_hour'])} compute s per footage hour. Stored per footage hour: vectors "
                      f"{fmt(cost['vector_bytes_per_hour'])} B, thumbnails {fmt(cost['thumbnail_bytes_per_hour'])} B, row payload "
                      f"{fmt(cost['row_payload_bytes_per_hour'])} B (column bytes, without SQLite page and index overhead).\n",
                  "## Questions\n", "| id | lang | category | query | first hit rank | top relevance | top result |", "| --- | --- | --- | --- | --- | --- | --- |"]
        for q in r["questions"]:
            if "skipped" in q:
                lines.append(f"| {q['id']} | {q['language']} | {q['category']} | {q['query']} | skipped | - | - |")
                continue
            top = q["top"][0] if q["top"] else None
            rank = "negative" if q["negative"] else ("-" if q["first_hit_rank"] is None else q["first_hit_rank"])
            flag = " (ambiguous)" if q["ambiguous"] else ""
            lines.append(f"| {q['id']} | {q['language']} | {q['category']}{flag} | {q['query']} | {rank} | {fmt(q['top_relevance'])} | "
                         + (f"{top['video']} {top['start_s']}-{top['end_s']} s" if top else "-") + " |")
        return "\n".join(lines) + "\n"

    def run(self) -> dict:
        started = utc_ms()
        self.match_videos()
        self.work.mkdir(parents=True, exist_ok=True)
        self.start_core()
        try:
            self.wait("the model worker", 300, lambda: self.api("GET", "/v1/analysis")["worker"]["state"] == "ready")
            self.import_corpus()
            status = self.index_version()
            rows = self.run_questions()
            return self.report(rows, status, started)
        finally:
            self.stop_core()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin-dir", required=True)
    p.add_argument("--questions", default=str(REPO / "eval" / "search" / "questions.json"))
    p.add_argument("--video", action="append", required=True, help="corpus video file (repeat)")
    p.add_argument("--index-version-name", required=True)
    p.add_argument("--worker-cmd", help="FOVEA_WORKER_CMD for the core (default: worker/.venv python -m fovea_worker.cli)")
    p.add_argument("--work-dir", help="core data directory parent, reused across runs (default: <temp>/fovea-eval-search)")
    p.add_argument("--limit", type=int, default=50, help="ranges requested per question (also the not-retrieved horizon)")
    p.add_argument("--min-gap-ms", type=int, default=3000)
    p.add_argument("--candidate-fraction", type=float, help="search candidate_fraction (default: the core's)")
    p.add_argument("--segment-seconds", type=int, default=10)
    p.add_argument("--index-timeout", type=float, default=3600)
    p.add_argument("--min-free-mb", type=int, default=256)
    p.add_argument("--out-dir", default=str(REPO / "docs" / "verification"))
    args = p.parse_args()
    evaluation = Evaluation(args)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    try:
        result = evaluation.run()
    except Fail as e:
        evaluation.log(f"ABORT: {e}")
        return 1
    except KeyboardInterrupt:
        evaluation.stop_core()
        return 1
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    split = str(result["question_set"].get("split") or "unknown")
    fraction = result["settings"].get("candidate_fraction")
    tag = ("default" if fraction is None else f"{fraction:g}")
    # The name carries a decimal point, so the extension is appended rather than
    # set with with_suffix, which would cut the name at that point.
    name = f"search-eval-{args.index_version_name}-{split}-cf{tag}-{stamp}"
    base = out / name
    (out / f"{name}.json").write_text(json.dumps(result, indent=1, ensure_ascii=False), encoding="utf-8")
    (out / f"{name}.md").write_text(Evaluation.markdown(result), encoding="utf-8")
    h = result["headline"]
    evaluation.log(f"{args.index_version_name}: R@1 {h.get('recall@1')} R@5 {h.get('recall@5')} R@10 {h.get('recall@10')} "
                   f"P@5 {h.get('precision@5')} over {h.get('questions')} questions")
    print(f"report: {base.name}.json, {base.name}.md")
    return 0


if __name__ == "__main__":
    if os.name != "nt":
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    sys.exit(main())
