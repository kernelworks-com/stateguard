#pragma once

#include "stateguard/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stateguard {

enum class CheckStatus { Clean, Violation, Inconclusive };

enum class DiagnosticCategory {
  StaleGeneration,
  IncompatibleSemanticReuse,
  WriteThroughSharedImmutableState,
  PrematureReclaim,
  OrphanedOperation,
  DoubleCompletion,
  // A state was published from an allocation whose last write failed.
  FailedWritePublication,
  InvalidStateTransition,
  IncompleteEvidence,
  MalformedTrace,
};

struct Diagnostic {
  DiagnosticCategory category;
  std::size_t event_index = 0;
  std::string message;
  std::vector<std::string> witness;
  // False when events may be missing before this finding: it follows a
  // coverage gap, or the trace ended incomplete without saying where events
  // were lost. A missing event (a completion or cancellation, say) could
  // explain an unconfirmed finding, so it makes the trace inconclusive rather
  // than a violation. Incomplete-evidence diagnostics are always confirmed.
  bool confirmed = true;
};

// A trace is a Violation only when it has a confirmed definite finding.
struct CheckReport {
  CheckStatus status = CheckStatus::Inconclusive;
  bool capture_complete = false;
  std::vector<Diagnostic> diagnostics;

  bool clean() const { return status == CheckStatus::Clean; }
};

class Checker {
 public:
  CheckReport check(const Trace& trace) const;
};

CheckReport check(const Trace& trace);

std::string to_string(CheckStatus status);
std::string to_string(DiagnosticCategory category);

// Accepts the names produced by to_string(DiagnosticCategory).
std::optional<DiagnosticCategory> parse_category(std::string_view name);

}  // namespace stateguard

