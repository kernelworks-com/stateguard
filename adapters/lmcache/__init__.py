"""Optional, pinned LMCache NIXL put instrumentation (static and dynamic backends)."""
from .adapter import instrument
from .trace import TraceWriter

__all__ = ['TraceWriter', 'instrument']
