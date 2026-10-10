"""Summarize the dedicated worker's load fixture and per-stage loop timings."""
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


def load_summary(path):
    rows = [{key: float(value) for key, value in row.items()} for row in csv.DictReader(path.open())]
    if len(rows) < 3:
        return {'valid': False, 'reason': 'fewer than three load samples'}
    # Skip the first two seconds while the rematch warms up.
    measured = [row for row in rows if row['real_ms'] >= rows[0]['real_ms'] + 2000]
    first, last = measured[0], measured[-1]
    real = last['real_ms'] - first['real_ms']
    return {
        'valid': real > 0,
        'seconds': round(real / 1000, 2),
        'simulation_speed': round((last['sim_ms'] - first['sim_ms']) / real, 4) if real > 0 else None,
        'actors_peak': max(row['actors'] for row in measured),
        'stress_alive_peak': max(row['stress_alive'] for row in measured),
        'particles_peak': max(row['particles'] for row in measured),
        'moids_peak': max(row['moids'] for row in measured),
        'explosions': last['explosions'],
    }


def stage_summary(path):
    rows = list(csv.DictReader(path.open()))
    playing = [row for row in rows if row.get('playing') == '1']
    if not playing:
        return {'valid': False}
    result = {'valid': True, 'samples': len(playing)}
    for key in playing[0]:
        if key.endswith('_ms') or key.endswith('_hz') or key.endswith('_kbps') or key in ('loops', 'sim_updates', 'snapshots'):
            values = [float(row[key]) for row in playing if row[key] != '']
            result[key + '_p50'] = percentile(values, 50)
            result[key + '_p95'] = percentile(values, 95)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    summary = {}
    load = args.directory / 'load-benchmark.csv'
    if load.exists():
        summary['load'] = load_summary(load)
    stages = args.directory / 'server-benchmark.csv'
    if stages.exists():
        summary['server'] = stage_summary(stages)
    (args.directory / 'server-summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
