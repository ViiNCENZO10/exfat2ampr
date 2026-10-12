#!/usr/bin/env python3
"""Repeat host conversions with several worker counts; keep each log and a JSON report (median per count).
The host cache is not flushed: the numbers compare builds on one machine, they do not predict the PS5."""
import argparse
import json
import platform
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('--binary', type=Path, default=Path('./exfat2ampr-host'))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workers', type=int, nargs='+', default=[1, 2, 4, 6])
    p.add_argument('--level', type=int, default=6)
    p.add_argument('--repeat', type=int, default=3)
    a = p.parse_args()
    if not a.source.is_dir() or not a.binary.is_file():
        p.error('a source folder and the compiled host binary are required')
    if a.repeat < 1 or not 1 <= a.level <= 12 or any(not 1 <= n <= 64 for n in a.workers):
        p.error('repeat >= 1, level 1..12 and workers 1..64')
    a.output.mkdir(parents=True, exist_ok=False)
    results = []
    for workers in a.workers:
        for trial in range(a.repeat):
            with tempfile.TemporaryDirectory(prefix='e2a-benchmark-', dir=a.output) as dest:
                cmd = [str(a.binary.resolve()), '--workers', str(workers), '--level', str(a.level),
                       str(a.source.resolve()), str(Path(dest).resolve()), 'PPSA00001']
                started = time.perf_counter()
                run = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                elapsed = time.perf_counter() - started
                log = f'workers-{workers}-trial-{trial + 1}.log'
                (a.output / log).write_text(run.stdout, encoding='utf-8')
                results.append(dict(workers=workers, trial=trial + 1, seconds=elapsed, returncode=run.returncode, log=log))
                report = dict(platform=platform.platform(), level=a.level, source=str(a.source.resolve()),
                              cache='not flushed', runs=results)
                (a.output / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
                if run.returncode:
                    raise SystemExit(f'conversion failed ({run.returncode}); see {a.output / log}')
    for workers in a.workers:
        values = [r['seconds'] for r in results if r['workers'] == workers]
        print(f'{workers} workers: median {statistics.median(values):.3f} s')


if __name__ == '__main__':
    main()
