"""check() and timeline() against the built tools, and their error paths."""
from pathlib import Path
import sys
import tempfile
import unittest

import _support
from stateguard_trace import (ToolError, ToolNotFoundError, TraceInputError, TraceWriter,
                              check, timeline)

CHECKER = _support.tool('stateguard-check')
TIMELINE = _support.tool('stateguard-timeline')
FIXTURES = _support.FIXTURES


def timed_history(clock, clock_name='test'):
    """A cancelled request whose copy completes before the buffer is reused."""
    writer = TraceWriter(clock=clock, clock_name=clock_name)
    writer.allocate('buffer-1', 1, 4096, 'cpu')
    writer.submit('copy-1', 'request-1', [('r', 'buffer-1', 1)])
    writer.cancel_signal('request-1')
    writer.cancel('request-1')
    writer.retire('buffer-1', 1)
    writer.complete('copy-1')
    writer.reclaim('buffer-1', 1)
    writer.allocate('buffer-1', 2, 4096, 'cpu')
    return writer


@unittest.skipUnless(CHECKER, 'stateguard-check not on PATH and STATEGUARD_BUILD_DIR unset')
class Check(unittest.TestCase):
    def test_results_by_path(self):
        clean = check(FIXTURES / 'safe_deferred_reclaim.sgtrace', CHECKER)
        self.assertEqual((clean['status'], clean['exit_code'], clean['diagnostics']),
                         ('clean', 0, []))
        self.assertTrue(clean['capture_complete'])
        bad = check(str(FIXTURES / 'premature_reclaim.sgtrace'), checker=CHECKER)
        self.assertEqual((bad['status'], bad['exit_code']), ('violation', 1))
        self.assertEqual(bad['diagnostics'][0]['category'], 'premature_reclaim')
        gap = check(FIXTURES / 'coverage_gap_unconfirmed.sgtrace', CHECKER)
        self.assertEqual((gap['status'], gap['exit_code']), ('inconclusive', 2))

    def test_text_and_writer_go_through_stdin(self):
        writer = TraceWriter()
        writer.allocate('buffer-1', 1, 4096, 'cpu')
        writer.submit('copy-1', 'request-1', [('r', 'buffer-1', 1)])
        writer.retire('buffer-1', 1)
        writer.reclaim('buffer-1', 1)
        self.assertEqual(check(writer, CHECKER)['status'], 'violation')
        text = writer.end()
        report = check(text, CHECKER)
        self.assertEqual(report['diagnostics'][0]['category'], 'premature_reclaim')
        self.assertEqual(check(text.encode(), CHECKER)['exit_code'], 1)
        # A CRLF trace is accepted: the reader strips a trailing carriage return.
        self.assertEqual(check(text.replace('\n', '\r\n'), CHECKER)['exit_code'], 1)

    def test_primary_only(self):
        path = FIXTURES / 'retry_acknowledges_other_attempt.sgtrace'
        full = check(path, CHECKER)
        primary = check(path, CHECKER, primary_only=True)
        self.assertTrue(any(d['follow_on'] for d in full['diagnostics']))
        self.assertFalse(any(d['follow_on'] for d in primary['diagnostics']))
        self.assertEqual(primary['exit_code'], full['exit_code'])

    def test_input_errors(self):
        with self.assertRaises(TraceInputError) as caught:
            check('STATEGUARD_TRACE|1\nbogus|x\n', CHECKER)
        self.assertEqual(caught.exception.exit_code, 3)
        self.assertTrue(caught.exception.message)
        with self.assertRaises(TraceInputError):
            check('STATEGUARD_TRACE|2\n', CHECKER)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(TraceInputError):
                check(Path(directory) / 'missing.sgtrace', CHECKER)

    def test_missing_and_broken_tools(self):
        with self.assertRaises(ToolNotFoundError):
            check(FIXTURES / 'premature_reclaim.sgtrace', 'stateguard-no-such-tool')
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ToolNotFoundError):
                check(FIXTURES / 'premature_reclaim.sgtrace', str(Path(directory) / 'nothing'))
        # A program that is not a Stateguard tool prints no JSON object.
        with self.assertRaisesRegex(ToolError, 'no JSON|exited'):
            check(FIXTURES / 'premature_reclaim.sgtrace', sys.executable)
        with self.assertRaises(TypeError):
            check(42, CHECKER)


@unittest.skipUnless(TIMELINE, 'stateguard-timeline not on PATH and STATEGUARD_BUILD_DIR unset')
class Timeline(unittest.TestCase):
    def test_fixture(self):
        report = timeline(FIXTURES / 'cancellation_timeline.sgtrace', TIMELINE)
        self.assertEqual((report['status'], report['clock']), ('complete', 'example'))
        self.assertEqual(report['requests'][0]['request'], 'request-1')
        self.assertEqual(report['requests'][0]['reuse_ns'], 6500)

    def test_writer_clock_feeds_the_timeline(self):
        readings = iter([0, 0, 1000, 1300, 1300, 6300, 6300, 6500, 6500])
        writer = timed_history(lambda: next(readings))
        report = timeline(writer.end(), TIMELINE)
        self.assertEqual((report['status'], report['clock']), ('complete', 'test'))
        request = report['requests'][0]
        self.assertEqual((request['signal_ns'], request['ack_ns'], request['reuse_ns']),
                         (1000, 1300, 6500))
        if CHECKER:
            self.assertEqual(check(writer, CHECKER)['status'], 'clean')

    def test_monotonic_clock(self):
        report = timeline(timed_history(True, 'monotonic').end(), TIMELINE)
        self.assertEqual((report['status'], report['clock']), ('complete', 'monotonic'))

    def test_untimed_trace_is_incomplete(self):
        report = timeline(timed_history(None).end(), TIMELINE)
        self.assertEqual(report['status'], 'incomplete')
        self.assertIsNone(report['first_ns'])

    def test_input_error(self):
        with self.assertRaises(TraceInputError):
            timeline('not a trace\n', TIMELINE)


if __name__ == '__main__':
    unittest.main()
