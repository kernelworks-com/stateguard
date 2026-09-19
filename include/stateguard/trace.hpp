#pragma once

#include "stateguard/types.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace stateguard {

class TraceParseError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The format is intentionally dependency-free and line-oriented.  Fields are
// separated by '|'; identifiers and descriptor text must not contain '|', '\n',
// or '\r'.  It is a fixture/capture interchange format, not an ABI promise.
std::string serialize_trace(const Trace& trace);
Trace parse_trace(std::string_view text);

}  // namespace stateguard

