#!/usr/bin/env python3
"""Agent fix loop: a model proposes a patch for a planted host bug; the harness decides.

Each planted bug is a small source change to a scratch copy of
harness/async_copy.cpp that makes the safe host policy behave like one of the
harness's induced bugs. Per attempt the model gets the planted source region,
the checker's JSON report and the reduced witness for the first failing
schedule, the event rules from docs/TRACE-FORMAT.md, and a strict output
contract (one unified diff, nothing else). The script applies the diff to the
scratch copy under BUILD_DIR/agent-fix/<bug>/, rebuilds the exploration tool
from it, runs every completion order of the bug's configuration, and replays
the failing schedule. The patch is accepted only when every schedule passes
(clean verdict, intact bytes, no unsafe reclaim, no misattributed or lost
completion, no failed publication, every allocation reclaimed) and the replayed
schedule passes too. The model's text is never a verdict.

Exit codes: 0 fix accepted, 1 attempts exhausted, 2 tooling error.
Python 3.9+, standard library only.
"""

import argparse
import difflib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from explain_trace import (DEFAULT_BASE_URL, DEFAULT_KEY_ENV, DEFAULT_MODEL,  # noqa: E402
                           ExplainError, chat_completion)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS_FILE = "harness/async_copy.cpp"
HEADER_FILE = "harness/async_copy.hpp"
EXPLORE_SOURCE = "examples/async_copy_explore.cpp"
TRACE_FORMAT_DOC = "docs/TRACE-FORMAT.md"

# muse-spark-1.3-contributor list prices, US dollars per million tokens.
PRICE_INPUT_PER_M = 0.10
PRICE_OUTPUT_PER_M = 0.20

EXIT_ACCEPTED = 0
EXIT_EXHAUSTED = 1
EXIT_TOOLING = 2

# The model may change only the CacheHost member functions: from the
# constructor up to the scenario code that drives the host.
REGION_START = "CacheHost::CacheHost(TraceRecorder& recorder"
REGION_END = "namespace {"

# Planted bugs. `plant` is a list of (pristine text, planted text) pairs
# applied to the scratch copy; `functions` are the definitions the model is
# shown (the first is where the bug is planted); `config` is the
# stateguard-async-copy-explore configuration that must pass afterwards. The
# configuration always uses the default (safe) HostOptions: the model has to
# repair the policy code, and no option switch makes the planted code safe.
BUGS = {
    "premature-reclaim": {
        "config": "shared-prefix",
        "induced": "ReclaimPolicy::Immediate (demo configuration 'immediate')",
        "functions": ["void CacheHost::release(", "void CacheHost::drop(",
                      "void CacheHost::reclaim("],
        "plant": [(
            "  record.released = true;\n"
            "  if (options_.reclaim == ReclaimPolicy::Immediate || record.holds == 0) "
            "reclaim(buffer);\n",
            "  record.released = true;\n"
            "  reclaim(buffer);\n")],
        "symptom": "release() reclaims a buffer at once, while a copy or a consuming "
                   "request still holds it: premature_reclaim in every shared-prefix "
                   "schedule, and request-2 receives overwritten bytes in some.",
    },
    "counted-redelivery": {
        "config": "redelivery",
        "induced": "HostOptions::idempotent_notifications = false",
        "functions": ["void CacheHost::poll(", "void CacheHost::drop("],
        "plant": [(
            "    if (record.notified && options_.idempotent_notifications) continue;\n",
            "")],
        "symptom": "with an engine that delivers every notification twice, poll() drops "
                   "an operation's holds once per delivery, so a buffer can be reclaimed "
                   "while another copy or a consumer still holds it.",
    },
    "failed-load-publish": {
        "config": "failed-retry",
        "induced": "HostOptions::publish_failed_loads = true (demo configuration "
                   "'failed-publish')",
        "functions": ["void CacheHost::poll(", "void CacheHost::retry("],
        "plant": [(
            "      const bool failed = notification->outcome == AccessOutcome::Failed;\n"
            "      if (failed && !options_.publish_failed_loads) {\n"
            "        retry(*operation, record);\n"
            "      } else {\n"
            "        if (failed) ++failed_publications_;\n"
            "        recorder_.record(PublishEvent{*record.publishes, "
            "to_ref(record.destination), *operation});\n"
            "        published_.insert(*record.publishes);\n"
            "      }\n",
            "      if (notification->outcome == AccessOutcome::Failed) ++failed_publications_;\n"
            "      recorder_.record(PublishEvent{*record.publishes, "
            "to_ref(record.destination), *operation});\n"
            "      published_.insert(*record.publishes);\n")],
        "symptom": "poll() publishes a loaded state even when its copy failed and wrote "
                   "only half the buffer: failed_write_publication in every schedule, "
                   "and the reader gets partial bytes.",
    },
    "tag-only-attribution": {
        "config": "reconnect-replay",
        "induced": "Attribution::TagOnly",
        "functions": ["std::optional<OperationId> CacheHost::attribute(",
                      "void CacheHost::track(", "void CacheHost::poll("],
        "plant": [(
            "  if (options_.attribution == Attribution::TagOnly) {\n"
            "    const auto found = tags_.find(route.tag);\n"
            "    if (found == tags_.end()) return std::nullopt;\n"
            "    return found->second;\n"
            "  }\n"
            "  const auto found = routes_.find(route);\n"
            "  if (found == routes_.end()) return std::nullopt;\n",
            "  const auto found = tags_.find(route.tag);\n"
            "  if (found == tags_.end()) return std::nullopt;\n")],
        "symptom": "attribute() matches a routed notification by its tag alone; tags "
                   "restart after a reconnect, so a late or replayed epoch-1 "
                   "notification acknowledges the epoch-2 load: misattributed_completion, "
                   "unsafe reclaims, and lost completions.",
    },
    "reclaim-on-timeout": {
        "config": "timeout-wait",
        "induced": "TimeoutPolicy::Reclaim",
        "functions": ["void CacheHost::expire(", "BufferHandle CacheHost::reload(",
                      "void CacheHost::drop("],
        "plant": [(
            "  if (options_.timeout == TimeoutPolicy::Reclaim && !record.notified && "
            "!record.abandoned) {\n",
            "  if (!record.notified && !record.abandoned) {\n")],
        "symptom": "expire() treats a timeout as the end of the operation and drops its "
                   "holds, so the timed-out load's buffers are reclaimed while the copy "
                   "can still run: premature_reclaim after an expired lease in every "
                   "schedule.",
    },
}

SYSTEM_PROMPT = """\
You repair lifetime bugs in a C++ prefix-cache host. The host loads staged \
bytes into cache buffers with an asynchronous copy engine, publishes states, \
lets requests consume and copy them out, and releases buffers. A test harness \
replays every legal completion order of a scenario, records a lifecycle \
trace, and a checker judges each trace. Your patch is accepted only when \
every schedule then checks clean, delivers intact bytes, and reclaims every \
buffer; nothing you say is taken as evidence.

Event rules (from docs/TRACE-FORMAT.md):
{rules}

Output contract:
- Reply with exactly one unified diff against harness/async_copy.cpp and \
nothing else: no explanation, no Markdown fences.
- Start with the lines `--- a/harness/async_copy.cpp` and \
`+++ b/harness/async_copy.cpp`, then one or more hunks headed \
`@@ -start,count +start,count @@`. Prefix every unchanged line with a space, \
removed lines with `-`, added lines with `+`, and copy at least two \
unchanged context lines around each change exactly as shown.
- Edit only CacheHost member function bodies (lines {first}-{last} of the \
file). The header, the scenario code, and the HostOptions defaults are fixed. \
The checked configuration uses the default HostOptions; keep the option \
switches working rather than deleting them.
- Keep every trace record (`recorder_.record`), pool call, and harness \
counter (`hazards_`, `misattributions_`, `failed_publications_`)."""


class ToolingError(Exception):
    pass


class PatchError(Exception):
    pass


# ---------------------------------------------------------------- utilities

def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def cost_usd(prompt_tokens, completion_tokens):
    return (prompt_tokens * PRICE_INPUT_PER_M + completion_tokens * PRICE_OUTPUT_PER_M) / 1e6


def event_rules():
    """The 'What each event means' list from the trace format document."""
    text = read_text(os.path.join(ROOT, TRACE_FORMAT_DOC))
    start = text.find("What each event means:")
    end = text.find("### `complete` forms")
    if start < 0 or end < 0:
        raise ToolingError(f"cannot find the event rules in {TRACE_FORMAT_DOC}")
    keep = ("allocate", "bind_state", "submit", "publish", "consume", "cancel", "complete",
            "connect", "expire", "retire", "reclaim")
    rules, current = [], None
    for line in text[start:end].splitlines()[1:]:
        if line.startswith("- "):
            current = line
            rules.append(current)
        elif line.startswith("  ") and current is not None:
            rules[-1] += " " + line.strip()
    picked = [r for r in rules if r.split("`")[1] in keep]
    return "\n".join(picked) + (
        "\nAn operation's leases are released only by its `complete`. Retire then "
        "reclaim is legal only when no lease and no consumer remains on that "
        "generation. A notification may be delivered more than once; a timeout or "
        "a new connection epoch is not proof that a copy stopped.")


def region_bounds(lines):
    start = next((i for i, l in enumerate(lines) if l.startswith(REGION_START)), None)
    if start is None:
        raise ToolingError(f"cannot find '{REGION_START}' in {HARNESS_FILE}")
    end = next((i for i in range(start, len(lines)) if lines[i].rstrip() == REGION_END), None)
    if end is None:
        raise ToolingError(f"cannot find the scenario section in {HARNESS_FILE}")
    return start, end  # 0-based, end exclusive


def function_text(lines, signature):
    start = next((i for i, l in enumerate(lines) if l.startswith(signature)), None)
    if start is None:
        raise ToolingError(f"cannot find '{signature}' in the planted source")
    end = next(i for i in range(start, len(lines)) if lines[i].rstrip() == "}")
    body = "".join(lines[start:end + 1])
    return start + 1, body


def header_excerpt(header):
    """HostOptions and the CacheHost declaration: what the model may rely on."""
    parts = []
    for begin, finish in (("enum class ReclaimPolicy", "struct Hazard"),
                          ("class CacheHost {", "enum class ScenarioKind")):
        a, b = header.find(begin), header.find(finish)
        if a < 0 or b < 0:
            raise ToolingError(f"cannot find '{begin}' in {HEADER_FILE}")
        parts.append(header[a:b].rstrip())
    return "\n\n".join(parts)


def plant(source, bug):
    for old, new in bug["plant"]:
        if source.count(old) != 1:
            raise ToolingError("the planted change no longer matches harness/async_copy.cpp; "
                               "update the BUGS table")
        source = source.replace(old, new)
    return source


# ---------------------------------------------------------------- diff handling

def extract_diff(completion):
    """The diff in a completion. Tolerates Markdown fences; nothing else."""
    text = completion.strip()
    fenced = re.findall(r"```[a-zA-Z]*\n(.*?)```", text, re.S)
    if fenced:
        text = max(fenced, key=len)
    lines = text.splitlines()
    first = next((i for i, l in enumerate(lines) if l.startswith("--- ") or l.startswith("@@")),
                 None)
    if first is None:
        raise PatchError("the reply contains no unified diff")
    return "\n".join(lines[first:]) + "\n"


HUNK = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")


def parse_hunks(diff):
    hunks, current = [], None
    for raw in diff.splitlines():
        if raw.startswith("--- ") and current is None:
            continue
        if raw.startswith("+++ "):
            target = raw[4:].strip().split("\t")[0]
            if not target.endswith("async_copy.cpp"):
                raise PatchError(f"the diff edits {target}; only {HARNESS_FILE} may change")
            continue
        if raw.startswith("--- "):
            target = raw[4:].strip().split("\t")[0]
            if not target.endswith("async_copy.cpp"):
                raise PatchError(f"the diff edits {target}; only {HARNESS_FILE} may change")
            continue
        match = HUNK.match(raw)
        if match:
            current = {"start": int(match.group(1)), "old": [], "new": []}
            hunks.append(current)
            continue
        if raw.startswith("@@"):
            raise PatchError(f"malformed hunk header: {raw}")
        if current is None:
            continue
        if raw.startswith("\\"):
            continue  # "\ No newline at end of file"
        tag, body = (raw[0], raw[1:]) if raw else (" ", "")
        if tag == " ":
            current["old"].append(body)
            current["new"].append(body)
        elif tag == "-":
            current["old"].append(body)
        elif tag == "+":
            current["new"].append(body)
        else:  # a context line that lost its leading space
            current["old"].append(raw)
            current["new"].append(raw)
    if not hunks:
        raise PatchError("the diff has no hunks")
    for hunk in hunks:
        if hunk["old"] == hunk["new"]:
            raise PatchError("a hunk changes nothing")
        if not hunk["old"]:
            raise PatchError("a hunk has no context or removed lines to anchor it")
    return hunks


def apply_diff(source, diff):
    """Apply hunks by content, nearest to the stated line; line counts are ignored."""
    lines = source.split("\n")
    offset = 0
    for number, hunk in enumerate(parse_hunks(diff), 1):
        old, new = hunk["old"], hunk["new"]
        hint = hunk["start"] - 1 + offset
        for normalize in (lambda s: s, lambda s: s.rstrip(), lambda s: " ".join(s.split())):
            want = [normalize(l) for l in old]
            found = [i for i in range(len(lines) - len(old) + 1)
                     if [normalize(l) for l in lines[i:i + len(old)]] == want]
            if found:
                break
        if not found:
            raise PatchError(f"hunk {number} does not match the source: its context and "
                             f"removed lines were not found")
        at = min(found, key=lambda i: abs(i - hint))
        lines[at:at + len(old)] = new
        offset += len(new) - len(old)
    return "\n".join(lines)


def check_region(planted, patched):
    """Reject edits outside the CacheHost member functions."""
    a = planted.splitlines(keepends=True)
    b = patched.splitlines(keepends=True)
    start, end = region_bounds(a)
    head, tail = a[:start], a[end:]
    if b[:start] != head or len(b) < len(head) + len(tail) or b[len(b) - len(tail):] != tail:
        raise PatchError(f"the diff changes {HARNESS_FILE} outside the CacheHost member "
                         f"functions (lines {start + 1}-{end})")


# Trace records and the harness's ground-truth counters. A patch must keep
# at least as many of each as the unplanted source has, so that it cannot pass
# by recording less or by no longer counting unsafe reclaims.
INSTRUMENTATION = ("recorder_.record(", "hazards_.push_back(", "++misattributions_",
                   "++failed_publications_", "pool_.reclaim(", "pool_.retire(")


def check_instrumentation(pristine, patched):
    def region(text):
        lines = text.splitlines(keepends=True)
        start, end = region_bounds(lines)
        return "".join(lines[start:end])
    before, after = region(pristine), region(patched)
    for token in INSTRUMENTATION:
        if after.count(token) < before.count(token):
            raise PatchError(f"the diff removes instrumentation ({token.strip('(')}); trace "
                             f"records and harness counters must stay")


# ---------------------------------------------------------------- build and run

def cmake_cache(build_dir):
    path = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.isfile(path):
        raise ToolingError(f"{path} not found; configure and build the project first")
    values = {}
    for line in read_text(path).splitlines():
        match = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def find_tool(build_dir, name):
    for candidate in (name, name + ".exe"):
        path = os.path.join(build_dir, candidate)
        if os.path.isfile(path):
            return path
    raise ToolingError(f"cannot find {name} in {build_dir}; build the project first")


class Builder:
    """Compiles the exploration tool from a scratch copy of the harness."""

    def __init__(self, build_dir, work_dir):
        cache = cmake_cache(build_dir)
        self.cxx = cache.get("CMAKE_CXX_COMPILER")
        if not self.cxx:
            raise ToolingError("CMakeCache.txt names no C++ compiler")
        build_type = cache.get("CMAKE_BUILD_TYPE", "").upper()
        flags = cache.get("CMAKE_CXX_FLAGS", "") + " " + cache.get(
            f"CMAKE_CXX_FLAGS_{build_type}", "-O2" if not build_type else "")
        self.flags = shlex.split(flags) + ["-std=c++20"]
        self.library = None
        for name in ("libstateguard.a", "stateguard.lib"):
            path = os.path.join(build_dir, name)
            if os.path.isfile(path):
                self.library = path
        if self.library is None:
            raise ToolingError(f"cannot find the stateguard library in {build_dir}")
        if "cl" == os.path.basename(self.cxx).lower().split(".")[0]:
            raise ToolingError("the scratch rebuild supports GCC and Clang only")
        self.work = work_dir
        self.src = os.path.join(work_dir, "harness")
        self.include = os.path.join(ROOT, "include")

    def compile(self, source, output, extra=()):
        command = [self.cxx, *self.flags, *extra, "-I", self.include, "-I", self.src,
                   "-c", source, "-o", output]
        return subprocess.run(command, capture_output=True, text=True)

    def build(self):
        """Return (ok, compiler output). Rebuilds only the patched harness."""
        main_object = os.path.join(self.work, "explore_main.o")
        explore_source = os.path.join(ROOT, EXPLORE_SOURCE)
        if (not os.path.isfile(main_object)
                or os.path.getmtime(main_object) < os.path.getmtime(explore_source)):
            proc = self.compile(explore_source, main_object, ['-DSTATEGUARD_VERSION="agent-fix"'])
            if proc.returncode != 0:
                raise ToolingError("cannot compile the exploration tool:\n" + proc.stderr[-2000:])
        harness_object = os.path.join(self.work, "async_copy.o")
        if os.path.exists(harness_object):
            os.remove(harness_object)
        proc = self.compile(os.path.join(self.src, "async_copy.cpp"), harness_object,
                            ["-Wall", "-Wextra", "-Wpedantic"])
        if proc.returncode != 0:
            return False, proc.stderr
        binary = self.explorer()
        link = subprocess.run([self.cxx, *self.flags, main_object, harness_object, self.library,
                               "-o", binary], capture_output=True, text=True)
        if link.returncode != 0:
            return False, link.stderr
        return True, proc.stderr

    def explorer(self):
        return os.path.join(self.work, "stateguard-async-copy-explore")


def run_explore(binary, config, choices=None, trace_path=None, failure_path=None):
    command = [binary, "--config", config, "--json"]
    if choices is not None:
        command += ["--choices", ",".join(str(c) for c in choices)]
        if trace_path:
            command += ["--trace", trace_path]
    if failure_path:
        command += ["--failure-trace", failure_path]
    try:
        proc = subprocess.run(command, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None, "exploration timed out after 600 s"
    if proc.returncode not in (0, 1):
        return None, (proc.stderr or proc.stdout).strip()[-2000:]
    try:
        return json.loads(proc.stdout), None
    except json.JSONDecodeError:
        return None, "exploration printed invalid JSON: " + proc.stdout[:500]


def diagnose(build_dir, trace_path):
    """stateguard-check --json and the stateguard-reduce witness for a trace."""
    checker = find_tool(build_dir, "stateguard-check")
    reducer = find_tool(build_dir, "stateguard-reduce")
    proc = subprocess.run([checker, "--json", trace_path], capture_output=True, text=True)
    if proc.returncode not in (0, 1, 2):
        raise ToolingError(f"stateguard-check failed: {proc.stderr.strip()}")
    report = json.loads(proc.stdout)
    proc = subprocess.run([reducer, trace_path], capture_output=True, text=True)
    if proc.returncode == 0:
        witness = proc.stdout.rstrip("\n") + "\n" + proc.stderr.rstrip("\n")
    else:
        witness = "(no reducible finding: " + (proc.stderr.strip() or "none") + ")"
    return report, witness


def compact_report(report, limit=6000):
    text = json.dumps(report, separators=(",", ":"))
    if len(text) <= limit:
        return text
    trimmed = dict(report)
    trimmed["diagnostics"] = [d for d in report.get("diagnostics", [])
                              if not d.get("follow_on")][:8]
    trimmed["note"] = "follow-on findings omitted"
    return json.dumps(trimmed, separators=(",", ":"))[:limit]


COUNT_KEYS = ("schedules", "passed", "clean", "violation", "inconclusive", "corrupted_bytes",
              "unsafe_reclaim", "misattributed", "lost_completion", "failed_publication",
              "unreclaimed")


def counts(result):
    return {k: result.get(k) for k in COUNT_KEYS} if result else None


def failure_text(result, report, witness):
    first = result["first_failure"]
    lines = [f"Exploration counts: {json.dumps(counts(result))}",
             f"First failing schedule (index {first['index']}, choices "
             f"{','.join(map(str, first['choices']))}): " + "; ".join(first["schedule"]),
             f"Harness ground truth for it: delivered_intact={first['delivered_intact']}, "
             f"unsafe_reclaims={first['unsafe_reclaims']}, "
             f"misattributions={first['misattributions']}, "
             f"unacknowledged={first['unacknowledged']}, "
             f"failed_publications={first['failed_publications']}, "
             f"unreclaimed={first['unreclaimed']}",
             "", "stateguard-check --json on its trace:", compact_report(report),
             "", "stateguard-reduce witness (reduced trace, then the finding it keeps):",
             witness]
    return "\n".join(lines)


# ---------------------------------------------------------------- model

class Model:
    def __init__(self, args):
        self.name = args.model
        self.args = args
        self.fake = args.model.startswith("fake:")

    def body(self, messages):
        return {"model": self.name, "messages": messages}

    def complete(self, messages):
        """Return (text, usage dict, seconds)."""
        started = time.monotonic()
        if self.fake:
            path = self.name[len("fake:"):]
            try:
                text = read_text(path)
            except OSError as exc:
                raise ToolingError(f"fake model cannot read {path}: {exc}") from exc
            return text, {"prompt_tokens": 0, "completion_tokens": 0}, 0.0
        api_key = os.environ.get(self.args.api_key_env)
        if not api_key:
            raise ToolingError(f"environment variable {self.args.api_key_env} is not set")
        try:
            text, payload = chat_completion(self.args.base_url, api_key, self.body(messages),
                                            self.args.timeout)
        except ExplainError as exc:
            raise ToolingError(str(exc)) from exc
        usage = payload.get("usage") or {}
        return text, usage, time.monotonic() - started


def usage_tokens(usage):
    prompt = usage.get("prompt_tokens", usage.get("input_tokens", 0)) or 0
    completion = usage.get("completion_tokens", usage.get("output_tokens", 0)) or 0
    return int(prompt), int(completion)


# ---------------------------------------------------------------- the loop

def prepare(args, bug_name):
    bug = BUGS[bug_name]
    work = os.path.join(os.path.abspath(args.build_dir), "agent-fix", bug_name)
    os.makedirs(os.path.join(work, "harness"), exist_ok=True)
    pristine = read_text(os.path.join(ROOT, HARNESS_FILE))
    header = read_text(os.path.join(ROOT, HEADER_FILE))
    planted = plant(pristine, bug)
    write_text(os.path.join(work, "harness", "async_copy.hpp"), header)
    write_text(os.path.join(work, "planted_async_copy.cpp"), planted)
    write_text(os.path.join(work, "harness", "async_copy.cpp"), planted)
    return bug, work, pristine, planted, header_excerpt(header)


def first_messages(bug_name, bug, planted, header, failing):
    lines = planted.splitlines(keepends=True)
    start, end = region_bounds(lines)
    system = SYSTEM_PROMPT.format(rules=event_rules(), first=start + 1, last=end)
    region = []
    for signature in bug["functions"]:
        line, text = function_text(lines, signature)
        region.append(f"// {HARNESS_FILE}, line {line}\n{text}")
    user = "\n".join([
        f"Configuration `{bug['config']}` (default HostOptions) fails in the harness.",
        failing,
        "",
        f"Declarations from {HEADER_FILE} (read-only):",
        header,
        "",
        f"Source region from {HARNESS_FILE} (the CacheHost functions involved):",
        "\n\n".join(region),
        "",
        "Write the unified diff that makes every schedule of this configuration pass.",
    ])
    return [{"role": "system", "content": system}, {"role": "user", "content": user}]


def evaluate(args, builder, bug, work, pristine, planted, completion, failing_choices):
    """Apply, build, explore, replay. Returns the attempt record fields."""
    record = {"diff": None, "stage": None, "accepted": False}
    try:
        diff = extract_diff(completion)
        record["diff"] = diff
        patched = apply_diff(planted, diff)
        check_region(planted, patched)
        check_instrumentation(pristine, patched)
    except PatchError as exc:
        record.update(stage="patch", reason=str(exc))
        return record, f"Your reply was rejected before building: {exc}"
    write_text(os.path.join(work, "harness", "async_copy.cpp"), patched)
    record["applied_diff"] = "".join(difflib.unified_diff(
        planted.splitlines(keepends=True), patched.splitlines(keepends=True),
        "a/" + HARNESS_FILE, "b/" + HARNESS_FILE))
    ok, output = builder.build()
    if not ok:
        errors = "\n".join(output.splitlines()[:40])
        record.update(stage="build", reason="the patched harness does not compile",
                      compiler=errors)
        return record, "The patched file does not compile:\n" + errors
    explored, error = run_explore(builder.explorer(), bug["config"],
                                  failure_path=os.path.join(work, "failure.sgtrace"))
    if explored is None:
        record.update(stage="explore", reason=f"exploration failed: {error}")
        return record, f"The patched harness crashed or threw during exploration: {error}"
    record["explore"] = counts(explored)
    # The demo's order (every copy as late as the host allows) and the planted
    # build's first failing order. A fix may change the host's choice points;
    # a failing order that no longer exists cannot reproduce.
    reproduced = []
    record["replay"] = {}
    for label, choices in (("demo_schedule", []), ("failing_schedule", failing_choices)):
        replayed, error = run_explore(builder.explorer(), bug["config"], choices=choices)
        if replayed is None and error and error.startswith("harness error: schedule choice"):
            record["replay"][label] = {"exists": False, "detail": error}
            continue
        if replayed is None:
            record["replay"][label] = {"error": error}
            reproduced.append(label)
            continue
        record["replay"][label] = counts(replayed)
        if replayed["passed"] != replayed["schedules"]:
            reproduced.append(label)
    if explored["passed"] == explored["schedules"] and not reproduced:
        record.update(stage="accepted", accepted=True,
                      reason="every schedule passed and the original failure no longer "
                             "reproduces")
        return record, None
    if explored["first_failure"] is None:
        record.update(stage="replay", reason="the original failure still reproduces: "
                      + ", ".join(reproduced))
        return record, "The original failing schedule still fails."
    report, witness = diagnose(args.build_dir, os.path.join(work, "failure.sgtrace"))
    record.update(stage="explore",
                  reason=f"{explored['schedules'] - explored['passed']} of "
                         f"{explored['schedules']} schedules still fail",
                  first_failure=explored["first_failure"], check=report, witness=witness)
    return record, ("The patch built, but schedules still fail.\n"
                    + failure_text(explored, report, witness))


def run_loop(args):
    bug_name = args.bug
    bug, work, pristine, planted, header = prepare(args, bug_name)
    builder = Builder(os.path.abspath(args.build_dir), work)
    model = Model(args)
    transcript = {"bug": bug_name, "config": bug["config"], "induced": bug["induced"],
                  "symptom": bug["symptom"], "model": args.model,
                  "max_attempts": args.max_attempts,
                  "prices_usd_per_million": {"input": PRICE_INPUT_PER_M,
                                             "output": PRICE_OUTPUT_PER_M},
                  "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                  "attempts": []}

    print(f"bug {bug_name}: {bug['symptom']}")
    print(f"configuration {bug['config']}; scratch copy in {work}")
    ok, output = builder.build()
    if not ok:
        raise ToolingError("the planted harness does not compile:\n" + output[-2000:])
    planted_run, error = run_explore(builder.explorer(), bug["config"],
                                     failure_path=os.path.join(work, "planted.sgtrace"))
    if planted_run is None:
        raise ToolingError(f"exploring the planted harness failed: {error}")
    if planted_run["first_failure"] is None:
        raise ToolingError("the planted bug does not reproduce: every schedule passes")
    failing_choices = planted_run["first_failure"]["choices"]
    report, witness = diagnose(args.build_dir, os.path.join(work, "planted.sgtrace"))
    failing = failure_text(planted_run, report, witness)
    transcript["planted"] = {"explore": counts(planted_run),
                             "first_failure": planted_run["first_failure"],
                             "check": report, "witness": witness}
    print(f"planted: {planted_run['schedules']} schedules, {planted_run['passed']} pass, "
          f"{planted_run['violation']} violation, {planted_run['inconclusive']} inconclusive, "
          f"{planted_run['corrupted_bytes']} corrupted bytes")

    messages = first_messages(bug_name, bug, planted, header, failing)
    if args.dry_run:
        print(json.dumps({"url": args.base_url.rstrip("/") + "/chat/completions",
                          "headers": {"Authorization": "Bearer <redacted>",
                                      "Content-Type": "application/json"},
                          "body": model.body(messages)}, indent=1))
        return EXIT_ACCEPTED

    accepted = False
    for number in range(1, args.max_attempts + 1):
        started = time.monotonic()
        sent = [dict(m) for m in messages]
        completion, usage, api_seconds = model.complete(messages)
        prompt_tokens, completion_tokens = usage_tokens(usage)
        record, feedback = evaluate(args, builder, bug, work, pristine, planted, completion,
                                    failing_choices)
        attempt = {"attempt": number, "messages": sent, "completion": completion,
                   "usage": usage, "prompt_tokens": prompt_tokens,
                   "completion_tokens": completion_tokens,
                   "cost_usd": round(cost_usd(prompt_tokens, completion_tokens), 8),
                   "api_seconds": round(api_seconds, 2),
                   "wall_seconds": round(time.monotonic() - started, 2), **record}
        transcript["attempts"].append(attempt)
        explore = record.get("explore") or {}
        print(f"attempt {number}: {record['stage']}: {record['reason']} "
              f"({prompt_tokens} in, {completion_tokens} out, ${attempt['cost_usd']:.6f}, "
              f"{attempt['wall_seconds']} s"
              + (f"; {explore.get('passed')}/{explore.get('schedules')} pass" if explore else "")
              + ")")
        if record["accepted"]:
            accepted = True
            break
        # The next request is the original two messages plus one message with
        # this attempt's reply and the new diagnostics.
        messages = messages[:2] + [{"role": "user", "content": (
            f"Attempt {number} replied:\n{completion}\n\n{feedback}\n\nReply with a new "
            "complete unified diff against the original file shown above (not against the "
            "rejected diff), and nothing else.")}]

    attempts = transcript["attempts"]
    transcript["result"] = {
        "accepted": accepted, "attempts": len(attempts),
        "prompt_tokens": sum(a["prompt_tokens"] for a in attempts),
        "completion_tokens": sum(a["completion_tokens"] for a in attempts),
        "cost_usd": round(sum(a["cost_usd"] for a in attempts), 8),
        "wall_seconds": round(sum(a["wall_seconds"] for a in attempts), 2),
        "schedules_explored": sum((a.get("explore") or {}).get("schedules") or 0
                                  for a in attempts),
        "accepted_diff": attempts[-1]["applied_diff"] if accepted else None,
    }
    if args.transcript:
        os.makedirs(os.path.dirname(os.path.abspath(args.transcript)), exist_ok=True)
        write_text(args.transcript, json.dumps(transcript, indent=1) + "\n")
    # Leave the scratch copy at the planted state unless a fix was accepted.
    if not accepted:
        write_text(os.path.join(work, "harness", "async_copy.cpp"), planted)
    result = transcript["result"]
    print(f"{'accepted' if accepted else 'not fixed'} after {result['attempts']} attempt(s): "
          f"{result['prompt_tokens']} in, {result['completion_tokens']} out, "
          f"${result['cost_usd']:.6f}, {result['wall_seconds']} s")
    if accepted:
        print(result["accepted_diff"], end="")
    return EXIT_ACCEPTED if accepted else EXIT_EXHAUSTED


# ---------------------------------------------------------------- replay

def replay(path, pace):
    transcript = json.loads(read_text(path))

    def say(text=""):
        for line in str(text).splitlines() or [""]:
            print(line)
            sys.stdout.flush()
            if pace:
                time.sleep(pace)

    say(f"== bug {transcript['bug']} (configuration {transcript['config']}, model "
        f"{transcript['model']}, {transcript['started']})")
    say(transcript["symptom"])
    planted = transcript["planted"]
    say(f"== planted harness: {json.dumps(planted['explore'])}")
    first = planted["first_failure"]
    say(f"first failing schedule {first['index']}: " + "; ".join(first["schedule"]))
    say(f"checker: {planted['check'].get('status')}, first finding "
        + next((f"{d.get('category')}: {d.get('message')}"
                for d in planted["check"].get("diagnostics", [])), "none"))
    say("witness:")
    say(planted["witness"])
    for attempt in transcript["attempts"]:
        say()
        say(f"== attempt {attempt['attempt']}: {attempt['prompt_tokens']} input tokens, "
            f"{attempt['completion_tokens']} output tokens, ${attempt['cost_usd']:.6f}, "
            f"{attempt['wall_seconds']} s")
        say(f"request: {len(attempt['messages'])} messages, "
            f"{sum(len(m['content']) for m in attempt['messages']):,} characters")
        say("model reply:")
        say(attempt["completion"])
        say(f"harness: {attempt['stage']}: {attempt['reason']}")
        if attempt.get("compiler"):
            say(attempt["compiler"])
        if attempt.get("explore"):
            say(f"exploration: {json.dumps(attempt['explore'])}")
        if attempt.get("replay"):
            say(f"replays: {json.dumps(attempt['replay'])}")
    result = transcript["result"]
    say()
    say(f"== {'accepted' if result['accepted'] else 'not fixed'} after {result['attempts']} "
        f"attempt(s): {result['prompt_tokens']} input tokens, {result['completion_tokens']} "
        f"output tokens, ${result['cost_usd']:.6f}, {result['wall_seconds']} s, "
        f"{result['schedules_explored']} schedules explored")
    return EXIT_ACCEPTED if result["accepted"] else EXIT_EXHAUSTED


# ---------------------------------------------------------------- main

def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bug", choices=sorted(BUGS), help="planted bug to fix")
    parser.add_argument("--build-dir", help="configured and built Stateguard build directory")
    parser.add_argument("--model", default=DEFAULT_MODEL,
                        help=f"model id (default {DEFAULT_MODEL}); fake:PATH replies with "
                             "the file at PATH and makes no request")
    parser.add_argument("--max-attempts", type=int, default=5, help="attempts (default 5)")
    parser.add_argument("--transcript", help="write the whole run as JSON to this file")
    parser.add_argument("--replay", metavar="FILE",
                        help="print a saved transcript at reading pace; no API calls")
    parser.add_argument("--pace", type=float, default=0.03,
                        help="seconds per line for --replay (default 0.03)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the first request with the key redacted and exit")
    parser.add_argument("--list-bugs", action="store_true", help="print the bug table")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL,
                        help=f"OpenAI-compatible API base (default {DEFAULT_BASE_URL})")
    parser.add_argument("--api-key-env", default=DEFAULT_KEY_ENV,
                        help=f"environment variable holding the API key "
                             f"(default {DEFAULT_KEY_ENV})")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="request timeout in seconds (default 120)")
    args = parser.parse_args(argv)
    if args.max_attempts < 1:
        parser.error("--max-attempts must be at least 1")
    if not (args.list_bugs or args.replay) and not (args.bug and args.build_dir):
        parser.error("--bug and --build-dir are required")
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.list_bugs:
        for name in sorted(BUGS):
            bug = BUGS[name]
            print(f"{name}\n  config: {bug['config']}\n  file: {HARNESS_FILE}, "
                  f"{bug['functions'][0].split('CacheHost::')[1].rstrip('(')}()\n"
                  f"  induced behaviour: {bug['induced']}\n  planted failure: {bug['symptom']}")
        return EXIT_ACCEPTED
    try:
        if args.replay:
            return replay(args.replay, args.pace)
        return run_loop(args)
    except (ToolingError, OSError, json.JSONDecodeError, KeyError) as exc:
        print(f"agent_fix: {exc}", file=sys.stderr)
        return EXIT_TOOLING
    except KeyboardInterrupt:
        return EXIT_TOOLING


if __name__ == "__main__":
    sys.exit(main())
