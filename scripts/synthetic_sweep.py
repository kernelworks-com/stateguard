#!/usr/bin/env python3
"""Generate many synthetic histories, check every one, and reduce a sample.

For each seed and each plantable fault (plus the fault-free history), the
sweep runs stateguard-generate, checks the result with stateguard-check
--json, and compares the verdict and first finding with the generator's
documented expectation. A sample of the failing traces is then reduced and
the reduced witness is checked again. Only Python's standard library and the
built tools are required. The exit code is 0 only when every verdict matched.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

# Expected (status, first category) per generator fault; see REFERENCE.md.
EXPECTED = {
    'none': ('clean', None),
    'premature_reclaim': ('violation', 'premature_reclaim'),
    'stale_generation': ('violation', 'stale_generation'),
    'incompatible_semantic_reuse': ('violation', 'incompatible_semantic_reuse'),
    'write_through_shared_immutable_state': ('violation', 'write_through_shared_immutable_state'),
    'double_completion': ('violation', 'double_completion'),
    'orphaned_completion': ('violation', 'orphaned_operation'),
    'publish_before_writer_completion': ('violation', 'invalid_state_transition'),
    'failed_write_publication': ('violation', 'failed_write_publication'),
    'dropped_completion': ('inconclusive', 'incomplete_evidence'),
    'coverage_gap': ('inconclusive', 'incomplete_evidence'),
    'incomplete_end': ('inconclusive', 'incomplete_evidence'),
}

# A few history shapes, cycled by seed, so the sweep is not one size only.
SHAPES = [
    {},
    {'--steps': '200', '--allocations': '6', '--prefixes': '4', '--requests': '6'},
    {'--steps': '800', '--allocations': '12', '--prefixes': '8', '--requests': '10'},
]


def tool(build_dir, name, explicit):
    if explicit:
        return str(explicit)
    if build_dir:
        for candidate in (Path(build_dir) / name, Path(build_dir) / 'Release' / name,
                          Path(build_dir) / (name + '.exe'), Path(build_dir) / 'Release' / (name + '.exe')):
            if candidate.exists():
                return str(candidate)
    found = shutil.which(name)
    if not found:
        sys.exit(f'synthetic_sweep: cannot find {name}; pass --build-dir or --{name.split("-")[1]}')
    return found


def check(checker, path):
    result = subprocess.run([checker, '--json', str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode == 3:
        return {'status': 'input_error', 'exit_code': 3, 'diagnostics': []}
    return json.loads(result.stdout)


def first_category(report):
    for diagnostic in report.get('diagnostics', []):
        if not diagnostic.get('follow_on', False):
            return diagnostic['category']
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--generate', type=Path)
    parser.add_argument('--check', type=Path)
    parser.add_argument('--reduce', type=Path)
    parser.add_argument('--seeds', type=int, default=100, help='seeds per fault (default 100)')
    parser.add_argument('--first-seed', type=int, default=1)
    parser.add_argument('--faults', default='all', help='comma-separated fault names (default all)')
    parser.add_argument('--reduce-sample', type=int, default=2,
                        help='failing traces to reduce per fault (default 2, 0 to skip)')
    parser.add_argument('--reduce-max-checks', type=int, default=4000)
    parser.add_argument('--json', type=Path, help='write the full record here')
    parser.add_argument('--keep-mismatches', type=Path, help='directory for traces whose verdict did not match')
    args = parser.parse_args()

    generate = tool(args.build_dir, 'stateguard-generate', args.generate)
    checker = tool(args.build_dir, 'stateguard-check', args.check)
    reducer = tool(args.build_dir, 'stateguard-reduce', args.reduce)
    faults = list(EXPECTED) if args.faults == 'all' else args.faults.split(',')
    unknown = [f for f in faults if f not in EXPECTED]
    if unknown:
        sys.exit(f'synthetic_sweep: unknown fault(s) {unknown}')

    started = time.time()
    per_fault = {}
    mismatches = []
    reductions = []
    with tempfile.TemporaryDirectory(prefix='stateguard-sweep-') as scratch:
        scratch = Path(scratch)
        for fault in faults:
            counts = {'checked': 0, 'matched': 0, 'events': 0}
            reduced_here = 0
            for seed in range(args.first_seed, args.first_seed + args.seeds):
                shape = SHAPES[seed % len(SHAPES)]
                trace = scratch / f'{fault}-{seed}.sgtrace'
                command = [generate, '--seed', str(seed), '--output', str(trace)]
                for flag, value in shape.items():
                    command += [flag, value]
                if fault != 'none':
                    command += ['--fault', fault]
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                report = check(checker, trace)
                expected_status, expected_category = EXPECTED[fault]
                category = first_category(report)
                counts['checked'] += 1
                counts['events'] += sum(1 for line in trace.read_text().splitlines() if '|' in line)
                if report['status'] == expected_status and category == expected_category:
                    counts['matched'] += 1
                else:
                    mismatches.append({'fault': fault, 'seed': seed, 'shape': shape,
                                       'status': report['status'], 'first_category': category})
                    if args.keep_mismatches:
                        args.keep_mismatches.mkdir(parents=True, exist_ok=True)
                        shutil.copy(trace, args.keep_mismatches / trace.name)
                if fault != 'none' and reduced_here < args.reduce_sample and \
                        report['status'] == expected_status and category == expected_category:
                    reduced_here += 1
                    out = scratch / f'{fault}-{seed}-reduced.sgtrace'
                    t0 = time.time()
                    result = subprocess.run([reducer, '--max-checks', str(args.reduce_max_checks),
                                             '--output', str(out), str(trace)], text=True,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                    record = {'fault': fault, 'seed': seed, 'reduce_exit': result.returncode,
                              'seconds': round(time.time() - t0, 3)}
                    if result.returncode == 0:
                        after = check(checker, out)
                        record['before_events'] = sum(1 for l in trace.read_text().splitlines() if '|' in l)
                        record['after_events'] = sum(1 for l in out.read_text().splitlines() if '|' in l)
                        record['same_finding'] = (after['status'] == expected_status and
                                                  first_category(after) == expected_category)
                    reductions.append(record)
                trace.unlink(missing_ok=True)
            per_fault[fault] = counts

    total = sum(c['checked'] for c in per_fault.values())
    matched = sum(c['matched'] for c in per_fault.values())
    bad_reductions = [r for r in reductions if r['reduce_exit'] != 0 or not r.get('same_finding')]
    summary = {
        'seeds_per_fault': args.seeds, 'first_seed': args.first_seed, 'shapes': SHAPES,
        'histories_checked': total, 'verdicts_matched': matched,
        'events_checked': sum(c['events'] for c in per_fault.values()),
        'per_fault': per_fault, 'mismatches': mismatches,
        'reductions': reductions, 'reductions_with_same_finding':
            sum(1 for r in reductions if r.get('same_finding')),
        'seconds': round(time.time() - started, 1),
        'platform': f'{sys.platform} python {sys.version.split()[0]}',
    }
    if args.json:
        args.json.write_text(json.dumps(summary, indent=2) + '\n')

    print(f'synthetic sweep: {total} histories ({summary["events_checked"]} events), '
          f'{matched} verdicts matched, {len(mismatches)} mismatched, '
          f'{summary["reductions_with_same_finding"]}/{len(reductions)} reductions kept the finding, '
          f'{summary["seconds"]} s')
    width = max(len(f) for f in per_fault)
    for fault, counts in per_fault.items():
        print(f'  {fault:<{width}}  {counts["matched"]}/{counts["checked"]}')
    for item in mismatches[:20]:
        print(f'  mismatch: {item}')
    for item in bad_reductions[:20]:
        print(f'  reduction problem: {item}')
    return 0 if not mismatches and not bad_reductions else 1


if __name__ == '__main__':
    sys.exit(main())
