#pragma once

#include "stateguard/checker.hpp"
#include "stateguard/types.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

namespace stateguard {

class ReductionError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

// Decides whether a candidate trace still reproduces a chosen failure of an
// original trace under this checker version. A candidate reproduces it when:
//   - its status equals the original status;
//   - it reports the target diagnostic (same category, message, witness, and
//     confirmation);
//   - every diagnostic it reports was also reported for the original.
// The last rule rejects candidates where deletion manufactured a different
// failure or new missing evidence, such as an unknown allocation or an
// operation whose completion was removed.
class ReductionOracle {
 public:
  // Selects the first diagnostic of `category` (preferring a confirmed one), or
  // by default the first confirmed violation (the first diagnostic when the
  // trace is only inconclusive). Throws ReductionError when the trace is clean,
  // uses an unsupported schema, or has no suitable diagnostic.
  explicit ReductionOracle(const Trace& original,
                           std::optional<DiagnosticCategory> category = std::nullopt);

  bool reproduced_by(const Trace& candidate) const;
  bool reproduced_by(const CheckReport& candidate) const;

  const Diagnostic& target() const { return target_; }
  CheckStatus status() const { return status_; }

 private:
  CheckStatus status_ = CheckStatus::Violation;
  Diagnostic target_;
  std::vector<Diagnostic> original_findings_;
};

struct ReductionOptions {
  std::optional<DiagnosticCategory> category;
  // Upper bound on checker runs. When reached, the best trace so far is
  // returned and ReductionResult::one_minimal is false.
  std::size_t max_checks = 100000;
};

struct ReductionResult {
  // A subsequence of the original events: order is preserved and no event or
  // identifier is rewritten, so the reduced trace keeps the original's causal
  // order. It is a checker witness, not proof that an engine reproduces it.
  Trace trace;
  // Index in the original trace of each kept event, in increasing order.
  std::vector<std::size_t> kept_events;
  // The target diagnostic as reported for the original trace.
  Diagnostic target;
  CheckStatus status = CheckStatus::Violation;
  std::size_t original_events = 0;
  std::size_t checks = 0;
  // True when removing any single remaining event, or every event that
  // mentions one identity, loses the failure.
  bool one_minimal = false;
};

// Deterministically shrinks a failing trace: first to the causal slice of
// events connected to the target through shared allocation, state, operation,
// and request identities; then by repeating whole-identity-group removal, delta
// debugging, and single-event removal until none of them removes anything.
// Every candidate is validated by the oracle.
ReductionResult reduce_trace(const Trace& trace, const ReductionOptions& options = {});

}  // namespace stateguard
