#include "stateguard/stateguard.hpp"

#include "cli.hpp"

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program << " [--json] TRACE\n";
}

void help(const char* program) {
  std::cout << "usage: " << program << " [--json] TRACE\n"
               "\n"
               "Print the cancellation timeline of a timestamped .sgtrace. TRACE may\n"
               "be - for standard input.\n"
               "\n"
               "options:\n"
               "  --json     print one JSON object instead of text\n"
               "  --help     print this help and exit\n"
               "  --version  print the version and exit\n"
               "\n"
               "exit codes:\n"
               "  0  complete\n"
               "  2  incomplete\n"
               "  3  usage error, unreadable or malformed trace\n";
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

std::string json_ref(const stateguard::AllocationRef& r) {
  return "{\"allocation\":" + stateguard::cli::json_string(r.allocation.value) +
         ",\"generation\":" + std::to_string(r.generation) + "}";
}

template <typename T, typename F>
std::string json_list(const std::vector<T>& items, F item) {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + item(items[i]);
  return out + "]";
}

void print_json(const stateguard::TimelineReport& report) {
  using stateguard::cli::json_bool;
  using stateguard::cli::json_number;
  using stateguard::cli::json_optional;
  using stateguard::cli::json_string;
  const auto request_id = [](const stateguard::RequestId& r) { return json_string(r.value); };
  std::cout << "{\"status\":" << json_string(stateguard::to_string(report.status))
            << ",\"clock\":" << json_string(report.clock)
            << ",\"first_ns\":" << json_optional(report.first_ns)
            << ",\"last_ns\":" << json_optional(report.last_ns) << ",\"requests\":"
            << json_list(report.requests, [&](const stateguard::RequestTimeline& r) {
                 return "{\"request\":" + request_id(r.request) +
                        ",\"signal_ns\":" + json_optional(r.signal_ns) +
                        ",\"ack_ns\":" + json_optional(r.ack_ns) +
                        ",\"last_access_ns\":" + json_optional(r.last_access_ns) +
                        ",\"cancelled_only\":" + json_list(r.cancelled_only, json_ref) +
                        ",\"not_attributed\":" + json_list(r.not_attributed, json_ref) +
                        ",\"eligible_ns\":" + json_optional(r.eligible_ns) +
                        ",\"reuse_ns\":" + json_optional(r.reuse_ns) + "}";
               })
            << ",\"allocations\":"
            << json_list(report.allocations, [&](const stateguard::AllocationTimeline& a) {
                 return "{\"allocation\":" + json_ref(a.allocation) +
                        ",\"bytes\":" + std::to_string(a.bytes) +
                        ",\"requests\":" + json_list(a.requests, request_id) +
                        ",\"last_access_ns\":" + json_optional(a.last_access_ns) +
                        ",\"eligible_ns\":" + json_optional(a.eligible_ns) +
                        ",\"reclaim_ns\":" + json_optional(a.reclaim_ns) +
                        ",\"reuse_ns\":" + json_optional(a.reuse_ns) +
                        ",\"reclaimed_before_eligible\":" +
                        json_bool(a.reclaimed_before_eligible) +
                        ",\"cancelled_only\":" + json_bool(a.cancelled_only) +
                        ",\"cancelled_only_ns\":" + std::to_string(a.cancelled_only_ns) +
                        ",\"cancelled_only_byte_seconds\":" +
                        json_number(a.cancelled_only_byte_seconds) +
                        ",\"retained_at_end\":" + json_bool(a.retained_at_end) + "}";
               })
            << ",\"peak_cancelled_only_bytes\":" << report.peak_cancelled_only_bytes
            << ",\"cancelled_only_byte_seconds\":"
            << json_number(report.cancelled_only_byte_seconds) << ",\"incomplete\":"
            << json_list(report.incomplete, [](const std::string& s) { return json_string(s); })
            << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool json = false;
  std::optional<std::string> path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help") {
      help(argv[0]);
      return 0;
    }
    if (arg == "--version") {
      stateguard::cli::print_version("stateguard-timeline");
      return 0;
    }
    if (arg == "--json") {
      json = true;
    } else if ((arg == "-" || !arg.starts_with("-")) && !path) {
      path = std::string(arg);
    } else {
      usage(argv[0]);
      return 3;
    }
  }
  if (!path) {
    usage(argv[0]);
    return 3;
  }
  const auto contents = stateguard::cli::read_input(*path);
  if (!contents) {
    std::cerr << "stateguard-timeline: cannot open " << *path << '\n';
    return 3;
  }
  stateguard::Trace trace;
  try {
    trace = stateguard::parse_trace(*contents);
  } catch (const stateguard::TraceParseError& error) {
    std::cerr << "malformed_trace: " << error.what() << '\n';
    return 3;
  }
  const auto report = stateguard::build_timeline(trace);
  const int code = report.status == stateguard::TimelineStatus::Complete ? 0 : 2;
  if (json) {
    print_json(report);
    return code;
  }
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
  return code;
}
