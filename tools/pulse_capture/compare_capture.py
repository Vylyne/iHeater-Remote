#!/usr/bin/env python3
"""Compare iHeater pulse-train captures exported from Saleae Logic 2.

  compare_capture.py BASELINE.csv CANDIDATE.csv
      The candidate must match the baseline: the same pulse count per frame,
      every HIGH and LOW within 10 us of the baseline's mean, every frame
      period within 5 ms of the baseline's mean, and every gap between frames
      at least 122 ms.

  compare_capture.py --codes 10,60 CAPTURE.csv
      For a capture whose count changes: every count is one of CODES, every
      width is within 10 us of 2000.2 us and every gap is at least 122 ms.
      There is no period check, since a changed setting sends a frame early.

Input: Logic 2's raw-data CSV export, a header row and then one
"time_seconds,level" row per transition. The first and last frames are
dropped, since the capture may have cut them. Exit status 0 means PASS.
"""

import argparse
import csv
import statistics
import sys
from dataclasses import dataclass, field

FRAME_GAP_S = 0.050      # a LOW longer than this ends a frame
WIDTH_TOL_US = 10.0
PERIOD_TOL_MS = 5.0
MIN_GAP_MS = 122.0       # the 120 ms SYNC plus the last 2 ms LOW
NOMINAL_WIDTH_US = 2000.2
MIN_FRAMES = 3


@dataclass
class Frame:
    start: float
    highs_us: list = field(default_factory=list)
    lows_us: list = field(default_factory=list)
    gap_ms: float = None
    period_ms: float = None


@dataclass
class Analysis:
    counts: list
    highs_us: list
    lows_us: list
    periods_ms: list
    gaps_ms: list
    frames: list


def load_capture(path):
    with open(path, newline='') as f:
        rows = list(csv.reader(f))
    out = []
    for row in rows[1:]:
        if len(row) < 2:
            continue
        out.append((float(row[0]), int(float(row[1]))))
    return out


def analyse(transitions):
    frames = []
    cur = None
    for (t0, level), (t1, _) in zip(transitions, transitions[1:]):
        dur = t1 - t0
        if level:
            if cur is None:
                cur = Frame(start=t0)
            cur.highs_us.append(dur * 1e6)
        elif cur is not None:
            if dur > FRAME_GAP_S:
                cur.gap_ms = dur * 1e3
                frames.append(cur)
                cur = None
            else:
                cur.lows_us.append(dur * 1e6)
    if cur is not None:
        frames.append(cur)
    for a, b in zip(frames, frames[1:]):
        a.period_ms = (b.start - a.start) * 1e3
    kept = frames[1:-1]
    return Analysis(
        counts=[len(f.highs_us) for f in kept],
        highs_us=[h for f in kept for h in f.highs_us],
        lows_us=[w for f in kept for w in f.lows_us],
        periods_ms=[f.period_ms for f in kept],
        gaps_ms=[f.gap_ms for f in kept],
        frames=kept)


def _width_reasons(i, f, mean_high, mean_low):
    reasons = []
    for kind, values, mean in (('HIGH', f.highs_us, mean_high),
                               ('LOW', f.lows_us, mean_low)):
        if not values or mean is None:
            continue
        worst = max(values, key=lambda v: abs(v - mean))
        if abs(worst - mean) > WIDTH_TOL_US:
            reasons.append('frame %d: %s %.1f us (expected %.1f +/- %.0f)'
                           % (i, kind, worst, mean, WIDTH_TOL_US))
    return reasons


def _gap_reason(i, f):
    if f.gap_ms is not None and f.gap_ms < MIN_GAP_MS:
        return ['frame %d: gap %.1f ms (minimum %.0f)'
                % (i, f.gap_ms, MIN_GAP_MS)]
    return []


def _enough(label, a):
    if len(a.frames) < MIN_FRAMES:
        return ['frame -: %s has %d complete frames (need %d)'
                % (label, len(a.frames), MIN_FRAMES)]
    return []


def compare(baseline, candidate):
    reasons = _enough('baseline', baseline) + _enough('candidate', candidate)
    if reasons:
        return reasons
    base_counts = set(baseline.counts)
    if len(base_counts) != 1:
        return ['frame -: baseline count changes (%s)'
                % ', '.join(map(str, sorted(base_counts)))]
    expected = next(iter(base_counts))
    mean_high = statistics.mean(baseline.highs_us)
    mean_low = (statistics.mean(baseline.lows_us)
                if baseline.lows_us else None)
    mean_period = statistics.mean(baseline.periods_ms)
    for i, f in enumerate(candidate.frames):
        if len(f.highs_us) != expected:
            reasons.append('frame %d: count %d (baseline %d)'
                           % (i, len(f.highs_us), expected))
        reasons += _width_reasons(i, f, mean_high, mean_low)
        if abs(f.period_ms - mean_period) > PERIOD_TOL_MS:
            reasons.append('frame %d: period %.1f ms (baseline %.1f +/- %.0f)'
                           % (i, f.period_ms, mean_period, PERIOD_TOL_MS))
        reasons += _gap_reason(i, f)
    return reasons


def check_codes(analysis, codes):
    reasons = _enough('capture', analysis)
    if reasons:
        return reasons
    allowed = ', '.join(map(str, sorted(codes)))
    for i, f in enumerate(analysis.frames):
        if len(f.highs_us) not in codes:
            reasons.append('frame %d: count %d (expected one of %s)'
                           % (i, len(f.highs_us), allowed))
        reasons += _width_reasons(i, f, NOMINAL_WIDTH_US, NOMINAL_WIDTH_US)
        reasons += _gap_reason(i, f)
    return reasons


def _span(values, fmt):
    if not values:
        return '-'
    return (fmt + '-' + fmt) % (min(values), max(values))


def summary(label, a):
    return ('%s: %d frames, counts {%s}, HIGH %s us, LOW %s us, '
            'period %s ms, min gap %s ms'
            % (label, len(a.frames),
               ', '.join(map(str, sorted(set(a.counts)))),
               _span(a.highs_us, '%.1f'), _span(a.lows_us, '%.1f'),
               _span(a.periods_ms, '%.1f'),
               '%.1f' % min(a.gaps_ms) if a.gaps_ms else '-'))


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__.split('\n\n')[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--codes',
                   help='comma-separated allowed counts; takes one capture')
    p.add_argument('captures', nargs='+')
    args = p.parse_args(argv)
    if args.codes:
        if len(args.captures) != 1:
            p.error('--codes takes exactly one capture')
        codes = {int(c) for c in args.codes.split(',') if c.strip()}
        a = analyse(load_capture(args.captures[0]))
        print(summary(args.captures[0], a))
        reasons = check_codes(a, codes)
    else:
        if len(args.captures) != 2:
            p.error('give BASELINE and CANDIDATE, or --codes and one capture')
        base = analyse(load_capture(args.captures[0]))
        cand = analyse(load_capture(args.captures[1]))
        print(summary('baseline  ' + args.captures[0], base))
        print(summary('candidate ' + args.captures[1], cand))
        reasons = compare(base, cand)
    if reasons:
        print('FAIL: ' + '; '.join(reasons))
        return 1
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
