"""Monotonic clock that matches C++ std::chrono::steady_clock in fovea-core.

- macOS: libc++ steady_clock reads CLOCK_MONOTONIC_RAW (includes sleep) while
  time.monotonic reads CLOCK_UPTIME_RAW (excludes sleep).
- Windows: MSVC steady_clock scales QueryPerformanceCounter; time.perf_counter_ns
  does the same, while time.monotonic may use GetTickCount64.
- Linux: both read CLOCK_MONOTONIC.
"""
from __future__ import annotations

import sys
import time

if sys.platform == "darwin":
    def mono_ns() -> int:
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
elif sys.platform == "win32":
    def mono_ns() -> int:
        return time.perf_counter_ns()
else:
    def mono_ns() -> int:
        return time.monotonic_ns()
