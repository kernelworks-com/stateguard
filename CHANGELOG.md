# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Stateguard is in an
experimental 0.x series: there is no ABI promise, and the installed CMake
package matches versions exactly.

## [Unreleased]

### Added

- Findings that exist only because the checker rejected an earlier event are
  marked `[follow-on]` (`Diagnostic::follow_on`, `"follow_on"` in JSON).
  `stateguard-check --primary-only` hides them. The status is unchanged.

## [0.1.0]

First experimental alpha. It runs on the CPU; an optional CUDA harness has been
checked on one NVIDIA T4.

### Added

- `stateguard-check`: checks a recorded `.sgtrace` and reports `clean`,
  `violation`, or `inconclusive` (exit codes 0, 1, 2; 3 for an input error).
  Checked rules cover stale allocation generations or mismatched state, reclaims
  while a consumer or operation still holds storage, writes to published
  immutable state, publishing before writers finish or after a failed write,
  invalid lifecycle transitions, orphaned operations, duplicate completions,
  completions acknowledged with evidence from another attempt or an earlier
  connection, reclaims after an unconfirmed timeout, and missing completion
  evidence or capture gaps. Findings after lost events are marked
  `[unconfirmed]`.
- `stateguard-reduce`: shrinks a failing trace to a small witness that
  reproduces the same diagnostic, deleting events only.
- `stateguard-timeline`: reports signal, acknowledgment, last access, reuse
  eligibility, and actual reuse for each cancelled request in a timestamped
  trace, plus memory retained only for cancelled requests.
- `stateguard-generate`: writes a deterministic synthetic history, optionally
  with one planted fault.
- `stateguard::TraceRecorder` and public headers for capturing events from
  native code into a fixed-size buffer; a capture that overflows is never
  reported `clean`.
- Async-copy harness and demo, and an optional CUDA copy demo
  (`-DSTATEGUARD_ENABLE_CUDA=ON`).
- Experimental LMCache static-put adapter, checked with CPU mocks at two pinned
  revisions.
- Installable CMake package (`find_package(stateguard 0.1.0 EXACT CONFIG)`),
  static by default or shared with `-DBUILD_SHARED_LIBS=ON`, with an
  `uninstall` target and `scripts/check_install.py`.

Tested configurations are listed in
[scope and limitations](docs/REFERENCE.md#scope-and-limitations).

[Unreleased]: https://github.com/kernelworks-com/stateguard/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/kernelworks-com/stateguard/releases/tag/v0.1.0
