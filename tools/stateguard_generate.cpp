#include "stateguard/stateguard.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void usage(const char* program) {
  std::cerr << "usage: " << program
            << " [--seed N] [--steps N] [--allocations N] [--prefixes N]\n"
               "       [--requests N] [--fault NAME] [--output FILE]\n"
               "       "
            << program << " --list-faults\n";
}

std::optional<std::uint64_t> parse_number(std::string_view text) {
  std::uint64_t value = 0;
  const auto* last = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), last, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != last) return std::nullopt;
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  stateguard::GeneratorConfig config;
  std::optional<std::string> output;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--list-faults") {
      for (const auto fault : stateguard::fault_kinds()) {
        std::cout << stateguard::to_string(fault) << '\n';
      }
      return 0;
    }
    if (i + 1 >= argc) {
      usage(argv[0]);
      return 3;
    }
    const std::string_view value = argv[++i];
    if (flag == "--fault") {
      const auto fault = stateguard::parse_fault(value);
      if (!fault) {
        std::cerr << "stateguard-generate: unknown fault '" << value
                  << "' (see --list-faults)\n";
        return 3;
      }
      config.fault = *fault;
      continue;
    }
    if (flag == "--output") {
      output = std::string(value);
      continue;
    }
    const auto number = parse_number(value);
    if (!number) {
      std::cerr << "stateguard-generate: " << flag << " needs a non-negative integer\n";
      return 3;
    }
    if (flag == "--seed") config.seed = *number;
    else if (flag == "--steps") config.steps = static_cast<std::size_t>(*number);
    else if (flag == "--allocations") config.allocations = static_cast<std::size_t>(*number);
    else if (flag == "--prefixes") config.prefixes = static_cast<std::size_t>(*number);
    else if (flag == "--requests") config.requests = static_cast<std::size_t>(*number);
    else {
      usage(argv[0]);
      return 3;
    }
  }

  stateguard::GeneratedTrace generated;
  std::string text;
  try {
    generated = stateguard::generate_trace(config);
    text = stateguard::serialize_trace(generated.trace);
  } catch (const std::invalid_argument& error) {
    std::cerr << "stateguard-generate: " << error.what() << '\n';
    return 3;
  }

  if (output) {
    std::ofstream file(*output, std::ios::binary);
    if (!(file << text) || !file.flush()) {
      std::cerr << "stateguard-generate: cannot write " << *output << '\n';
      return 3;
    }
  } else {
    std::cout << text;
  }

  std::cerr << "seed=" << config.seed << " fault=" << stateguard::to_string(config.fault)
            << " events=" << generated.trace.events.size()
            << " expected=" << stateguard::to_string(generated.expected_status);
  if (generated.expected_category) {
    std::cerr << '/' << stateguard::to_string(*generated.expected_category);
  }
  if (generated.fault_event) std::cerr << " fault_event=" << *generated.fault_event;
  std::cerr << '\n';
  return 0;
}
