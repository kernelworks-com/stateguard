"""Shared test setup: import the package from this checkout and find the tools.

Tools are looked up in STATEGUARD_BUILD_DIR when it is set (CTest sets it),
otherwise on PATH. Without either, tests that need a tool are skipped.
"""
import os
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'python'))

SPEC = ROOT / 'docs' / 'TRACE-FORMAT.md'
FIXTURES = ROOT / 'tests' / 'fixtures'
BUILD_DIR = os.environ.get('STATEGUARD_BUILD_DIR')


def tool(name):
    """Return the tool's path, None if it is unavailable, or raise under CTest."""
    if BUILD_DIR:
        for candidate in (name, name + '.exe'):
            path = Path(BUILD_DIR) / candidate
            if path.is_file():
                return str(path.resolve())
        raise FileNotFoundError(f'{name} not found in STATEGUARD_BUILD_DIR={BUILD_DIR}')
    return shutil.which(name)


def spec_traces():
    """Each fenced text block of the specification that starts with the header."""
    import re
    text = SPEC.read_text(encoding='utf-8')
    for match in re.finditer(r'^```text\n(.*?)^```', text, re.S | re.M):
        body = match.group(1)
        if body.startswith('STATEGUARD_TRACE|1\n'):
            yield f'{SPEC.name}:{text.count(chr(10), 0, match.start()) + 1}', body
