#include "stateguard/trace.hpp"

#include <charconv>
#include <cstdint>
#include <sstream>
#include <type_traits>
#include <utility>

namespace stateguard {
namespace {

std::vector<std::string> split(std::string_view value, char separator) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find(separator, begin);
    fields.emplace_back(value.substr(begin, end == std::string_view::npos
                                               ? value.size() - begin
                                               : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return fields;
}

void require(bool condition, std::size_t line, std::string_view message) {
  if (!condition) {
    throw TraceParseError("line " + std::to_string(line) + ": " +
                          std::string(message));
  }
}

std::uint64_t parse_uint(std::string_view value, std::size_t line,
                         std::string_view field) {
  require(!value.empty(), line, std::string(field) + " is empty");
  std::uint64_t result = 0;
  const auto* first = value.data();
  const auto* last = first + value.size();
  const auto parsed = std::from_chars(first, last, result);
  require(parsed.ec == std::errc{} && parsed.ptr == last, line,
          std::string("invalid ") + std::string(field));
  return result;
}

bool parse_bool(std::string_view value, std::size_t line) {
  if (value == "complete" || value == "1" || value == "true") return true;
  if (value == "incomplete" || value == "0" || value == "false") return false;
  throw TraceParseError("line " + std::to_string(line) +
                        ": invalid completion marker");
}

void require_token(std::string_view value, std::size_t line,
                   std::string_view field, bool allow_empty = false) {
  require(allow_empty || !value.empty(), line,
          std::string(field) + " is empty");
  require(value.find('|') == std::string_view::npos &&
              value.find('\n') == std::string_view::npos &&
              value.find('\r') == std::string_view::npos,
          line, std::string(field) + " contains a reserved character");
}

SemanticDescriptor parse_descriptor(const std::vector<std::string>& f,
                                    std::size_t offset, std::size_t line) {
  require(f.size() == offset + 10, line, "descriptor has the wrong field count");
  SemanticDescriptor d;
  d.name_space = NamespaceId(f[offset]);
  d.model_revision = f[offset + 1];
  d.adapter = f[offset + 2];
  d.component = f[offset + 3];
  d.prefix = f[offset + 4];
  d.token_begin = parse_uint(f[offset + 5], line, "token_begin");
  d.token_end = parse_uint(f[offset + 6], line, "token_end");
  d.representation = f[offset + 7];
  d.layout = f[offset + 8];
  d.position_metadata = f[offset + 9];
  for (std::size_t i = offset; i < f.size(); ++i) {
    require_token(f[i], line, "descriptor field");
  }
  require(!d.name_space.empty(), line, "namespace is empty");
  require(d.token_begin <= d.token_end, line, "token interval is inverted");
  return d;
}

void validate_identifier(std::string_view value, std::string_view field);

void validate_pipe_field(std::string_view value, std::string_view field,
                        bool allow_empty = true) {
  if (!allow_empty && value.empty()) {
    throw std::invalid_argument(std::string(field) + " is empty");
  }
  if (value.find_first_of("|\n\r") != std::string_view::npos) {
    throw std::invalid_argument(std::string(field) + " contains a reserved character");
  }
}

std::string descriptor_fields(const SemanticDescriptor& d) {
  validate_pipe_field(d.name_space.value, "namespace", false);
  validate_pipe_field(d.model_revision, "model revision", false);
  validate_pipe_field(d.adapter, "adapter", false);
  validate_pipe_field(d.component, "component", false);
  validate_pipe_field(d.prefix, "prefix", false);
  validate_pipe_field(d.representation, "representation", false);
  validate_pipe_field(d.layout, "layout", false);
  validate_pipe_field(d.position_metadata, "position metadata", false);
  if (d.token_begin > d.token_end) {
    throw std::invalid_argument("token interval is inverted");
  }
  return d.name_space.value + "|" + d.model_revision + "|" + d.adapter +
         "|" + d.component + "|" + d.prefix + "|" +
         std::to_string(d.token_begin) + "|" + std::to_string(d.token_end) +
         "|" + d.representation + "|" + d.layout + "|" +
         d.position_metadata;
}

std::string ref_fields(const AllocationRef& ref) {
  validate_identifier(ref.allocation.value, "allocation");
  if (ref.generation == 0) throw std::invalid_argument("generation must be nonzero");
  return ref.allocation.value + "|" + std::to_string(ref.generation);
}

void validate_identifier(std::string_view value, std::string_view field) {
  if (value.empty() || value.find_first_of("|;:,\n\r") != std::string_view::npos) {
    throw std::invalid_argument(std::string(field) + " contains a reserved character");
  }
}

}  // namespace

std::string serialize_trace(const Trace& trace) {
  if (trace.schema != Trace::schema_major) {
    throw std::invalid_argument("unsupported trace schema major version");
  }
  std::ostringstream out;
  out << "STATEGUARD_TRACE|" << trace.schema << '\n';
  for (const auto& event : trace.events) {
    std::visit(
        [&out](const auto& e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, AllocateEvent>) {
            validate_identifier(e.allocation.value, "allocation");
            validate_identifier(e.domain, "domain");
            if (e.generation == 0) throw std::invalid_argument("generation must be nonzero");
            out << "allocate|" << e.allocation.value << '|' << e.generation << '|'
                << e.bytes << '|' << e.domain << '\n';
          } else if constexpr (std::is_same_v<T, BindStateEvent>) {
            validate_identifier(e.allocation.allocation.value, "allocation");
            validate_identifier(e.state.value, "state");
            out << "bind_state|" << ref_fields(e.allocation) << '|' << e.state.value
                << '|' << descriptor_fields(e.descriptor) << '\n';
          } else if constexpr (std::is_same_v<T, SubmitEvent>) {
            validate_identifier(e.operation.value, "operation");
            validate_identifier(e.request.value, "request");
            out << "submit|" << e.operation.value << '|' << e.request.value << '|';
            for (std::size_t i = 0; i < e.leases.size(); ++i) {
              if (i != 0) out << ';';
              char access_code = 0;
              if (e.leases[i].access == AccessKind::Read) access_code = 'r';
              else if (e.leases[i].access == AccessKind::Write) access_code = 'w';
              else throw std::invalid_argument("unknown access kind");
              validate_identifier(e.leases[i].allocation.allocation.value,
                                  "lease allocation");
              if (e.leases[i].allocation.generation == 0) {
                throw std::invalid_argument("lease generation must be nonzero");
              }
              out << access_code << ':'
                  << e.leases[i].allocation.allocation.value << ':'
                  << e.leases[i].allocation.generation;
            }
            out << '|';
            for (std::size_t i = 0; i < e.dependencies.size(); ++i) {
              if (i != 0) out << ',';
              validate_identifier(e.dependencies[i].value, "dependency");
              out << e.dependencies[i].value;
            }
            out << '\n';
          } else if constexpr (std::is_same_v<T, PublishEvent>) {
            validate_identifier(e.state.value, "state");
            out << "publish|" << e.state.value << '|' << ref_fields(e.allocation)
                << '|' << (e.completed_operation ? e.completed_operation->value : "-")
                << '\n';
          } else if constexpr (std::is_same_v<T, ConsumeEvent>) {
            validate_identifier(e.request.value, "request");
            validate_identifier(e.state.value, "state");
            out << "consume|" << e.request.value << '|' << e.state.value << '|'
                << ref_fields(e.allocation) << '|' << descriptor_fields(e.expected)
                << '\n';
          } else if constexpr (std::is_same_v<T, CancelRequestEvent>) {
            validate_identifier(e.request.value, "request");
            out << "cancel|" << e.request.value << '\n';
          } else if constexpr (std::is_same_v<T, CompleteAccessEvent>) {
            validate_identifier(e.operation.value, "operation");
            out << "complete|" << e.operation.value << '\n';
          } else if constexpr (std::is_same_v<T, RetireEvent>) {
            out << "retire|" << ref_fields(e.allocation) << '\n';
          } else if constexpr (std::is_same_v<T, ReclaimEvent>) {
            out << "reclaim|" << ref_fields(e.allocation) << '\n';
          } else if constexpr (std::is_same_v<T, CoverageGapEvent>) {
            validate_identifier(e.reason, "coverage gap reason");
            out << "coverage_gap|" << e.reason << '\n';
          } else if constexpr (std::is_same_v<T, EndTraceEvent>) {
            out << "end|" << (e.capture_complete ? "complete" : "incomplete") << '\n';
          }
        },
        event);
  }
  return out.str();
}

Trace parse_trace(std::string_view text) {
  Trace trace;
  std::size_t line_number = 0;
  std::size_t begin = 0;
  bool header_seen = false;
  while (begin <= text.size()) {
    ++line_number;
    const auto end = text.find('\n', begin);
    auto line = text.substr(begin, end == std::string_view::npos
                                     ? text.size() - begin
                                     : end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) {
      const auto fields = split(line, '|');
      if (!header_seen) {
        require(fields.size() == 2 && fields[0] == "STATEGUARD_TRACE", line_number,
                "missing trace header");
        trace.schema = static_cast<std::uint32_t>(parse_uint(fields[1], line_number, "schema"));
        require(trace.schema == Trace::schema_major, line_number,
                "unsupported schema major version");
        header_seen = true;
      } else {
        require(!fields.empty(), line_number, "empty event");
        const auto& kind = fields[0];
        if (kind == "allocate") {
          require(fields.size() == 5, line_number, "allocate has the wrong field count");
          require_token(fields[1], line_number, "allocation");
          require_token(fields[4], line_number, "domain", true);
          trace.events.emplace_back(AllocateEvent{AllocationId(fields[1]),
                                                   parse_uint(fields[2], line_number, "generation"),
                                                   parse_uint(fields[3], line_number, "bytes"),
                                                   fields[4]});
        } else if (kind == "bind_state") {
          require(fields.size() == 14, line_number, "bind_state has the wrong field count");
          require_token(fields[1], line_number, "allocation");
          require_token(fields[3], line_number, "state");
          trace.events.emplace_back(BindStateEvent{
              AllocationRef{AllocationId(fields[1]), parse_uint(fields[2], line_number, "generation")},
              StateId(fields[3]), parse_descriptor(fields, 4, line_number)});
        } else if (kind == "submit") {
          require(fields.size() == 5, line_number, "submit has the wrong field count");
          require_token(fields[1], line_number, "operation");
          require_token(fields[2], line_number, "request");
          SubmitEvent event{OperationId(fields[1]), RequestId(fields[2]), {}, {}};
          if (!fields[3].empty()) {
            for (const auto& lease_text : split(fields[3], ';')) {
              const auto lease = split(lease_text, ':');
              require(lease.size() == 3 && (lease[0] == "r" || lease[0] == "w"),
                      line_number, "invalid lease");
              require_token(lease[1], line_number, "lease allocation");
              event.leases.push_back(Lease{AllocationRef{
                                                AllocationId(lease[1]),
                                                parse_uint(lease[2], line_number, "lease generation")},
                                            lease[0] == "r" ? AccessKind::Read : AccessKind::Write});
            }
          }
          if (!fields[4].empty()) {
            for (const auto& dependency : split(fields[4], ',')) {
              require_token(dependency, line_number, "dependency");
              event.dependencies.emplace_back(dependency);
            }
          }
          trace.events.emplace_back(std::move(event));
        } else if (kind == "publish") {
          require(fields.size() == 5, line_number, "publish has the wrong field count");
          require_token(fields[1], line_number, "state");
          require_token(fields[2], line_number, "allocation");
          std::optional<OperationId> op;
          if (fields[4] != "-") {
            require_token(fields[4], line_number, "completed operation");
            op.emplace(fields[4]);
          }
          trace.events.emplace_back(PublishEvent{
              StateId(fields[1]),
              AllocationRef{AllocationId(fields[2]), parse_uint(fields[3], line_number, "generation")},
              op});
        } else if (kind == "consume") {
          require(fields.size() == 15, line_number, "consume has the wrong field count");
          require_token(fields[1], line_number, "request");
          require_token(fields[2], line_number, "state");
          require_token(fields[3], line_number, "allocation");
          trace.events.emplace_back(ConsumeEvent{
              RequestId(fields[1]), StateId(fields[2]),
              AllocationRef{AllocationId(fields[3]), parse_uint(fields[4], line_number, "generation")},
              parse_descriptor(fields, 5, line_number)});
        } else if (kind == "cancel") {
          require(fields.size() == 2, line_number, "cancel has the wrong field count");
          require_token(fields[1], line_number, "request");
          trace.events.emplace_back(CancelRequestEvent{RequestId(fields[1])});
        } else if (kind == "complete") {
          require(fields.size() == 2, line_number, "complete has the wrong field count");
          require_token(fields[1], line_number, "operation");
          trace.events.emplace_back(CompleteAccessEvent{OperationId(fields[1])});
        } else if (kind == "retire" || kind == "reclaim") {
          require(fields.size() == 3, line_number, "allocation lifecycle event has the wrong field count");
          require_token(fields[1], line_number, "allocation");
          const AllocationRef ref{AllocationId(fields[1]), parse_uint(fields[2], line_number, "generation")};
          if (kind == "retire") trace.events.emplace_back(RetireEvent{ref});
          else trace.events.emplace_back(ReclaimEvent{ref});
        } else if (kind == "coverage_gap") {
          require(fields.size() == 2, line_number, "coverage_gap has the wrong field count");
          require_token(fields[1], line_number, "coverage gap reason");
          trace.events.emplace_back(CoverageGapEvent{fields[1]});
        } else if (kind == "end") {
          require(fields.size() == 2, line_number, "end has the wrong field count");
          trace.events.emplace_back(EndTraceEvent{parse_bool(fields[1], line_number)});
        } else {
          throw TraceParseError("line " + std::to_string(line_number) +
                                ": unknown event kind '" + kind + "'");
        }
      }
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  require(header_seen, line_number == 0 ? 1 : line_number, "missing trace header");
  return trace;
}

}  // namespace stateguard
