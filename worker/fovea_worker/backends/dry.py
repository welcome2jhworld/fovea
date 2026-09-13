"""Contract-only backend. Validates inputs, loads no model, produces no observations.

It exists so the core <-> worker protocol can be exercised without weights.
Its output is marked status="dry_run" and must never be shown as analysis.
"""
from __future__ import annotations

import time

from ..protocol import Job, Result


class DryBackend:
    name = "dry"
    version = "0"

    def load(self) -> None:
        return None

    def run(self, job: Job) -> Result:
        t0 = time.monotonic()
        missing = [f.path for f in job.frames if not f.path]
        status = "dry_run" if not missing else "error"
        return Result(
            job_id=job.job_id,
            generation=job.generation,
            status=status,
            observations=[],
            model=self.name,
            model_version=self.version,
            input_frames=len(job.frames),
            processing_ms=int((time.monotonic() - t0) * 1000),
            error="" if not missing else f"{len(missing)} frame refs without path",
        )
