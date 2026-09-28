#!/usr/bin/env python3
"""Exercise the packaged benchmark and its measurement contract on native macOS."""
from __future__ import annotations

import csv
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    if len(sys.argv) != 3:
        raise RuntimeError('Usage: validate-macos-benchmark.py APP DIAGNOSTICS')
    app = Path(sys.argv[1]).resolve()
    diagnostics = Path(sys.argv[2]).resolve()
    diagnostics.mkdir(parents=True, exist_ok=True)
    binary = app / 'Contents/MacOS/Lumen Fusion'
    wrapper = app / 'Contents/Resources/benchmark-latency.command'
    require(binary.is_file() and wrapper.is_file(), 'Packaged benchmark or wrapper missing')

    with tempfile.TemporaryDirectory(prefix='lumen benchmark home ') as temporary:
        home = Path(temporary)
        state = home / '.config/lumina'
        state.mkdir(parents=True)
        # The early benchmark entry must not parse, replace or supplement these files.
        sentinels = {name: f'benchmark must not read or change {name}\n'.encode()
                     for name in ('sunshine.conf', 'sunshine_state.json', 'apps.json')}
        for name, content in sentinels.items():
            (state / name).write_bytes(content)
        before = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob('*') if p.is_file()}
        env = os.environ.copy()
        env['HOME'] = str(home)
        env['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
        env.pop('DYLD_LIBRARY_PATH', None)

        def invoke(args: list[str], name: str, expected_success: bool) -> subprocess.CompletedProcess[str]:
            result = subprocess.run([str(binary), '--benchmark', *args], env=env,
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            (diagnostics / name).write_text(result.stdout)
            require((result.returncode == 0) == expected_success,
                    f'{name}: unexpected exit {result.returncode}; see retained log')
            return result

        invoke(['--help'], 'help.log', True)
        invoke(['--width', '65'], 'invalid-dimensions.log', False)
        invoke(['--codec', 'hevc', '--variant', 'h264-cavlc'], 'invalid-variant.log', False)
        invoke(['--output', str(app / 'Contents/Resources/benchmark-output')], 'invalid-bundle-output.log', False)
        cases = diagnostics / 'cases'
        result = subprocess.run(['/bin/bash', str(wrapper), '--app', str(app), '--output', str(cases),
                                 '--repeat', '2'], env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=900)
        (diagnostics / 'matrix.log').write_text(result.stdout)
        print(result.stdout, flush=True)
        require(result.returncode == 0, 'Benchmark matrix has failed cases; see cases.tsv and individual logs')
        records = list(csv.DictReader((cases / 'cases.tsv').open(), delimiter='\t'))
        require(len(records) == 12 and all(int(row['exit_code']) == 0 for row in records),
                'Comparison matrix did not complete all 12 cases')
        reports = sorted(cases.glob('*.json'))
        require(len(reports) == 24, 'Expected two independent runs of all 12 cases')
        compact = []
        for path in reports:
            report = json.loads(path.read_text())
            measured = report['measured']
            decoded = report['decoder_validation']
            request = report['requested']
            require(request['width'] == 1600 and request['height'] == 1112 and request['fps'] == 60,
                    f'{path.name}: unexpected dimensions or FPS')
            require(request['output_completion'] and request['allow_sw'] == 0,
                    f'{path.name}: completion/hardware-only requirement changed')
            require(measured['submitted'] == 180 and measured['matched_packets'] == 180 and
                    measured['final_backlog'] == 0, f'{path.name}: packet count or backlog mismatch')
            require(decoded['status'] == 'passed' and decoded['decoded_frames'] == 210 and
                    decoded['expected_frames'] == 210, f'{path.name}: native decode validation failed')
            columns = report['columns']
            rows = [dict(zip(columns, row)) for row in report['rows']]
            require(len(rows) == 210, f'{path.name}: incomplete raw samples')
            require([row['pts'] for row in rows] == list(range(210)), f'{path.name}: PTS mismatch')
            phase = [row for row in rows if row['phase'] == 'measured']
            latency = [row['packet_receipt_ms'] - row['send_enter_ms'] for row in phase]
            require(math.isclose(statistics.mean(latency), measured['submit_to_output_ms']['mean'],
                                 rel_tol=1e-9, abs_tol=1e-9), f'{path.name}: summary differs from raw samples')
            fps = (len(phase) - 1) * 1000 / (phase[-1]['packet_receipt_ms'] - phase[0]['packet_receipt_ms'])
            require(math.isclose(fps, measured['actual_fps'], rel_tol=1e-9),
                    f'{path.name}: effective FPS does not match packet timestamps')
            require(all(row['prep_enter_ms'] <= row['prep_exit_ms'] <= row['send_enter_ms'] <=
                        row['send_exit_ms'] <= row['packet_receipt_ms'] <= row['loop_exit_ms'] for row in rows),
                    f'{path.name}: timestamps are not ordered')
            require(sum(row['skipped_ticks'] for row in rows) == rows[-1]['tick'] + 1 - len(rows),
                    f'{path.name}: missed-tick accounting differs')
            with path.with_suffix('.csv').open(newline='') as stream:
                table = list(csv.reader(stream))
            require(table[0] == columns and len(table) == 211, f'{path.name}: CSV does not match JSON')
            compact.append({'case': path.stem, 'encoder': report['encoder']['EncoderID'],
                            'submit_to_output_ms': measured['submit_to_output_ms'],
                            'actual_fps': fps, 'wake_lateness_ms': measured['wake_lateness_ms'],
                            'skipped_ticks': measured['skipped_ticks_before_submissions'],
                            'decoded_frames': decoded['decoded_frames']})
        (diagnostics / 'summary.json').write_text(json.dumps(compact, indent=2) + '\n')
        existing = reports[0]
        prefix = str(existing)[:-len('-r1.json')] if existing.name.endswith('-r1.json') else ''
        require(bool(prefix), 'Expected first repeat output')
        original = existing.read_bytes()
        invoke(['--output', prefix], 'existing-output.log', False)
        require(existing.read_bytes() == original, 'Existing result was overwritten')
        after = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob('*') if p.is_file()}
        require(before == after, 'Benchmark changed or created user-state files')
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
        print('Packaged benchmark: 24 runs, native decode and raw measurement checks passed; user state and signature preserved.')
        print('No 5 ms or sustained-60-FPS performance threshold is asserted by this correctness test.')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(f'Benchmark validation failed: {error}', file=sys.stderr)
        raise SystemExit(1)
