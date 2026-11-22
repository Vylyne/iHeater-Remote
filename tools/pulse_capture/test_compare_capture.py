import os
import subprocess
import sys
import tempfile
import unittest

import compare_capture as cc

HERE = os.path.dirname(os.path.abspath(__file__))


def make_capture(counts, high_us=2000.2, low_us=2000.2, period_ms=1000.1,
                 gap_ms=None, lead_partial=0):
    """Transitions for frames of `counts` pulses, as Logic 2 would export.

    counts lists every frame, the first and last included (the analysis
    drops those two). gap_ms, when given, is the LOW from a frame's last
    falling edge to the next frame's first rising edge, and the period
    follows from it. lead_partial adds that many pulses before the first
    frame, as if the capture started mid-frame.
    """
    high = high_us / 1e6
    low = low_us / 1e6
    out = [(0.0, 0)]
    t = 0.05

    def frame(start, n):
        tt = start
        for _ in range(n):
            out.append((tt, 1))
            tt += high
            out.append((tt, 0))
            tt += low
        return tt - low  # the last falling edge

    if lead_partial:
        frame(t, lead_partial)
        t += period_ms / 1e3
    for n in counts:
        last_fall = frame(t, n)
        if gap_ms is not None:
            t = last_fall + gap_ms / 1e3
        else:
            t += period_ms / 1e3
    out.append((t, 0))  # the capture ends during the idle low
    return out


def write_csv(path, transitions):
    with open(path, 'w', newline='') as f:
        f.write('Time [s],Channel 0\n')
        for t, v in transitions:
            f.write('%.9f,%d\n' % (t, v))


def analysed(**kw):
    counts = kw.pop('counts')
    return cc.analyse(make_capture(counts, **kw))


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.base = analysed(counts=[60] * 6)

    def test_identical_passes(self):
        self.assertEqual(cc.compare(self.base, analysed(counts=[60] * 6)), [])

    def test_width_within_tolerance_passes(self):
        cand = analysed(counts=[60] * 6, high_us=2009.0)
        self.assertEqual(cc.compare(self.base, cand), [])

    def test_wide_pulse_fails(self):
        cand = analysed(counts=[60] * 6, high_us=2011.0)
        reasons = cc.compare(self.base, cand)
        self.assertTrue(any('HIGH' in r for r in reasons), reasons)

    def test_period_off_fails(self):
        cand = analysed(counts=[60] * 6, period_ms=1006.2)
        reasons = cc.compare(self.base, cand)
        self.assertTrue(any('period' in r for r in reasons), reasons)

    def test_count_differs_fails(self):
        cand = analysed(counts=[59] * 6)
        reasons = cc.compare(self.base, cand)
        self.assertTrue(any('count' in r for r in reasons), reasons)

    def test_short_gap_fails(self):
        cand = analysed(counts=[60] * 6, gap_ms=100)
        reasons = cc.compare(self.base, cand)
        self.assertTrue(any('gap' in r for r in reasons), reasons)

    def test_partial_first_frame_dropped(self):
        cand = analysed(counts=[60] * 6, lead_partial=3)
        self.assertEqual(cand.counts, [60] * 5)
        self.assertEqual(cc.compare(self.base, cand), [])

    def test_too_few_frames_fails(self):
        cand = analysed(counts=[60] * 3)
        self.assertEqual(len(cand.counts), 1)
        self.assertNotEqual(cc.compare(self.base, cand), [])

    def test_reason_names_frame(self):
        cand = analysed(counts=[60] * 6, high_us=2011.0)
        reasons = cc.compare(self.base, cand)
        self.assertTrue(all(r.startswith('frame ') for r in reasons), reasons)


class CheckCodesTest(unittest.TestCase):
    def test_check_codes_switching_passes(self):
        a = analysed(counts=[10, 10, 60, 60, 10, 10])
        self.assertEqual(cc.check_codes(a, {10, 60}), [])

    def test_check_codes_stray_pulse_fails(self):
        a = analysed(counts=[10, 10, 1, 10, 10])
        reasons = cc.check_codes(a, {10, 60})
        self.assertTrue(any('count 1 ' in r for r in reasons), reasons)

    def test_check_codes_wide_pulse_fails(self):
        a = analysed(counts=[10] * 5, low_us=2011.0)
        reasons = cc.check_codes(a, {10})
        self.assertTrue(any('LOW' in r for r in reasons), reasons)


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, os.path.join(HERE, 'compare_capture.py')]
            + list(args), capture_output=True, text=True)

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            a = os.path.join(d, 'a.csv')
            b = os.path.join(d, 'b.csv')
            c = os.path.join(d, 'c.csv')
            write_csv(a, make_capture([60] * 6))
            write_csv(b, make_capture([60] * 6))
            write_csv(c, make_capture([60] * 6, high_us=2050.0))
            ok = self.run_cli(a, b)
            self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
            self.assertIn('PASS', ok.stdout)
            bad = self.run_cli(a, c)
            self.assertEqual(bad.returncode, 1, bad.stdout + bad.stderr)
            self.assertIn('FAIL', bad.stdout)
            codes = self.run_cli('--codes', '60', a)
            self.assertEqual(codes.returncode, 0, codes.stdout + codes.stderr)

    def test_load_capture_reads_export(self):
        with tempfile.TemporaryDirectory() as d:
            a = os.path.join(d, 'a.csv')
            write_csv(a, make_capture([10] * 4))
            tr = cc.load_capture(a)
            self.assertEqual(tr[0], (0.0, 0))
            self.assertEqual(tr[1][1], 1)


if __name__ == '__main__':
    unittest.main()
