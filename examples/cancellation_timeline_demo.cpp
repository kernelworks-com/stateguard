// Runs the cancellation scenario on the steady clock with an injected copy
// delay, then checks each trace and builds its cancellation timeline.
//
// usage: stateguard-cancellation-demo [--delay-ms N] [--trace-dir DIR]
//                                     [--measure N]
//
// With --trace-dir, the traces are written to DIR/cancel-<name>.sgtrace for
// stateguard-check and stateguard-timeline. With --measure, the scenario also
// runs N times (after one warmup) without the delay in each capture mode, and
// the time per run is printed as median, minimum, and maximum. Exits 0 when
// every result matches its expectation.

#include "cancellation.hpp"
#include "stateguard/stateguard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace stateguard;
using namespace stateguard::harness;

std::uint64_t steady_ns() { return steady_trace_clock().now_ns(); }

struct Expectation {
  const char* name;
  const char* label;
  CancellationOptions options;
  CheckStatus check;
  TimelineStatus timeline;
  std::size_t cancelled_only;
};

bool run_one(const Expectation& expected, std::uint64_t delay_ns,
             const std::optional<std::string>& trace_dir) {
  const auto run = run_cancellation(expected.options);
  if (trace_dir) {
    std::ofstream out(*trace_dir + "/cancel-" + expected.name + ".sgtrace");
    out << serialize_trace(run.trace);
    if (!out) {
      std::cerr << "cannot write trace " << expected.name << '\n';
      return false;
    }
  }
  const auto checked = check(run.trace);
  const auto timeline = build_timeline(run.trace);
  bool ok = checked.status == expected.check && timeline.status == expected.timeline &&
            timeline.requests.size() == 1 &&
            timeline.requests.front().cancelled_only.size() == expected.cancelled_only;
  std::cout << expected.name << ": " << expected.label << '\n'
            << "  check " << to_string(checked.status) << ", timeline "
            << to_string(timeline.status) << '\n';
  if (!timeline.requests.empty()) {
    const auto& r = timeline.requests.front();
    if (r.signal_ns && r.reuse_ns) {
      const auto interval = *r.reuse_ns - *r.signal_ns;
      std::cout << "  cancellation_to_reuse " << interval << " ns (injected delay "
                << delay_ns << " ns)\n";
      ok = ok && interval >= delay_ns;
    }
    std::cout << "  cancelled_only " << r.cancelled_only.size() << " allocations, "
              << timeline.cancelled_only_byte_seconds << " byte-seconds, peak "
              << timeline.peak_cancelled_only_bytes << " bytes\n";
  }
  for (const auto& reason : timeline.incomplete) std::cout << "  incomplete: " << reason << '\n';
  std::cout << "  " << (ok ? "as expected" : "UNEXPECTED") << '\n';
  return ok;
}

struct Sample {
  std::vector<double> us;
  std::size_t events = 0;
};

void print_distribution(const char* mode, Sample sample) {
  std::sort(sample.us.begin(), sample.us.end());
  const auto n = sample.us.size();
  const double median =
      n % 2 == 1 ? sample.us[n / 2] : (sample.us[n / 2 - 1] + sample.us[n / 2]) / 2;
  std::cout << "  " << mode << ": median " << median << " us, min " << sample.us.front()
            << " us, max " << sample.us.back() << " us, " << sample.events
            << " events per run\n";
}

void measure(std::size_t repeat) {
  CancellationOptions untimed;
  CancellationOptions timed;
  timed.clock = steady_trace_clock();
  // Each mode runs back to back after a warmup, on one thread.
  const auto sample = [repeat](const CancellationOptions& options, bool build) {
    Sample result;
    for (std::size_t i = 0; i <= repeat; ++i) {
      const auto start = steady_ns();
      const auto run = run_cancellation(options);
      if (build) {
        const auto timeline = build_timeline(run.trace);
        if (timeline.status != TimelineStatus::Complete) throw std::runtime_error("timeline");
      }
      const auto stop = steady_ns();
      result.events = run.trace.events.size();
      if (i != 0) result.us.push_back(static_cast<double>(stop - start) / 1000.0);
    }
    return result;
  };
  std::cout << "measurement: " << repeat << " runs per mode, no injected delay\n";
  print_distribution("capture without timestamps", sample(untimed, false));
  print_distribution("capture with timestamps", sample(timed, false));
  print_distribution("capture with timestamps and timeline", sample(timed, true));
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t delay_ms = 20;
  std::size_t repeat = 0;
  std::optional<std::string> trace_dir;
  for (int i = 1; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (std::strcmp(argv[i], "--delay-ms") == 0 && has_value) {
      delay_ms = std::stoull(argv[++i]);
    } else if (std::strcmp(argv[i], "--trace-dir") == 0 && has_value) {
      trace_dir = argv[++i];
    } else if (std::strcmp(argv[i], "--measure") == 0 && has_value) {
      repeat = std::stoull(argv[++i]);
    } else {
      std::cerr << "usage: " << argv[0]
                << " [--delay-ms N] [--trace-dir DIR] [--measure N]\n";
      return 2;
    }
  }
  const auto delay = std::chrono::milliseconds(delay_ms);
  const std::uint64_t delay_ns = delay_ms * 1000000;
  CancellationOptions delayed;
  delayed.clock = steady_trace_clock();
  delayed.copy_delay = [delay] { std::this_thread::sleep_for(delay); };
  CancellationOptions shared = delayed;
  shared.shared_live_consumer = true;
  CancellationOptions missing = delayed;
  missing.complete_copy = false;

  const std::vector<Expectation> expectations{
      {"delayed", "copy completes after the cancellation", delayed, CheckStatus::Clean,
       TimelineStatus::Complete, 2},
      {"shared", "a live request shares the prefix", shared, CheckStatus::Clean,
       TimelineStatus::Complete, 1},
      {"missing", "the copy never completes", missing, CheckStatus::Inconclusive,
       TimelineStatus::Incomplete, 2},
  };
  bool ok = true;
  for (const auto& expected : expectations) ok = run_one(expected, delay_ns, trace_dir) && ok;
  if (repeat != 0) measure(repeat);
  return ok ? 0 : 1;
}
