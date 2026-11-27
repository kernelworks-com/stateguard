"""Run `stateguard-check` and `stateguard-timeline` with --json.

The returned dicts are the tools' JSON objects; their keys are listed in the
"Machine-readable output" section of docs/REFERENCE.md.
"""
import json
import os
import shutil
import subprocess

from .writer import HEADER, TraceWriter


class ToolError(RuntimeError):
    """A Stateguard tool could not produce a result."""


class ToolNotFoundError(ToolError):
    """The tool executable was not found."""


class TraceInputError(ToolError):
    """The tool rejected its input (exit 3): unreadable or malformed trace.

    `message` is the tool's stderr message, `exit_code` is 3.
    """

    def __init__(self, tool, message):
        super().__init__(f'{tool}: {message}')
        self.tool = tool
        self.message = message
        self.exit_code = 3


def _is_text(value):
    return '\n' in value or value.startswith(HEADER)


def _input(trace):
    """Return (argument, stdin text) for a path, trace text, or TraceWriter."""
    if isinstance(trace, TraceWriter):
        return '-', trace.getvalue()
    if isinstance(trace, bytes):
        return '-', trace.decode('utf-8')
    if isinstance(trace, os.PathLike):
        return os.fspath(trace), None
    if isinstance(trace, str):
        return ('-', trace) if _is_text(trace) else (trace, None)
    raise TypeError('trace must be a path, trace text, bytes, or TraceWriter')


def _resolve(tool):
    found = shutil.which(tool)
    if found is None and os.path.dirname(tool) and os.path.isfile(tool):
        found = tool
    if found is None:
        raise ToolNotFoundError(f'{tool} not found; put the Stateguard tools on PATH '
                                'or pass the executable path')
    return found


def _run(tool, options, trace, results):
    executable = _resolve(tool)
    argument, stdin = _input(trace)
    try:
        # Bytes in both directions: no newline translation on Windows.
        completed = subprocess.run(
            [executable, '--json', *options, argument],
            input=None if stdin is None else stdin.encode('utf-8'),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        raise ToolNotFoundError(f'{tool} could not be started: {error}') from error
    stderr = completed.stderr.decode('utf-8', 'replace').strip()
    if completed.returncode == 3:
        raise TraceInputError(tool, stderr or 'input error')
    if completed.returncode not in results:
        raise ToolError(f'{tool} exited {completed.returncode}: {stderr}')
    try:
        report = json.loads(completed.stdout.decode('utf-8'))
    except ValueError as error:
        raise ToolError(f'{tool} printed no JSON object: {error}') from error
    if not isinstance(report, dict):
        raise ToolError(f'{tool} printed JSON that is not an object')
    return report


def check(path_or_text, checker='stateguard-check', primary_only=False):
    """Check a trace and return the checker's JSON report as a dict.

    path_or_text is a path (str or os.PathLike), trace text (a str that
    contains a newline or starts with the header), bytes, or a TraceWriter;
    text is passed on standard input. checker is a name on PATH or a path.
    Exit codes 0, 1, and 2 are results (`report['exit_code']`). Raises
    ToolNotFoundError when the checker cannot be run, TraceInputError on exit
    3, and ToolError on any other failure.
    """
    options = ['--primary-only'] if primary_only else []
    return _run(checker, options, path_or_text, (0, 1, 2))


def timeline(path_or_text, tool='stateguard-timeline'):
    """Build the cancellation timeline and return its JSON report as a dict.

    Inputs, tool resolution, and exceptions are as for check(). Exit 0
    (complete) and 2 (incomplete) are results.
    """
    return _run(tool, [], path_or_text, (0, 2))
