"""Optional, pinned LMCache static NIXL put instrumentation."""
from .adapter import instrument
from .trace import TraceWriter

__all__ = ['TraceWriter', 'instrument']
