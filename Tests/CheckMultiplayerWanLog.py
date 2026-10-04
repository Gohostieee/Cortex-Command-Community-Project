"""Reject native guest logs that stop receiving state despite reporting FPS.

The encounter emits PERFORMANCE once per second. Check the full interval after
the baseline loads, including disconnects and a final freeze before lobby return.
"""
import re
import sys
from pathlib import Path

active = False
previous = None
samples = 0
for line in Path(sys.argv[1]).read_text(errors='replace').splitlines():
    if line == 'ENCOUNTER: guest world loaded':
        active = True
    if active and re.match(r'LOBBY UPDATE: .*playing=0', line):
        active = False
    match = re.match(r'PERFORMANCE: fps=([\d.]+) updates=(\d+)', line)
    if not active or not match:
        continue
    count = int(match[2])
    if previous is not None and count <= previous:
        raise SystemExit(f'FAIL: guest world stopped for a full sample interval: updates {previous} -> {count}, reported FPS {float(match[1]):.1f}')
    previous = count
    samples += 1
if samples < 15:
    raise SystemExit(f'FAIL: only {samples} gameplay samples; need at least 15')
print(f'PASS: fresh guest state throughout {samples} one-second gameplay samples')
