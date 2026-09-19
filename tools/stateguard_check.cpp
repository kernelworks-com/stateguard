#include "stateguard/stateguard.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program << " TRACE\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    usage(argv[0]);
    return 3;
  }
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "stateguard-check: cannot open " << argv[1] << '\n';
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
  const auto report = stateguard::check(trace);
  std::cout << stateguard::to_string(report.status) << '\n';
  for (const auto& diagnostic : report.diagnostics) {
    std::cout << diagnostic.event_index << ' ' <<
        stateguard::to_string(diagnostic.category) << ": " <<
        diagnostic.message << '\n';
    for (const auto& witness : diagnostic.witness) {
      std::cout << "  " << witness << '\n';
    }
  }
  switch (report.status) {
    case stateguard::CheckStatus::Clean: return 0;
    case stateguard::CheckStatus::Violation: return 1;
    case stateguard::CheckStatus::Inconclusive: return 2;
  }
  return 3;
}

