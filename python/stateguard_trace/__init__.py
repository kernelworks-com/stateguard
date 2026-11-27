"""Write Stateguard `.sgtrace` lifecycle traces (schema 1) and check them.

The format is specified in docs/TRACE-FORMAT.md. TraceWriter collects a
trace in memory, FileTraceWriter streams it to a file; both validate every
value and write the end marker when used as a context manager. check() and
timeline() run the command-line tools with --json.
"""
from .tools import ToolError, ToolNotFoundError, TraceInputError, check, timeline
from .writer import (HEADER, MAX_UINT, Descriptor, FileTraceWriter, Route, TraceWriter,
                     identifier, text, uint)

__version__ = '0.3.0'

__all__ = [
    'Descriptor', 'FileTraceWriter', 'HEADER', 'MAX_UINT', 'Route', 'ToolError',
    'ToolNotFoundError', 'TraceInputError', 'TraceWriter', 'check', 'identifier',
    'text', 'timeline', 'uint',
]
