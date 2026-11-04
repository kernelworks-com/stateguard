#include "stateguard/stateguard.hpp"

#include "cli.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " [--category NAME] [--max-checks N] [--output FILE] TRACE\n";
}

void help(const char* program) {
  std::cout << "usage: " << program
            << " [--category NAME] [--max-checks N] [--output FILE] TRACE\n"
               "\n"
               "Shrink a failing .sgtrace to a smaller trace with the same first\n"
               "finding. TRACE may be - for standard input.\n"
               "\n"
               "options:\n"
               "  --category NAME  reduce toward this diagnostic category\n"
               "  --max-checks N   stop after N checker runs\n"
               "  --output FILE    write the reduced trace to FILE, not stdout\n"
               "  --help           print this help and exit\n"
               "  --version        print the version and exit\n"
               "\n"
               "exit codes:\n"
               "  0  reduced\n"
               "  1  no reducible finding in the trace\n"
               "  3  usage error, unreadable or malformed trace, or write error\n";
}

}  // namespace

int main(int argc, char** argv) {
  stateguard::ReductionOptions options;
  std::optional<std::string> output;
  std::optional<std::string> path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--help") {
      help(argv[0]);
      return 0;
    }
    if (flag == "--version") {
      stateguard::cli::print_version("stateguard-reduce");
      return 0;
    }
    if (flag.starts_with("--")) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 3;
      }
      const std::string_view value = argv[++i];
      if (flag == "--category") {
        options.category = stateguard::parse_category(value);
        if (!options.category) {
          std::cerr << "stateguard-reduce: unknown category '" << value << "'\n";
          return 3;
        }
      } else if (flag == "--max-checks") {
        std::uint64_t checks = 0;
        const auto* last = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), last, checks);
        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != last) {
          std::cerr << "stateguard-reduce: --max-checks needs a non-negative integer\n";
          return 3;
        }
        options.max_checks = static_cast<std::size_t>(checks);
      } else if (flag == "--output") {
        output = std::string(value);
      } else {
        usage(argv[0]);
        return 3;
      }
    } else if (!path) {
      path = std::string(flag);
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
    std::cerr << "stateguard-reduce: cannot open " << *path << '\n';
    return 3;
  }
  stateguard::Trace trace;
  try {
    trace = stateguard::parse_trace(*contents);
  } catch (const stateguard::TraceParseError& error) {
    std::cerr << "malformed_trace: " << error.what() << '\n';
    return 3;
  }

  stateguard::ReductionResult result;
  try {
    result = stateguard::reduce_trace(trace, options);
  } catch (const stateguard::ReductionError& error) {
    std::cerr << "stateguard-reduce: " << error.what() << '\n';
    return 1;
  }

  const auto text = stateguard::serialize_trace(result.trace);
  if (output) {
    std::ofstream file(*output, std::ios::binary);
    if (!(file << text) || !file.flush()) {
      std::cerr << "stateguard-reduce: cannot write " << *output << '\n';
      return 3;
    }
  } else {
    std::cout << text;
  }

  std::cerr << "target: " << stateguard::to_string(result.status) << ' '
            << stateguard::to_string(result.target.category)
            << (result.target.confirmed ? "" : " [unconfirmed]") << " at event "
            << result.target.event_index << ": " << result.target.message << '\n';
  for (const auto& witness : result.target.witness) std::cerr << "  " << witness << '\n';
  std::cerr << "reduced " << result.original_events << " -> " << result.trace.events.size()
            << " events in " << result.checks << " checks"
            << (result.one_minimal ? " (1-minimal)" : " (check budget reached)") << '\n';
  std::cerr << "kept original events:";
  for (const auto index : result.kept_events) std::cerr << ' ' << index;
  std::cerr << '\n';
  return 0;
}
