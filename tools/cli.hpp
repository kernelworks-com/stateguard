#pragma once

// Helpers shared by the command-line tools. Not part of the library.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#ifndef STATEGUARD_VERSION
#error "STATEGUARD_VERSION must be defined by the build"
#endif

namespace stateguard::cli {

inline void print_version(const char* tool) {
  std::cout << tool << ' ' << STATEGUARD_VERSION << '\n';
}

// Reads a whole trace from a file, or from standard input when path is "-".
inline std::optional<std::string> read_input(const std::string& path) {
  if (path == "-") {
    std::string contents((std::istreambuf_iterator<char>(std::cin)),
                         std::istreambuf_iterator<char>());
    if (std::cin.bad()) return std::nullopt;
    return contents;
  }
  std::ifstream input(path);
  if (!input) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

// A JSON string literal, with quotes and every control character escaped.
inline std::string json_string(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
          out += escaped;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

inline std::string json_optional(const std::optional<std::uint64_t>& value) {
  return value ? std::to_string(*value) : "null";
}

// Round-trippable; the values here are always finite.
inline std::string json_number(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.17g", value);
  return text;
}

inline const char* json_bool(bool value) { return value ? "true" : "false"; }

}  // namespace stateguard::cli
