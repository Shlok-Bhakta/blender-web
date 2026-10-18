# SPDX-License-Identifier: GPL-2.0-or-later
"""Timing for browser add-ons when process CPU clocks are unavailable."""
import time as _time

try:
    _time.process_time()
    _elapsed_clock = _time.process_time
except (OSError, RuntimeError):
    # Browser elapsed-time statistics use a monotonic clock. This is not a
    # measurement of CPU utilization, which the browser does not expose here.
    _elapsed_clock = _time.perf_counter


def process_time():
    return _elapsed_clock()


def __getattr__(name):
    return getattr(_time, name)
