// Runs the CUDA copy scenario under safe and induced-bug configurations and
// the CUDA cancellation scenario, checks every captured trace, builds its
// cancellation timeline, and reports whether the device delivered the staged
// bytes. Needs a CUDA device; built only with STATEGUARD_ENABLE_CUDA.
//
// usage: stateguard-cuda-copy-demo [--repeat N] [--mib N] [--delay-ms N]
//                                  [--trace-dir DIR]
//
// Every trace is stamped with the steady clock. The checker verdict is
// expected to be the same on every run: clean for deferred reclamation and
// for the cancellation, premature_reclaim for immediate reclamation. The
// timeline is expected complete on every run, and the cancellation scenario
// must report a cancellation-to-reuse interval. Whether the early reclaim
// actually corrupted the copy depends on device timing and is reported, not
// required. The timeline intervals printed are those of the last run, a
// measurement on this machine only. With --trace-dir, the last trace of each
// configuration is written to DIR/cuda-<name>.sgtrace. Exits 0 when every
// expectation holds, every deferred and cancellation run delivered intact
// bytes, and no CUDA call failed.

#include "cancellation.hpp"
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
#include <utility>
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
  // Runs run_gpu_cancellation with the same clock, engine, and slot size.
  bool cancellation = false;
};

// What one run of either scenario produced.
struct Outcome {
  Trace trace;
  bool delivered_intact = false;
  std::size_t corrupt_bytes = 0;
  // Copy pending at reclaim for the copy scenario; for the cancellation,
  // copy still running on the device when the cancellation was acknowledged.
  bool device_flag = false;
  bool reclaimed_while_pending = false;
};

Outcome run_configuration(const Configuration& configuration) {
  const auto& options = configuration.options;
  Outcome outcome;
  if (configuration.cancellation) {
    GpuCancellationOptions cancel;
    cancel.clock = options.clock;
    cancel.engine = options.engine;
    cancel.slot_bytes = options.slot_bytes;
    auto run = run_gpu_cancellation(cancel);
    outcome.trace = std::move(run.trace);
    outcome.delivered_intact = run.delivered_intact;
    outcome.corrupt_bytes = run.corrupt_bytes;
    outcome.device_flag = run.running_at_cancel;
    outcome.reclaimed_while_pending = run.reclaimed_while_pending;
  } else {
    auto run = run_gpu_scenario(options);
    outcome.trace = std::move(run.trace);
    outcome.delivered_intact = run.delivered_intact;
    outcome.corrupt_bytes = run.corrupt_bytes;
    outcome.device_flag = run.reclaimed_while_pending;
    outcome.reclaimed_while_pending = run.reclaimed_while_pending;
  }
  return outcome;
}

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
      GpuScenarioOptions options;
      options.clock = steady_trace_clock();
      options.engine.start_delay = delay;
      options.slot_bytes = static_cast<std::size_t>(mib) << 20;
      for (const auto policy : {ReclaimPolicy::Deferred, ReclaimPolicy::Immediate}) {
        options.reclaim = policy;
        const bool deferred = policy == ReclaimPolicy::Deferred;
        configurations.push_back(
            {(deferred ? "safe" : "immediate") + suffix,
             (deferred ? "safe deferred reclamation" : "induced immediate reclamation") +
                 timing,
             options, deferred ? CheckStatus::Clean : CheckStatus::Violation, false});
      }
      options.reclaim = ReclaimPolicy::Deferred;
      configurations.push_back({"cancel" + suffix,
                                "cancellation while the copy is in flight" + timing, options,
                                CheckStatus::Clean, true});
    }

    bool as_expected = true;
    for (const auto& configuration : configurations) {
      long matched = 0;
      long intact = 0;
      long device_flag = 0;
      long timeline_complete = 0;
      long with_interval = 0;
      long reclaimed_while_pending = 0;
      std::size_t max_corrupt = 0;
      std::optional<std::string> first_finding;
      std::optional<Trace> last_trace;
      CancellationSummary last_summary;
      for (long run_index = 0; run_index < repeat; ++run_index) {
        auto run = run_configuration(configuration);
        const auto report = check(run.trace);
        // The checker ignores timestamps; the timeline reads them.
        const auto summary = summarize_cancellation(build_timeline(run.trace));
        if (report.status == configuration.expected) ++matched;
        if (summary.status == TimelineStatus::Complete) ++timeline_complete;
        if (summary.cancellation_to_reuse_ns) ++with_interval;
        if (run.delivered_intact) ++intact;
        if (run.device_flag) ++device_flag;
        if (run.reclaimed_while_pending) ++reclaimed_while_pending;
        if (run.corrupt_bytes > max_corrupt) max_corrupt = run.corrupt_bytes;
        if (const auto* finding = first_confirmed(report); finding && !first_finding) {
          first_finding = std::to_string(finding->event_index) + ' ' +
                          to_string(finding->category) + ": " + finding->message;
        }
        last_trace = std::move(run.trace);
        last_summary = summary;
      }
      const bool deferred = configuration.options.reclaim == ReclaimPolicy::Deferred;
      const bool cancellation = configuration.cancellation;
      std::cout << configuration.label << '\n'
                << "  checker: " << matched << '/' << repeat << ' '
                << to_string(configuration.expected) << '\n';
      if (first_finding) std::cout << "  first finding: " << *first_finding << '\n';
      if (cancellation) {
        std::cout << "  device: copy still running at cancellation in " << device_flag << '/'
                  << repeat << "; output intact in " << intact << '/' << repeat
                  << "; reclaimed while pending in " << reclaimed_while_pending << '/'
                  << repeat << '\n';
      } else {
        std::cout << "  device: copy still pending at reclaim in " << device_flag << '/'
                  << repeat << "; cache intact in " << intact << '/' << repeat
                  << "; most corrupt bytes in one run " << max_corrupt << '\n';
      }
      std::cout << "  timeline: " << timeline_complete << '/' << repeat << " complete";
      if (cancellation) {
        std::cout << ", cancellation_to_reuse in " << with_interval << '/' << repeat;
      }
      std::cout << "\n  timeline of the last run (steady clock, measured on this machine): "
                << to_string(last_summary) << '\n';
      if (matched != repeat || (deferred && intact != repeat) || timeline_complete != repeat ||
          (cancellation && (with_interval != repeat || reclaimed_while_pending != 0))) {
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
