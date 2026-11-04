#!/usr/bin/env python3
"""Verify installed tools and a downstream CMake consumer, then uninstall.

Only Python's standard library, CMake, and a C++20 compiler are required.
Use a fresh --workdir; this check never installs into the system prefix.
"""
import argparse
from pathlib import Path
import shutil
import subprocess


def run(*args, expected=0, cwd=None):
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != expected:
        raise RuntimeError(f'{args[0]} exited {result.returncode}, expected {expected}\n{result.stdout}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--workdir', type=Path, required=True)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--shared', action='store_true')
    parser.add_argument('--cxx-flags', default='-Wall -Wextra -Wpedantic -Werror')
    args = parser.parse_args()
    source = args.source.resolve()
    work = args.workdir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    build, prefix, relocated = work / 'build', work / 'prefix', work / 'relocated prefix'
    common = [f'-DCMAKE_CXX_COMPILER={args.compiler}', f'-DCMAKE_CXX_FLAGS={args.cxx_flags}']
    run(args.cmake, '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
        '-DSTATEGUARD_BUILD_TESTS=OFF', '-DSTATEGUARD_BUILD_EXAMPLES=ON',
        '-DBUILD_SHARED_LIBS=' + ('ON' if args.shared else 'OFF'), *common)
    run(args.cmake, '--build', build, '--parallel', '2')
    run(args.cmake, '--install', build, '--prefix', prefix)
    manifest = (build / 'install_manifest.txt').read_text().splitlines()
    forbidden = {'plan.md', 'agent.md', 'AGENTS.md', '.private', '.git', 'adapters'}
    for installed in manifest:
        path = Path(installed)
        if not path.is_relative_to(prefix) or forbidden.intersection(path.parts):
            raise RuntimeError('unexpected file in installed manifest')
    # Relocating the prefix, including a space, catches absolute build/source
    # references in the exported package. Consumers get only installed headers.
    shutil.copytree(prefix, relocated)
    consumer = work / 'consumer'
    consumer.mkdir()
    (consumer / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(consumer LANGUAGES CXX)
find_package(stateguard 0.1.1 EXACT CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE stateguard::stateguard)
''')
    (consumer / 'main.cpp').write_text('''#include <stateguard/stateguard.hpp>
int main() {
  stateguard::Trace trace;
  trace.events.emplace_back(stateguard::EndTraceEvent{true});
  return stateguard::check(trace).clean() ? 0 : 1;
}
''')
    consumer_build = work / 'consumer-build'
    run(args.cmake, '-S', consumer, '-B', consumer_build,
        f'-DCMAKE_PREFIX_PATH={relocated}', *common)
    run(args.cmake, '--build', consumer_build, '--parallel', '2')
    run(consumer_build / 'consumer')
    tools, traces = relocated / 'bin', relocated / 'share/stateguard/traces'
    safe = run(tools / 'stateguard-check', traces / 'safe_deferred_reclaim.sgtrace')
    if safe != 'clean\n':
        raise RuntimeError('installed checker did not accept safe trace')
    bad = run(tools / 'stateguard-check', traces / 'premature_reclaim.sgtrace', expected=1)
    if 'premature_reclaim' not in bad:
        raise RuntimeError('installed checker lost failure category')
    timeline = run(tools / 'stateguard-timeline', traces / 'cancellation_timeline.sgtrace')
    if 'cancellation_to_reuse 5500 ns' not in timeline or '0.02048 byte-seconds' not in timeline:
        raise RuntimeError('installed timeline output differs from known interval')
    generated = work / 'generated.sgtrace'
    run(tools / 'stateguard-generate', '--seed', '7', '--fault', 'premature_reclaim',
        '--output', generated)
    reduced = work / 'reduced.sgtrace'
    run(tools / 'stateguard-reduce', '--output', reduced, generated)
    run(tools / 'stateguard-check', reduced, expected=1)
    run(tools / 'stateguard-cancellation-demo', '--delay-ms', '1', '--trace-dir', work)
    run(tools / 'stateguard-async-copy-demo', '--trace-dir', work)
    run(tools / 'stateguard-instrumented-demo')
    # An unrelated file must survive uninstall. Only the manifest is removed.
    keep = prefix / 'unrelated.txt'
    keep.write_text('keep\n')
    run(args.cmake, '--build', build, '--target', 'uninstall')
    if any(Path(path).exists() for path in manifest) or keep.read_text() != 'keep\n':
        raise RuntimeError('uninstall did not preserve the manifest boundary')
    print(f'PASS: {len(manifest)} installed files; relocatable CMake consumer; '
          '4 CLIs; 3 demos; samples; manifest-only uninstall')


if __name__ == '__main__':
    main()
