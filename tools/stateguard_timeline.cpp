#include "stateguard/stateguard.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program << " TRACE\n";
}

// Times are printed relative to the trace's first timestamp.
std::string at(const std::optional<std::uint64_t>& ns, std::uint64_t origin) {
  if (!ns) return "unknown";
  return "+" + std::to_string(*ns - origin) + " ns";
}

std::string ref(const stateguard::AllocationRef& r) {
  return r.allocation.value + "#" + std::to_string(r.generation);
}

std::string byte_seconds(double value) {
  char text[64];
  std::snprintf(text, sizeof text, "%.6g", value);
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    usage(argv[0]);
    return 3;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "stateguard-timeline: cannot open " << argv[1] << '\n';
    return 3;
  }
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  stateguard::Trace trace;
  try {
    trace = stateguard::parse_trace(contents);
  } catch (const stateguard::TraceParseError& error) {
    std::cerr << "malformed_trace: " << error.what() << '\n';
    return 3;
  }
  const auto report = stateguard::build_timeline(trace);
  const auto origin = report.first_ns.value_or(0);
  std::cout << stateguard::to_string(report.status) << '\n';
  if (!report.clock.empty()) std::cout << "clock " << report.clock << '\n';
  for (const auto& r : report.requests) {
    std::cout << "request " << r.request.value << '\n'
              << "  signal " << at(r.signal_ns, origin) << '\n'
              << "  ack " << at(r.ack_ns, origin) << '\n'
              << "  last_access " << at(r.last_access_ns, origin) << '\n';
    if (!r.cancelled_only.empty()) {
      std::cout << "  eligible " << at(r.eligible_ns, origin) << '\n'
                << "  reuse " << at(r.reuse_ns, origin) << '\n';
      const auto start = r.signal_ns ? r.signal_ns : r.ack_ns;
      if (start && r.reuse_ns) {
        std::cout << "  cancellation_to_reuse " << (*r.reuse_ns - *start) << " ns\n";
      }
    }
    for (const auto& a : r.cancelled_only) std::cout << "  cancelled_only " << ref(a) << '\n';
    for (const auto& a : r.not_attributed) std::cout << "  not_attributed " << ref(a) << '\n';
  }
  for (const auto& a : report.allocations) {
    std::cout << "allocation " << ref(a.allocation) << " bytes " << a.bytes << " requests";
    for (const auto& r : a.requests) std::cout << ' ' << r.value;
    std::cout << '\n'
              << "  last_access " << at(a.last_access_ns, origin) << '\n'
              << "  eligible " << at(a.eligible_ns, origin) << '\n'
              << "  reclaim " << at(a.reclaim_ns, origin) << '\n'
              << "  reuse " << at(a.reuse_ns, origin) << '\n';
    if (a.reclaimed_before_eligible) std::cout << "  reclaimed_before_eligible\n";
    if (a.cancelled_only) {
      std::cout << "  cancelled_only " << a.cancelled_only_ns << " ns "
                << byte_seconds(a.cancelled_only_byte_seconds) << " byte-seconds"
                << (a.retained_at_end ? " (retained at end, lower bound)" : "") << '\n';
    }
  }
  std::cout << "cancelled_only_total " << byte_seconds(report.cancelled_only_byte_seconds)
            << " byte-seconds, peak " << report.peak_cancelled_only_bytes << " bytes\n";
  for (const auto& reason : report.incomplete) std::cout << "incomplete: " << reason << '\n';
  return report.status == stateguard::TimelineStatus::Complete ? 0 : 2;
}
