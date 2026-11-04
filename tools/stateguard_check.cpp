#include "stateguard/stateguard.hpp"

#include "cli.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program << " [--json | --quiet] TRACE\n";
}

void help(const char* program) {
  std::cout << "usage: " << program << " [--json | --quiet] TRACE\n"
               "\n"
               "Check a recorded .sgtrace for lifetime errors. TRACE may be - for\n"
               "standard input.\n"
               "\n"
               "options:\n"
               "  --json     print one JSON object instead of text\n"
               "  --quiet    print the status line only\n"
               "  --help     print this help and exit\n"
               "  --version  print the version and exit\n"
               "\n"
               "exit codes:\n"
               "  0  clean\n"
               "  1  violation\n"
               "  2  inconclusive\n"
               "  3  usage error, unreadable or malformed trace\n";
}

int exit_code(stateguard::CheckStatus status) {
  switch (status) {
    case stateguard::CheckStatus::Clean: return 0;
    case stateguard::CheckStatus::Violation: return 1;
    case stateguard::CheckStatus::Inconclusive: return 2;
  }
  return 3;
}

void print_json(const stateguard::CheckReport& report) {
  using stateguard::cli::json_string;
  std::cout << "{\"status\":" << json_string(stateguard::to_string(report.status))
            << ",\"exit_code\":" << exit_code(report.status)
            << ",\"capture_complete\":" << stateguard::cli::json_bool(report.capture_complete)
            << ",\"diagnostics\":[";
  for (std::size_t i = 0; i < report.diagnostics.size(); ++i) {
    const auto& diagnostic = report.diagnostics[i];
    std::cout << (i ? "," : "") << "{\"index\":" << diagnostic.event_index
              << ",\"category\":" << json_string(stateguard::to_string(diagnostic.category))
              << ",\"confirmed\":" << stateguard::cli::json_bool(diagnostic.confirmed)
              << ",\"message\":" << json_string(diagnostic.message) << ",\"witness\":[";
    for (std::size_t w = 0; w < diagnostic.witness.size(); ++w) {
      std::cout << (w ? "," : "") << json_string(diagnostic.witness[w]);
    }
    std::cout << "]}";
  }
  std::cout << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool json = false;
  bool quiet = false;
  std::optional<std::string> path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help") {
      help(argv[0]);
      return 0;
    }
    if (arg == "--version") {
      stateguard::cli::print_version("stateguard-check");
      return 0;
    }
    if (arg == "--json") {
      json = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if ((arg == "-" || !arg.starts_with("-")) && !path) {
      path = std::string(arg);
    } else {
      usage(argv[0]);
      return 3;
    }
  }
  if (!path || (json && quiet)) {
    usage(argv[0]);
    return 3;
  }
  const auto contents = stateguard::cli::read_input(*path);
  if (!contents) {
    std::cerr << "stateguard-check: cannot open " << *path << '\n';
    return 3;
  }
  stateguard::Trace trace;
  try {
    trace = stateguard::parse_trace(*contents);
  } catch (const stateguard::TraceParseError& error) {
    std::cerr << "malformed_trace: " << error.what() << '\n';
    return 3;
  }
  const auto report = stateguard::check(trace);
  if (json) {
    print_json(report);
    return exit_code(report.status);
  }
  std::cout << stateguard::to_string(report.status) << '\n';
  if (quiet) return exit_code(report.status);
  for (const auto& diagnostic : report.diagnostics) {
    std::cout << diagnostic.event_index << ' ' <<
        stateguard::to_string(diagnostic.category) <<
        (diagnostic.confirmed ? "" : " [unconfirmed]") << ": " <<
        diagnostic.message << '\n';
    for (const auto& witness : diagnostic.witness) {
      std::cout << "  " << witness << '\n';
    }
  }
  return exit_code(report.status);
}
