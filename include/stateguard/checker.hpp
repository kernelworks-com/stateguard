#pragma once

#include "stateguard/types.hpp"

#include <cstddef>
#include <string>
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
  InvalidStateTransition,
  IncompleteEvidence,
  MalformedTrace,
};

struct Diagnostic {
  DiagnosticCategory category;
  std::size_t event_index = 0;
  std::string message;
  std::vector<std::string> witness;
};

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

}  // namespace stateguard

