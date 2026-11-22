# Pulse capture comparison

`compare_capture.py` checks the Remote's pulse output against a logic
analyser capture. It was written for the Arduino-ESP32 3.x upgrade, to prove
the waveform did not change.

## Capturing

1. Connect the analyser to the pulse pin (GPIO3 on the C3 Super Mini) and
   ground.
2. In Saleae Logic 2, capture at least 5 s. Use 1 MS/s or faster.
3. Export with **File → Export Raw Data**, as CSV, digital channel only.
   Save it in `logic captures/` at the repo root, which git ignores.

## Checking

Compare a capture with a baseline taken in the same state, for example Off
or 60 °C:

```bash
python tools/pulse_capture/compare_capture.py BASELINE.csv CANDIDATE.csv
```

The candidate passes when:

- every frame has the baseline's pulse count;
- every HIGH and LOW is within ±10 µs of the baseline's mean;
- every frame period is within ±5 ms of the baseline's mean;
- every LOW between frames is at least 122 ms (the 120 ms SYNC plus the last
  2 ms LOW).

For a capture where the setting changes:

```bash
python tools/pulse_capture/compare_capture.py --codes 10,60 CAPTURE.csv
```

Every count must be one of the given codes, every width within ±10 µs of
2000.2 µs, and every gap at least 122 ms. There is no period check, because
a changed setting sends a frame early.

Both checks drop the first and last frames, which the capture may have cut.
They need at least 3 frames in between. Failures name the frame, counting
from 0 after the dropped one. The exit status is 0 for a pass.

Tests: `python -m unittest discover -s tools/pulse_capture -p "test_*.py"`.
