import sys
import time
import unittest

from fovea_worker.clock import mono_ns


class ClockTest(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "darwin", "CLOCK_MONOTONIC_RAW choice is macOS specific")
    def test_darwin_reads_clock_monotonic_raw_like_libcxx_steady_clock(self):
        before = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        now = mono_ns()
        after = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        self.assertLessEqual(before, now)
        self.assertLessEqual(now, after)

    @unittest.skipIf(sys.platform == "darwin", "other platforms use time.monotonic_ns")
    def test_other_platforms_read_monotonic_ns(self):
        before = time.monotonic_ns()
        now = mono_ns()
        after = time.monotonic_ns()
        self.assertLessEqual(before, now)
        self.assertLessEqual(now, after)


if __name__ == "__main__":
    unittest.main()
