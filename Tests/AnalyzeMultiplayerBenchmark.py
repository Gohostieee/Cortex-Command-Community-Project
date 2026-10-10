"""Summarize opt-in native traces; keep planned reconnects out of combat latency."""
import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100
    lo, hi = math.floor(position), math.ceil(position)
    return round(ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo), 2)


def analyze(path):
    rows = list(csv.DictReader(path.open()))
    first_playing = next((int(r['time_ms']) for r in rows if r['epoch'] == '1' and r['playing'] == '1'), None)
    first_interactive = next((int(r['time_ms']) for r in rows if r['epoch'] == '1' and r['playing'] == '1' and r['ready'] == '1' and r['paused'] == '0'), None)
    selected = []
    active_since = None
    for row in rows:
        values = {key: int(value) for key, value in row.items() if key != 'event'}
        active = values['playing'] and values['ready'] and not values['paused'] and values['stage'] == 3
        if not active:
            active_since = None
            continue
        if active_since is None:
            active_since = values['time_ms']
        if values['time_ms'] - active_since >= 2000:
            selected.append(dict(event=row['event'], segment=active_since, **values))
    samples = [row for row in selected if row['event'] == 'sample']
    worlds = [row['delta_ms'] for row in selected if row['event'] == 'world']
    frames = [row['delta_ms'] for row in selected if row['event'] == 'render']
    if not samples or not worlds or not frames:
        log = path.with_name(path.name.replace('-benchmark.csv', '-smoke.log')).read_text(errors='replace')
        return {'trace': str(path), 'valid': False, 'reason': 'No measured rematch combat interval',
                'first_match_load_seconds': round((first_interactive - first_playing) / 1000, 2) if first_interactive is not None and first_playing is not None else None,
                'ever_interactive': first_interactive is not None, 'world_updates_total': sum(r['event'] == 'world' for r in rows),
                'lifecycle_passed': False, 'failures': [line for line in log.splitlines() if line.startswith('FAIL:')]}
    log = path.with_name(path.name.replace('-benchmark.csv', '-smoke.log')).read_text(errors='replace')
    spans = {}
    for row in selected:
        start, end = spans.get(row['segment'], (row['time_ms'], row['time_ms']))
        spans[row['segment']] = (min(start, row['time_ms']), max(end, row['time_ms']))
    return {
        'trace': str(path), 'valid': True,
        'upload_mbps_measured': sorted({r['upload_mbps'] for r in selected if 'upload_mbps' in r}),
        'first_match_load_seconds': round((first_interactive - first_playing) / 1000, 2) if first_interactive is not None and first_playing is not None else None,
        'lifecycle_passed': 'PASS: hosted client received two matches, combat updates and persistent lobby' in log and 'FAIL:' not in log,
        'combat_seconds': round(sum(end - start for start, end in spans.values()) / 1000, 2),
        'render_fps_mean': round(1000 * len(frames) / sum(frames), 2),
        'frame_ms_p95': percentile(frames, 95), 'frame_ms_p99': percentile(frames, 99), 'frame_ms_max': max(frames),
        'world_hz_median': percentile([r['world_hz'] for r in samples], 50),
        'world_hz_p5': percentile([r['world_hz'] for r in samples], 5),
        'update_gap_ms_p50': percentile(worlds, 50), 'update_gap_ms_p95': percentile(worlds, 95),
        'update_gap_ms_p99': percentile(worlds, 99), 'update_gap_ms_max': max(worlds),
        'sampled_update_age_ms_p95': percentile([r['delta_ms'] for r in samples], 95),
        'sampled_update_age_ms_max': max(r['delta_ms'] for r in samples),
        'stale_samples_over_500ms': sum(r['delta_ms'] >= 500 for r in samples),
        'stale_samples_over_1000ms': sum(r['delta_ms'] >= 1000 for r in samples),
        'ping_ms_p50': percentile([r['ping_ms'] for r in samples], 50),
        'world_updates_measured': len(worlds), 'frames_measured': len(frames),
        # Scene tiles drawn from an older revision, or covered black while missing.
        'stale_tiles_p95': percentile([r.get('stale_tiles', 0) for r in samples], 95),
        'black_tiles_p95': percentile([r.get('black_tiles', 0) for r in samples], 95),
        'black_fog_tiles_p95': percentile([r.get('black_fog_tiles', 0) for r in samples], 95),
        'samples': len(samples),
        'samples_with_black_tiles': sum(r.get('black_tiles', 0) > 0 for r in samples),
        'samples_with_black_fog_tiles': sum(r.get('black_fog_tiles', 0) > 0 for r in samples),
        # Own-movement prediction: share of samples predicting, and host corrections (pixels).
        'motion_active_share': round(sum(r.get('motion_active', 0) for r in samples) / max(1, len(samples)), 2),
        'motion_correction_px_p50': percentile([r.get('motion_correction_px', 0) / 10 for r in samples if r.get('motion_active', 0)], 50),
        'motion_correction_px_p95': percentile([r.get('motion_correction_px', 0) / 10 for r in samples if r.get('motion_active', 0)], 95)
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = [analyze(path) for path in sorted(args.directory.rglob('*-benchmark.csv'))]
    if not result:
        raise SystemExit('No benchmark traces found')
    output = args.directory / 'client-summary.json'
    output.write_text(json.dumps(result, indent=2))
    for row in result:
        print(json.dumps(row))


if __name__ == '__main__':
    main()
