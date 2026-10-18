// Runs the instrumented async-copy harness under six configurations and
// checks each captured trace. Every copy completes as late as the host allows,
// which is the schedule where an early reclaim does the most damage.
//
// usage: stateguard-async-copy-demo [--trace-dir DIR]
//
// With --trace-dir, each captured trace is also written to DIR/<name>.sgtrace
// for stateguard-check and stateguard-reduce. Exits 0 when every result
// matches its expectation.

#include "async_copy.hpp"
#include "stateguard/stateguard.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace stateguard;
using namespace stateguard::harness;

struct Configuration {
  const char* name;
  const char* label;
  ScenarioOptions options;
  CheckStatus expected;
};

std::vector<Configuration> configurations() {
  ScenarioOptions safe;
  ScenarioOptions immediate;
  immediate.host.reclaim = ReclaimPolicy::Immediate;
  ScenarioOptions overflow;
  overflow.capture_capacity = 16;
  ScenarioOptions no_completion;
  no_completion.engine.reports_completion = false;
  ScenarioOptions failed_retry;
  failed_retry.engine.failing_operations = {"load-1"};
  ScenarioOptions failed_publish = failed_retry;
  failed_publish.host.publish_failed_loads = true;
  return {
      {"safe", "safe deferred reclamation", safe, CheckStatus::Clean},
      {"immediate", "induced immediate reclamation", immediate, CheckStatus::Violation},
      {"overflow", "safe run with a 16-event capture buffer", overflow,
       CheckStatus::Inconclusive},
      {"no-completion", "safe run on an engine without completion events", no_completion,
       CheckStatus::Inconclusive},
      {"failed-retry", "failed load retried before publication", failed_retry,
       CheckStatus::Clean},
      {"failed-publish", "induced publication of a failed load", failed_publish,
       CheckStatus::Violation},
  };
}

void print_finding(const Diagnostic& diagnostic) {
  std::cout << "  first finding: " << diagnostic.event_index << ' '
            << to_string(diagnostic.category) << (diagnostic.confirmed ? "" : " [unconfirmed]")
            << ": " << diagnostic.message << '\n';
  for (const auto& witness : diagnostic.witness) std::cout << "    " << witness << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::string> trace_dir;
  if (argc == 3 && std::strcmp(argv[1], "--trace-dir") == 0) {
    trace_dir = argv[2];
  } else if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [--trace-dir DIR]\n";
    return 3;
  }

  bool as_expected = true;
  for (const auto& configuration : configurations()) {
    const auto run = run_scenario(configuration.options);
    const auto report = check(run.trace);
    std::cout << configuration.label << ": " << to_string(report.status) << '\n';

    // A clean trace has no findings. Otherwise show the first confirmed
    // finding, or the first finding when none is confirmed.
    const Diagnostic* first = nullptr;
    for (const auto& diagnostic : report.diagnostics) {
      if (diagnostic.category != DiagnosticCategory::IncompleteEvidence && diagnostic.confirmed) {
        first = &diagnostic;
        break;
      }
    }
    if (first == nullptr && !report.diagnostics.empty()) first = &report.diagnostics.front();
    if (first != nullptr) {
      print_finding(*first);
      std::cout << "  findings: " << report.diagnostics.size() << " (stateguard-check lists all)\n";
    }

    std::cout << "  capture: " << run.capture.recorded << " events kept, "
              << run.capture.dropped << " dropped, capacity " << run.capture.capacity
              << "; access completion "
              << (configuration.options.engine.reports_completion ? "recorded by the engine"
                                                                 : "not observed")
              << '\n';
    std::cout << "  harness: " << run.hazards.size() << " unsafe reclaim"
              << (run.hazards.size() == 1 ? "" : "s");
    if (run.failed_publications > 0) {
      std::cout << ", " << run.failed_publications << " failed load"
                << (run.failed_publications == 1 ? "" : "s") << " published";
    }
    std::cout << "; request-2 received "
              << (run.delivered_intact ? "prefix-1 intact" : "corrupted bytes") << '\n';

    if (trace_dir) {
      const auto path = *trace_dir + "/" + configuration.name + ".sgtrace";
      std::ofstream output(path);
      output << serialize_trace(run.trace);
      if (!output) {
        std::cerr << "cannot write " << path << '\n';
        return 3;
      }
    }
    if (report.status != configuration.expected) {
      std::cout << "  unexpected: wanted " << to_string(configuration.expected) << '\n';
      as_expected = false;
    }
  }
  return as_expected ? 0 : 1;
}
