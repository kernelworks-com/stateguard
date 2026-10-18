#pragma once

#include "stateguard/checker.hpp"
#include "stateguard/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stateguard {

// A deliberate violation the generator can plant in an otherwise legal history.
// Evidence faults (DroppedCompletion, CoverageGap, IncompleteEnd) model capture
// loss and must make the result inconclusive rather than clean.
enum class Fault {
  None,
  PrematureReclaim,
  StaleGeneration,
  IncompatibleSemanticReuse,
  WriteThroughSharedImmutableState,
  DoubleCompletion,
  OrphanedCompletion,
  PublishBeforeWriterCompletion,
  DroppedCompletion,
  CoverageGap,
  IncompleteEnd,
  // A writer's access fails, and the state is published from it anyway.
  FailedWritePublication,
};

struct GeneratorConfig {
  std::uint64_t seed = 0;
  // Random lifecycle actions taken before the history is drained.
  std::size_t steps = 64;
  // Distinct allocation identities; each can be reused across generations.
  std::size_t allocations = 4;
  // Distinct semantic states. Several allocations may hold the same state.
  std::size_t prefixes = 3;
  // Request identities available to submit, consume, and cancel.
  std::size_t requests = 8;
  Fault fault = Fault::None;
};

struct GeneratedTrace {
  Trace trace;
  GeneratorConfig config;
  // The checker result the planted fault must produce (Clean for Fault::None).
  CheckStatus expected_status = CheckStatus::Clean;
  std::optional<DiagnosticCategory> expected_category;
  // Index of the event at which the checker should report the fault.
  std::optional<std::size_t> fault_event;
};

// Produces a bounded, causally valid lifecycle history from config.seed. The
// output depends only on the config, not on the platform or standard library.
// Throws std::invalid_argument for out-of-range configuration values.
GeneratedTrace generate_trace(const GeneratorConfig& config);

// Every fault kind, including Fault::None, in declaration order.
const std::vector<Fault>& fault_kinds();
std::string to_string(Fault fault);
std::optional<Fault> parse_fault(std::string_view name);

}  // namespace stateguard
