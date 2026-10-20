"""Run CPU mocked static-put scenarios using the pinned upstream test helpers.

Run from the Stateguard root: python -m adapters.lmcache.regression --help
"""
import argparse
import importlib.util
import json
import platform
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import types

from . import TraceWriter, instrument

PINS = {'919fe8d67369cfe1dea85b28725f71a611369e1a': 'vulnerable',
        '217b1d8248fdf63e1c6eede75a3ac5cde998a7b7': 'fixed'}


def load_helpers(checkout, tests):
    revision = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    if revision not in PINS:
        raise ValueError('checkout must be one of the documented pins')
    # The test helper must be byte-identical to the fixed pin, including when
    # executing the vulnerable checkout. Never import an installed PyPI LMCache.
    expected = subprocess.check_output(['git', '-C', str(tests.parent.parent.parent),
                                        'show', next(k for k, v in PINS.items() if v == 'fixed') +
                                        ':tests/v1/test_nixl_put_refcount.py'])
    if tests.read_bytes() != expected:
        raise ValueError('upstream helper differs from the fixed pin')
    sys.path.insert(0, str(checkout))
    spec = importlib.util.spec_from_file_location('stateguard_upstream_helpers', tests)
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    import lmcache
    if Path(lmcache.__file__).resolve().parent != checkout / 'lmcache':
        raise ValueError('LMCache imported from outside the selected checkout')
    return revision, helpers


def scenario(helpers, scenario_name, enabled=True, failed_post_is_quiescent=True):
    """The loop and all producers are drained before hooks are restored."""
    gate = threading.Event()
    entered = threading.Event()
    finished = threading.Event()
    loop = helpers._LoopThread()
    backend = helpers._mock_static_backend(loop.loop, gate)
    for name in ('batched_submit_put_task', '_collect_metadata_with_lock', 'contains', 'remove'):
        setattr(backend, name, types.MethodType(getattr(helpers.NixlStaticStorageBackend, name), backend))
    keys, objects = helpers._make_put(1)

    def transfer(handle):
        entered.set()
        if not gate.wait(5):
            raise TimeoutError('transfer gate timed out')
        if scenario_name == 'failure':
            raise RuntimeError('injected transfer failure')
    backend.agent.post_blocking.side_effect = transfer
    writer = TraceWriter()
    capture = instrument(backend, writer, failed_post_is_quiescent=failed_post_is_quiescent) if enabled else None
    # Observe task return after all upstream finally blocks, including ref drops.
    original = backend.mem_to_storage
    def schedule(*args, **kwargs):
        coroutine = original(*args, **kwargs)
        async def run():
            try:
                return await coroutine
            finally:
                finished.set()
        return run()
    backend.mem_to_storage = schedule
    try:
        backend.batched_submit_put_task(keys, objects)
        if not entered.wait(5):
            raise TimeoutError('transfer was not entered')
        if scenario_name == 'source':
            objects[0].ref_count_down()
        gate.set()
        if not finished.wait(5):
            raise TimeoutError('put did not finish')
        # Barrier: the task's synchronous exception/reporting path has run.
        import asyncio
        asyncio.run_coroutine_threadsafe(asyncio.sleep(0), loop.loop).result(timeout=5)
        visible = backend.contains(keys[0])
        if visible:
            backend._collect_metadata_with_lock(keys)
        state = {'visible': visible, 'inflight': bool(backend.progress_set),
                 'metadata': bool(backend.key_dict), 'refs': objects[0].get_ref_count()}
        if scenario_name == 'failure':
            objects[0].ref_count_down()
        return writer.finish() if enabled else None, state
    finally:
        gate.set()
        loop.close()
        if capture:
            capture.close()


def check(checker, trace):
    with tempfile.TemporaryDirectory(prefix='stateguard-lmcache-') as directory:
        path = Path(directory) / 'capture.sgtrace'
        path.write_text(trace)
        result = subprocess.run([str(checker), str(path)], capture_output=True, text=True)
    if result.returncode not in (0, 1, 2):
        raise RuntimeError(result.stderr or result.stdout)
    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--upstream-tests', type=Path, required=True,
                        help='fixed checkout tests/v1/test_nixl_put_refcount.py')
    parser.add_argument('--checker', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--benchmark-repeat', type=int, default=10)
    args = parser.parse_args()
    if args.repeat < 1 or args.benchmark_repeat < 2:
        parser.error('repeat must be positive and benchmark-repeat at least 2')
    revision, helpers = load_helpers(args.checkout.resolve(), args.upstream_tests.resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    for name in ('source', 'failure'):
        for repetition in range(args.repeat):
            trace, state = scenario(helpers, name)
            code, report = check(args.checker.resolve(), trace)
            if PINS[revision] == 'fixed':
                assert code == 0, report
                assert not state['inflight']
                assert state['visible'] == (name == 'source')
            elif name == 'source':
                assert code == 1 and 'premature_reclaim' in report, report
            else:
                assert code == 2 and 'cleanup-' in report and 'no observed access completion' in report, report
                assert state['inflight'] and state['metadata'] and not state['visible']
            (args.output / f'{name}-{repetition}.sgtrace').write_text(trace)
            records.append({'scenario': name, 'returncode': code, 'state': state, 'report': report})
    unproven, _ = scenario(helpers, 'failure', failed_post_is_quiescent=False)
    unproven_code, unproven_report = check(args.checker.resolve(), unproven)
    assert unproven_code == 2 and 'unproven-failed-transfer-completion' in unproven_report, unproven_report
    (args.output / 'unproven-failure.sgtrace').write_text(unproven)
    timings = {}
    # One warmup per mode, then wall time including the same mocked backend,
    # loop startup, gated transfer, metadata lookup, and teardown in every mode.
    for mode in ('off', 'capture', 'checking'):
        samples = []
        for iteration in range(args.benchmark_repeat + 1):
            start = time.perf_counter_ns()
            trace, _ = scenario(helpers, 'source', enabled=mode != 'off')
            if mode == 'checking':
                check(args.checker.resolve(), trace)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            if iteration:
                samples.append(elapsed)
        timings[mode] = {'samples_ms': samples, 'median_ms': statistics.median(samples),
                         'min_ms': min(samples), 'max_ms': max(samples)}
    result = {'revision': revision, 'kind': PINS[revision], 'records': records,
              'unproven_failure_returncode': unproven_code,
              'environment': {'platform': platform.platform(), 'python': platform.python_version(),
                              'torch': helpers.torch.__version__},
              'timings': timings, 'benchmark_repetitions': args.benchmark_repeat}
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'revision': revision, 'scenarios': len(records), 'timings': timings}, indent=2))


if __name__ == '__main__':
    main()
