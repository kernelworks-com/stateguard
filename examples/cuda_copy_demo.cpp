// Runs the CUDA copy scenario under safe and induced-bug configurations,
// checks every captured trace, and reports whether the device delivered the
// staged bytes. Needs a CUDA device; built only with STATEGUARD_ENABLE_CUDA.
//
// usage: stateguard-cuda-copy-demo [--repeat N] [--mib N] [--delay-ms N]
//                                  [--trace-dir DIR]
//
// The checker verdict is expected to be the same on every run: clean for
// deferred reclamation, premature_reclaim for immediate reclamation. Whether
// the early reclaim actually corrupted the copy depends on device timing and
// is reported, not required. With --trace-dir, the last trace of each
// configuration is written to DIR/cuda-<name>.sgtrace. Exits 0 when every
// verdict matches, every deferred run delivered intact bytes, and no CUDA
// call failed.

#include "cuda_copy.hpp"
#include "stateguard/stateguard.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace stateguard;
using namespace stateguard::harness;
using namespace stateguard::harness::cuda;

struct Configuration {
  std::string name;
  std::string label;
  GpuScenarioOptions options;
  CheckStatus expected;
};

std::optional<long> parse_count(const char* text) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 0 || value > 100000) return std::nullopt;
  return value;
}

std::string version(int encoded) {
  return std::to_string(encoded / 1000) + '.' + std::to_string(encoded % 1000 / 10);
}

const Diagnostic* first_confirmed(const CheckReport& report) {
  for (const auto& diagnostic : report.diagnostics) {
    if (diagnostic.category != DiagnosticCategory::IncompleteEvidence && diagnostic.confirmed) {
      return &diagnostic;
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  long repeat = 10;
  long mib = 64;
  long delay_ms = 20;
  std::optional<std::string> trace_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    std::optional<long> value;
    if (i + 1 < argc && flag == "--trace-dir") {
      trace_dir = argv[++i];
      continue;
    }
    if (i + 1 < argc) value = parse_count(argv[i + 1]);
    if (flag == "--repeat" && value && *value > 0) {
      repeat = *value;
    } else if (flag == "--mib" && value && *value > 0 && *value <= 1024) {
      mib = *value;
    } else if (flag == "--delay-ms" && value && *value <= 10000) {
      delay_ms = *value;
    } else {
      std::cerr << "usage: " << argv[0]
                << " [--repeat N] [--mib N] [--delay-ms N] [--trace-dir DIR]\n";
      return 3;
    }
    ++i;
  }

  try {
    const auto device = device_info();
    std::cout << "device: " << device.device << ' ' << device.name << " (compute "
              << device.compute_major << '.' << device.compute_minor << "), driver CUDA "
              << version(device.driver_version) << ", runtime CUDA "
              << version(device.runtime_version) << '\n';
    std::cout << "workload: " << mib << " MiB device-to-device copy, " << repeat
              << " runs per configuration\n";

    std::vector<Configuration> configurations;
    for (const bool delayed : {false, true}) {
      if (delayed && delay_ms == 0) continue;
      const std::chrono::milliseconds delay(delayed ? delay_ms : 0);
      const std::string suffix = delayed ? "-delayed" : "";
      const std::string timing =
          delayed ? ", copy start delayed " + std::to_string(delay_ms) + " ms" : "";
      for (const auto policy : {ReclaimPolicy::Deferred, ReclaimPolicy::Immediate}) {
        GpuScenarioOptions options;
        options.reclaim = policy;
        options.engine.start_delay = delay;
        options.slot_bytes = static_cast<std::size_t>(mib) << 20;
        const bool deferred = policy == ReclaimPolicy::Deferred;
        configurations.push_back(
            {(deferred ? "safe" : "immediate") + suffix,
             (deferred ? "safe deferred reclamation" : "induced immediate reclamation") +
                 timing,
             options, deferred ? CheckStatus::Clean : CheckStatus::Violation});
      }
    }

    bool as_expected = true;
    for (const auto& configuration : configurations) {
      long matched = 0;
      long intact = 0;
      long pending_at_reclaim = 0;
      std::size_t max_corrupt = 0;
      std::optional<std::string> first_finding;
      std::optional<Trace> last_trace;
      for (long run_index = 0; run_index < repeat; ++run_index) {
        const auto run = run_gpu_scenario(configuration.options);
        const auto report = check(run.trace);
        if (report.status == configuration.expected) ++matched;
        if (run.delivered_intact) ++intact;
        if (run.reclaimed_while_pending) ++pending_at_reclaim;
        if (run.corrupt_bytes > max_corrupt) max_corrupt = run.corrupt_bytes;
        if (const auto* finding = first_confirmed(report); finding && !first_finding) {
          first_finding = std::to_string(finding->event_index) + ' ' +
                          to_string(finding->category) + ": " + finding->message;
        }
        last_trace = run.trace;
      }
      const bool deferred = configuration.options.reclaim == ReclaimPolicy::Deferred;
      std::cout << configuration.label << '\n'
                << "  checker: " << matched << '/' << repeat << ' '
                << to_string(configuration.expected) << '\n';
      if (first_finding) std::cout << "  first finding: " << *first_finding << '\n';
      std::cout << "  device: copy still pending at reclaim in " << pending_at_reclaim << '/'
                << repeat << "; cache intact in " << intact << '/' << repeat
                << "; most corrupt bytes in one run " << max_corrupt << '\n';
      if (matched != repeat || (deferred && intact != repeat)) {
        std::cout << "  unexpected result\n";
        as_expected = false;
      }
      if (trace_dir && last_trace) {
        const auto path = *trace_dir + "/cuda-" + configuration.name + ".sgtrace";
        std::ofstream output(path);
        output << serialize_trace(*last_trace);
        if (!output) {
          std::cerr << "cannot write " << path << '\n';
          return 3;
        }
      }
    }
    return as_expected ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
