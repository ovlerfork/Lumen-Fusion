#!/usr/bin/env python3
"""Exercise the packaged benchmark and its measurement contract on native macOS."""
from __future__ import annotations

import csv
import io
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
        dangling_prefix = diagnostics / 'dangling-result'
        dangling = dangling_prefix.with_name(dangling_prefix.name + '-r1.json')
        target = diagnostics / 'dangling-target'
        require(not os.path.lexists(target), 'Dangling-symlink test target already exists')
        dangling.symlink_to(target)
        invoke(['--output', str(dangling_prefix), '--frames', '2', '--warmup', '0'],
               'dangling-output.log', False)
        require(dangling.is_symlink() and dangling.readlink() == target and not os.path.lexists(target),
                'Benchmark changed a dangling result symlink or created its target')
        require(not os.path.lexists(str(dangling_prefix) + '-r1.csv'), 'Benchmark left a partial CSV')
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
        sweep = diagnostics / 'frame-delay'
        result = subprocess.run(['/bin/bash', str(wrapper), '--app', str(app), '--output', str(sweep),
                                 '--suite', 'frame-delay', '--frames', '1000', '--warmup', '600',
                                 '--fps', '120', '--repeat', '2'], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1200)
        (diagnostics / 'frame-delay.log').write_text(result.stdout)
        print(result.stdout, flush=True)
        require(result.returncode == 0, 'Frame-delay sweep has failed cases; see retained logs')
        records = list(csv.DictReader((sweep / 'cases.tsv').open(), delimiter='\t'))
        require({row['case'] for row in records} ==
                {f'hevc-auto-frame-delay-{delay}' for delay in (-1, 0, 1, 2)} and
                len(records) == 4 and all(int(row['exit_code']) == 0 for row in records),
                'Frame-delay sweep did not complete all four cases')
        sweep_reports = sorted(sweep.glob('*-r*.json'))
        require({path.name for path in sweep_reports} ==
                {f'hevc-auto-frame-delay-{delay}-r{repeat}.json'
                 for delay in (-1, 0, 1, 2) for repeat in (1, 2)},
                'Expected two independent runs of all four frame delays')
        compact = []
        delay_compact = []
        for path in reports + sweep_reports:
            is_sweep = path.parent == sweep
            frames, warmup, requested_fps = (1000, 600, 120) if is_sweep else (180, 30, 60)
            total = frames + warmup
            report = json.loads(path.read_text())
            measured = report['measured']
            decoded = report['decoder_validation']
            request = report['requested']
            require(request['width'] == 1600 and request['height'] == 1112 and request['fps'] == requested_fps,
                    f'{path.name}: unexpected dimensions or FPS')
            require(request['frames'] == frames and request['warmup'] == warmup and request['repeat'] == 2,
                    f'{path.name}: unexpected frame or repeat settings')
            expected_delay = int(path.name.split('frame-delay-')[1].split('-r')[0]) if is_sweep else -1
            require(request['max_frame_delay'] == expected_delay,
                    f'{path.name}: unexpected requested frame delay')
            if is_sweep:
                require(request['codec'] == 'hevc' and request['variant'] == 'auto' and request['paced'],
                        f'{path.name}: sweep must use paced HEVC auto')
            setting = report['encoder']['max_frame_delay']
            require(setting['requested'] == expected_delay and setting['prepare_observed'] is True and
                    setting['application_stage'] == 'before_PrepareToEncodeFrames',
                    f'{path.name}: frame-delay setup was not observed before prepare')
            require(type(setting['supported_dictionary_status']) is int and
                    isinstance(setting['supported_property_description'], str),
                    f'{path.name}: missing support observation')
            status = setting['setter_status']
            require(status is None or type(status) is int, f'{path.name}: invalid setter status')
            outcome = ('inherit_unset' if expected_delay < 0 else 'not_attempted' if status is None else
                       'accepted' if status == 0 else 'unsupported' if status == -12900 else 'rejected')
            require(setting['setter_result'] == outcome and (expected_delay >= 0 or status is None),
                    f'{path.name}: setter outcome disagrees with request/status')
            for stage in ('after_open', 'after_run'):
                readback = setting[stage]
                require(type(readback['status']) is int and 'value' in readback and
                        (readback['value'] is None or type(readback['value']) is int),
                        f'{path.name}: invalid {stage} property readback')
            require(request['output_completion'] and request['allow_sw'] == 0,
                    f'{path.name}: completion/hardware-only requirement changed')
            require(measured['submitted'] == frames and measured['matched_packets'] == frames and
                    measured['final_backlog'] == 0, f'{path.name}: packet count or backlog mismatch')
            require(decoded['status'] == 'passed' and decoded['decoded_frames'] == total and
                    decoded['expected_frames'] == total, f'{path.name}: native decode validation failed')
            columns = report['columns']
            rows = [dict(zip(columns, row)) for row in report['rows']]
            require(len(rows) == total, f'{path.name}: incomplete raw samples')
            require([row['pts'] for row in rows] == list(range(total)), f'{path.name}: PTS mismatch')
            phase = [row for row in rows if row['phase'] == 'measured']
            require(len(phase) == frames, f'{path.name}: measured raw count differs')
            latency = [row['packet_receipt_ms'] - row['send_enter_ms'] for row in phase]
            require(math.isclose(statistics.mean(latency), measured['submit_to_output_ms']['mean'],
                                 rel_tol=1e-9, abs_tol=1e-9), f'{path.name}: summary differs from raw samples')
            ordered = sorted(latency)
            for key, value in (('p99', ordered[math.ceil(0.99 * len(ordered)) - 1]), ('max', ordered[-1])):
                require(math.isclose(value, measured['submit_to_output_ms'][key], rel_tol=1e-9, abs_tol=1e-9),
                        f'{path.name}: {key} differs from matched raw samples')
            require(measured['submit_to_output_ms']['count'] == frames and
                    math.isclose(measured['frame_budget_ms'], 1000 / requested_fps, rel_tol=1e-9) and
                    measured['submit_to_output_over_budget_count'] ==
                    sum(value > 1000 / requested_fps for value in latency),
                    f'{path.name}: frame-budget count differs from raw samples')
            # Includes M=0: validate the observation, not a synchrony or queue-bound promise.
            require(all(type(row['callback_completed_at_native_return']) is bool and
                        type(row['native_pending_at_return']) is int and row['native_pending_at_return'] >= 0 and
                        row['callback_completed_at_native_return'] == (row['native_pending_at_return'] == 0) and
                        row['native_encode_status'] == 0 for row in rows),
                    f'{path.name}: inconsistent native return observations')
            require(measured['callbacks_completed_at_native_return'] ==
                    sum(row['callback_completed_at_native_return'] for row in phase) and
                    measured['max_native_pending_at_return'] == max(row['native_pending_at_return'] for row in phase),
                    f'{path.name}: native observation summary differs from raw samples')
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
            require(table[0] == columns and len(table) == total + 1, f'{path.name}: CSV does not match JSON')
            entry = {'case': path.stem, 'encoder': report['encoder']['EncoderID'],
                     'submit_to_output_ms': measured['submit_to_output_ms'],
                     'actual_fps': fps, 'wake_lateness_ms': measured['wake_lateness_ms'],
                     'skipped_ticks': measured['skipped_ticks_before_submissions'],
                     'decoded_frames': decoded['decoded_frames']}
            if is_sweep:
                entry.update({'requested': request, 'max_frame_delay': setting,
                              'frame_budget_ms': measured['frame_budget_ms'],
                              'submit_to_output_over_budget_count': measured['submit_to_output_over_budget_count'],
                              'callbacks_completed_at_native_return': measured['callbacks_completed_at_native_return'],
                              'max_native_pending_at_return': measured['max_native_pending_at_return'],
                              'native_return_observation': report['native_return_observation'],
                              'maximum_interpretation': report['maximum_interpretation']})
                delay_compact.append(entry)
            else:
                compact.append(entry)
        (diagnostics / 'summary.json').write_text(json.dumps(compact, indent=2) + '\n')
        (diagnostics / 'frame-delay-summary.json').write_text(json.dumps(delay_compact, indent=2) + '\n')
        # Exercise the shipped reader with native results and the system-only PATH.
        summary_fields = {
            'requestedFPS': ('requested', 'fps'),
            'measuredCount': ('measured', 'submit_to_output_ms', 'count'),
            'actualFPS': ('measured', 'actual_fps'),
            'encoderP50_ms': ('measured', 'submit_to_output_ms', 'p50'),
            'encoderP95_ms': ('measured', 'submit_to_output_ms', 'p95'),
            'encoderP99_ms': ('measured', 'submit_to_output_ms', 'p99'),
            'encoderMax_ms': ('measured', 'submit_to_output_ms', 'max'),
            'prepMax_ms': ('measured', 'prep_ms', 'max'),
            'wakeLatenessMax_ms': ('measured', 'wake_lateness_ms', 'max'),
            'packetGapMax_ms': ('measured', 'packet_interarrival_ms', 'max'),
            'skippedTicks': ('measured', 'skipped_ticks_before_submissions'),
            'coldEncoder_ms': ('cold_first_frame', 'submit_to_output_ms'),
            'warmupEncoderMax_ms': ('warmup', 'submit_to_output_ms', 'max'),
        }
        for directory in (cases, sweep):
            originals = {p.name: p.read_bytes() for p in directory.iterdir() if p.is_file()}
            result = subprocess.run(['/bin/bash', str(wrapper), '--app', '/nonexistent.app',
                                     '--summarize', str(directory)], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            (diagnostics / f'{directory.name}-summary.tsv').write_text(result.stdout)
            require(result.returncode == 0, 'Read-only summary failed without an app')
            summaries = list(csv.DictReader(io.StringIO(result.stdout), delimiter='\t'))
            expected_reports = {p.stem: json.loads(p.read_text()) for p in directory.glob('*-r*.json')}
            require(len(summaries) == len(expected_reports) and
                    {row['case'] for row in summaries} == set(expected_reports), 'Summary result set differs')
            for row in summaries:
                for column, keys in summary_fields.items():
                    value = expected_reports[row['case']]
                    for key in keys:
                        value = value.get(key) if isinstance(value, dict) else None
                    require(row[column] == 'N/A' if value is None else
                            math.isclose(float(row[column]), value, rel_tol=1e-5, abs_tol=1e-6),
                            f"{row['case']}: summary {column} differs from native JSON")
            require(originals == {p.name: p.read_bytes() for p in directory.iterdir() if p.is_file()},
                    'Summary altered result files')
        for args in (['--summarize', str(home / 'missing')], ['--summarize', str(state)],
                     ['--suite', 'frame-delay', '--max-frame-delay', '0']):
            result = subprocess.run(['/bin/bash', str(wrapper), *args], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            require(result.returncode != 0, f'Wrapper accepted invalid arguments: {args}')
        existing = reports[0]
        prefix = str(existing)[:-len('-r1.json')] if existing.name.endswith('-r1.json') else ''
        require(bool(prefix), 'Expected first repeat output')
        original = existing.read_bytes()
        invoke(['--output', prefix], 'existing-output.log', False)
        require(existing.read_bytes() == original, 'Existing result was overwritten')
        after = {str(p.relative_to(home)): p.read_bytes() for p in home.rglob('*') if p.is_file()}
        require(before == after, 'Benchmark changed or created user-state files')
        subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
        print('Packaged benchmark: 32 runs, native decode and raw measurement checks passed; user state and signature preserved.')
        print('No latency-bound or sustained-FPS performance threshold is asserted by this correctness test.')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(f'Benchmark validation failed: {error}', file=sys.stderr)
        raise SystemExit(1)
