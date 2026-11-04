// Runs every completion order of one async-copy harness configuration, or one
// chosen schedule, checks each captured trace, and prints the counts.
//
// usage: stateguard-async-copy-explore --config NAME [--json]
//            [--choices LIST] [--trace FILE] [--failure-trace FILE]
//        stateguard-async-copy-explore --list
//
// A schedule passes when its trace checks clean, the checked request received
// intact bytes, and the harness's own ground truth agrees: no unsafe reclaim,
// no misattributed or lost notification, and every allocation reclaimed by the
// end. Exit codes: 0 when every schedule passes, 1 when any fails, 2 for a
// usage or harness error.

#include "../tools/cli.hpp"
#include "async_copy.hpp"
#include "stateguard/stateguard.hpp"

#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace stateguard;
using namespace stateguard::harness;

struct Configuration {
  const char* name;
  const char* label;
  ScenarioOptions options;
};

std::vector<Configuration> configurations() {
  ScenarioOptions shared;
  ScenarioOptions immediate;
  immediate.host.reclaim = ReclaimPolicy::Immediate;
  ScenarioOptions redelivery;
  redelivery.engine.duplicate_notifications = true;
  ScenarioOptions counted = redelivery;
  counted.host.idempotent_notifications = false;
  ScenarioOptions failed_retry;
  failed_retry.engine.failing_operations = {"load-1"};
  ScenarioOptions failed_publish = failed_retry;
  failed_publish.host.publish_failed_loads = true;
  ScenarioOptions reconnect;
  reconnect.kind = ScenarioKind::Reconnect;
  ScenarioOptions reconnect_tag_only = reconnect;
  reconnect_tag_only.host.attribution = Attribution::TagOnly;
  ScenarioOptions replay = reconnect;
  replay.engine.redeliver_on_reconnect = true;
  ScenarioOptions replay_tag_only = replay;
  replay_tag_only.host.attribution = Attribution::TagOnly;
  ScenarioOptions timeout;
  timeout.kind = ScenarioKind::TimeoutRetry;
  ScenarioOptions timeout_reclaim = timeout;
  timeout_reclaim.host.timeout = TimeoutPolicy::Reclaim;
  ScenarioOptions timeout_latest = timeout;
  timeout_latest.host.attribution = Attribution::LatestAttempt;
  return {
      {"shared-prefix", "shared prefix, deferred reclamation", shared},
      {"shared-prefix-immediate", "shared prefix, induced immediate reclamation", immediate},
      {"redelivery", "shared prefix, duplicated notifications, idempotent host", redelivery},
      {"redelivery-counted", "shared prefix, duplicated notifications, induced counting host",
       counted},
      {"failed-retry", "failed load retried before publication", failed_retry},
      {"failed-publish", "induced publication of a failed load", failed_publish},
      {"reconnect", "reconnect, route-checked host", reconnect},
      {"reconnect-tag-only", "reconnect, induced tag-only attribution", reconnect_tag_only},
      {"reconnect-replay", "reconnect with replayed notifications, route-checked host", replay},
      {"reconnect-replay-tag-only",
       "reconnect with replayed notifications, induced tag-only attribution", replay_tag_only},
      {"timeout-wait", "timeout and retry, host waits for the late completion", timeout},
      {"timeout-reclaim", "timeout and retry, induced reclaim on timeout", timeout_reclaim},
      {"timeout-latest-attempt",
       "timeout and retry, induced crediting of the newest attempt", timeout_latest},
  };
}

// Allocation generations that were allocated but never reclaimed.
std::size_t unreclaimed(const Trace& trace) {
  std::set<AllocationRef> live;
  for (const auto& event : trace.events) {
    if (const auto* a = std::get_if<AllocateEvent>(&event)) {
      live.insert({a->allocation, a->generation});
    } else if (const auto* r = std::get_if<ReclaimEvent>(&event)) {
      live.erase(r->allocation);
    }
  }
  return live.size();
}

struct Tally {
  std::size_t schedules = 0;
  std::size_t passed = 0;
  std::size_t clean = 0;
  std::size_t violation = 0;
  std::size_t inconclusive = 0;
  std::size_t corrupted = 0;
  std::size_t unsafe_reclaim = 0;
  std::size_t misattributed = 0;
  std::size_t lost_completion = 0;
  std::size_t failed_publication = 0;
  std::size_t leaked = 0;
  std::optional<std::size_t> first_failure;
  std::optional<ScenarioRun> first_failure_run;
  std::string first_failure_status;
  std::string first_failure_category;
};

void add(Tally& tally, ScenarioRun run) {
  const auto report = check(run.trace);
  const auto index = tally.schedules++;
  switch (report.status) {
    case CheckStatus::Clean: ++tally.clean; break;
    case CheckStatus::Violation: ++tally.violation; break;
    case CheckStatus::Inconclusive: ++tally.inconclusive; break;
  }
  if (!run.delivered_intact) ++tally.corrupted;
  if (!run.hazards.empty()) ++tally.unsafe_reclaim;
  if (run.misattributions > 0) ++tally.misattributed;
  if (run.unacknowledged > 0) ++tally.lost_completion;
  if (run.failed_publications > 0) ++tally.failed_publication;
  const bool leaked = unreclaimed(run.trace) > 0;
  if (leaked) ++tally.leaked;
  const bool passed = report.clean() && run.delivered_intact && run.hazards.empty() &&
                      run.misattributions == 0 && run.unacknowledged == 0 &&
                      run.failed_publications == 0 && !leaked;
  if (passed) {
    ++tally.passed;
    return;
  }
  if (tally.first_failure) return;
  tally.first_failure = index;
  tally.first_failure_status = to_string(report.status);
  for (const auto& d : report.diagnostics) {
    if (d.category != DiagnosticCategory::IncompleteEvidence && d.confirmed) {
      tally.first_failure_category = to_string(d.category);
      break;
    }
  }
  if (tally.first_failure_category.empty() && !report.diagnostics.empty()) {
    tally.first_failure_category = to_string(report.diagnostics.front().category);
  }
  tally.first_failure_run = std::move(run);
}

std::string json_list(const std::vector<std::size_t>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    out += std::to_string(values[i]);
  }
  return out + "]";
}

std::string json_list(const std::vector<std::string>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    out += cli::json_string(values[i]);
  }
  return out + "]";
}

void print_json(const Configuration& configuration, const Tally& t, bool single) {
  std::cout << "{\"config\":" << cli::json_string(configuration.name)
            << ",\"mode\":" << cli::json_string(single ? "schedule" : "exhaustive")
            << ",\"schedules\":" << t.schedules << ",\"passed\":" << t.passed
            << ",\"clean\":" << t.clean << ",\"violation\":" << t.violation
            << ",\"inconclusive\":" << t.inconclusive << ",\"corrupted_bytes\":" << t.corrupted
            << ",\"unsafe_reclaim\":" << t.unsafe_reclaim
            << ",\"misattributed\":" << t.misattributed
            << ",\"lost_completion\":" << t.lost_completion
            << ",\"failed_publication\":" << t.failed_publication
            << ",\"unreclaimed\":" << t.leaked << ",\"first_failure\":";
  if (!t.first_failure) {
    std::cout << "null";
  } else {
    const auto& run = *t.first_failure_run;
    std::cout << "{\"index\":" << *t.first_failure
              << ",\"status\":" << cli::json_string(t.first_failure_status)
              << ",\"category\":"
              << (t.first_failure_category.empty() ? "null"
                                                   : cli::json_string(t.first_failure_category))
              << ",\"delivered_intact\":" << cli::json_bool(run.delivered_intact)
              << ",\"unsafe_reclaims\":" << run.hazards.size()
              << ",\"misattributions\":" << run.misattributions
              << ",\"unacknowledged\":" << run.unacknowledged
              << ",\"failed_publications\":" << run.failed_publications
              << ",\"unreclaimed\":" << unreclaimed(run.trace)
              << ",\"choices\":" << json_list(run.choices)
              << ",\"schedule\":" << json_list(run.schedule) << '}';
  }
  std::cout << "}\n";
}

void print_text(const Configuration& configuration, const Tally& t) {
  std::cout << configuration.label << ": " << t.schedules << " schedule"
            << (t.schedules == 1 ? "" : "s") << ", " << t.passed << " passed; " << t.clean
            << " clean, " << t.violation << " violation, " << t.inconclusive
            << " inconclusive; " << t.corrupted << " corrupted bytes, " << t.unsafe_reclaim
            << " unsafe reclaim, " << t.misattributed << " misattributed, "
            << t.lost_completion << " lost completion, " << t.failed_publication
            << " failed publication, " << t.leaked << " unreclaimed\n";
  if (t.first_failure) {
    std::cout << "first failing schedule (" << *t.first_failure << ", "
              << t.first_failure_status << "):";
    for (const auto& action : t.first_failure_run->schedule) std::cout << "\n  " << action;
    std::cout << "\n";
  }
}

bool write_trace(const std::string& path, const Trace& trace) {
  std::ofstream output(path);
  output << serialize_trace(trace);
  if (!output) {
    std::cerr << "cannot write " << path << '\n';
    return false;
  }
  return true;
}

std::optional<std::vector<std::size_t>> parse_choices(const std::string& text) {
  std::vector<std::size_t> choices;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (item.empty()) continue;
    if (item.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    choices.push_back(static_cast<std::size_t>(std::stoull(item)));
  }
  return choices;
}

int usage(const char* program) {
  std::cerr << "usage: " << program
            << " --config NAME [--json] [--choices LIST] [--trace FILE] [--failure-trace FILE]\n"
            << "       " << program << " --list\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::string> name;
  std::optional<std::string> choices_text;
  std::optional<std::string> trace_path;
  std::optional<std::string> failure_path;
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--list") {
      for (const auto& c : configurations()) std::cout << c.name << "  " << c.label << '\n';
      return 0;
    } else if (arg == "--json") {
      json = true;
    } else if (arg == "--config" && has_value) {
      name = argv[++i];
    } else if (arg == "--choices" && has_value) {
      choices_text = argv[++i];
    } else if (arg == "--trace" && has_value) {
      trace_path = argv[++i];
    } else if (arg == "--failure-trace" && has_value) {
      failure_path = argv[++i];
    } else if (arg == "--help") {
      std::cout << "usage: " << argv[0]
                << " --config NAME [--json] [--choices LIST] [--trace FILE]"
                   " [--failure-trace FILE]\n"
                   "  --config NAME         configuration to run (--list shows them)\n"
                   "  --json                print one JSON object\n"
                   "  --choices LIST        run one schedule, e.g. 0,1,0 (empty: the first)\n"
                   "  --trace FILE          with --choices, write that schedule's trace\n"
                   "  --failure-trace FILE  write the first failing schedule's trace\n"
                   "exit codes: 0 every schedule passed, 1 a schedule failed, 2 error\n";
      return 0;
    } else {
      return usage(argv[0]);
    }
  }
  if (!name) return usage(argv[0]);
  std::optional<Configuration> configuration;
  for (const auto& c : configurations()) {
    if (*name == c.name) configuration = c;
  }
  if (!configuration) {
    std::cerr << "unknown configuration " << *name << " (--list shows them)\n";
    return 2;
  }

  Tally tally;
  try {
    if (choices_text) {
      const auto choices = parse_choices(*choices_text);
      if (!choices) return usage(argv[0]);
      auto run = run_scenario(configuration->options, *choices);
      if (trace_path && !write_trace(*trace_path, run.trace)) return 2;
      add(tally, std::move(run));
    } else {
      explore_scenario(configuration->options,
                       [&](const ScenarioRun& run) { add(tally, run); });
    }
  } catch (const std::exception& error) {
    std::cerr << "harness error: " << error.what() << '\n';
    return 2;
  }
  if (failure_path && tally.first_failure_run &&
      !write_trace(*failure_path, tally.first_failure_run->trace)) {
    return 2;
  }
  if (json) {
    print_json(*configuration, tally, choices_text.has_value());
  } else {
    print_text(*configuration, tally);
  }
  return tally.passed == tally.schedules ? 0 : 1;
}
