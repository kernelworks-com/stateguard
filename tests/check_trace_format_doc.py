"""Run stateguard-check on every example trace in docs/TRACE-FORMAT.md.

Usage: check_trace_format_doc.py CHECKER DOC

Each fenced ```text block whose first line is the schema header must parse:
the checker has to exit with 0, 1, or 2, never 3 (input error).
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = 'STATEGUARD_TRACE|1'


def blocks(text):
    for match in re.finditer(r'^```text\n(.*?)^```', text, re.S | re.M):
        body = match.group(1)
        if body.startswith(HEADER + '\n'):
            yield text.count('\n', 0, match.start()) + 1, body


def main():
    checker, doc = sys.argv[1], Path(sys.argv[2])
    found = list(blocks(doc.read_text(encoding='utf-8')))
    if not found:
        print(f'no example traces found in {doc}')
        return 1
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for line, body in found:
            path = Path(tmp) / f'example-{line}.sgtrace'
            path.write_text(body, encoding='utf-8')
            result = subprocess.run([checker, str(path)], capture_output=True, text=True)
            status = result.stdout.splitlines()[0] if result.stdout else ''
            print(f'{doc.name}:{line}: exit {result.returncode} {status}')
            if result.returncode not in (0, 1, 2):
                failures += 1
                print(result.stdout + result.stderr, end='')
    print(f'{len(found)} example traces, {failures} rejected')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
