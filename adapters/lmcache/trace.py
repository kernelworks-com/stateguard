"""Re-export of the stateguard_trace writer used by this adapter.

Install it with `pip install -e python/`; when it is not installed, the copy
in this checkout's python/ directory is used.
"""
from pathlib import Path
import sys

try:
    import stateguard_trace  # noqa: F401
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'python'))

from stateguard_trace import MAX_UINT, TraceWriter, identifier, uint  # noqa: E402

__all__ = ['MAX_UINT', 'TraceWriter', 'identifier', 'uint']
