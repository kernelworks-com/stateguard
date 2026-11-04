#!/usr/bin/env python3
"""Explain the first stateguard-check finding with an OpenAI-compatible chat endpoint.

Runs stateguard-check --json (and stateguard-timeline --json for timestamped
traces), prints the checker's own result, then sends the trace and the report
to a chat-completions endpoint and prints the reply, labelled as not evidence.

The exit code is the checker's. Network and API errors go to stderr and exit 4
after the checker result has been printed. Python 3.9+, standard library only.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

DEFAULT_BASE_URL = "https://api.meta.ai/v1"
DEFAULT_MODEL = "muse-spark-1.3"
DEFAULT_KEY_ENV = "META_API_KEY"
DEFAULT_MAX_TRACE_BYTES = 64 * 1024
EXIT_EXPLAIN_ERROR = 4
EXIT_USAGE = 3

SYSTEM_PROMPT = """\
You explain diagnostics from stateguard, a checker for recorded lifecycle \
events of inference KV-cache memory. The event model:
- An allocation is identified by id and generation. Reusing an id starts a \
new generation; an access names the generation it targets, and an access to \
an old generation is stale.
- Bound states are immutable: published content must not be written again.
- A submitted operation holds a lease on every allocation it reads or writes \
until that operation completes. Completion is the only proof that an access \
ended.
- A cancellation signal or acknowledgment releases no lease; an operation \
cancelled but not completed may still be touching memory.
- Retire then reclaim is legal only after every lease on that generation has \
been released by completion.
- When the capture has gaps (lost events, missing end marker), findings are \
unconfirmed and must be described as such.
Rules for your answer: use only the trace and the JSON report you are given. \
Never invent findings, events, identifiers, or timings that are not in them. \
If the report has no findings, say so and do not explain anything else."""

USER_REQUEST = (
    "Explain the first finding in the report in plain language. Name the "
    "ordering edge that caused it (which event happened before which "
    "required event). Suggest a fix in the producer's terms (what the code "
    "that emits these events should wait for or do differently). Answer in "
    "under 250 words."
)


class ExplainError(Exception):
    pass


def find_tool(name, explicit, build_dir):
    if explicit:
        return explicit
    if build_dir:
        for candidate in (name, name + ".exe"):
            path = os.path.join(build_dir, candidate)
            if os.path.isfile(path):
                return path
    found = shutil.which(name)
    if not found:
        raise ExplainError(f"cannot find {name}; pass --build-dir or --{name.split('-')[-1]}")
    return found


def run_json(tool, trace_path):
    proc = subprocess.run([tool, "--json", trace_path], capture_output=True, text=True)
    if proc.returncode not in (0, 1, 2):
        raise ExplainError(f"{os.path.basename(tool)} failed ({proc.returncode}): {proc.stderr.strip()}")
    try:
        return proc.returncode, json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise ExplainError(f"{os.path.basename(tool)} printed invalid JSON: {exc}") from exc


def has_time_events(trace_text):
    return any(line.startswith("time|") for line in trace_text.splitlines())


def build_messages(trace_text, check_report, timeline_report):
    parts = ["Trace:", "```", trace_text.rstrip("\n"), "```", "",
             "stateguard-check --json:", json.dumps(check_report, indent=1)]
    if timeline_report is not None:
        parts += ["", "stateguard-timeline --json:", json.dumps(timeline_report, indent=1)]
    parts += ["", USER_REQUEST]
    return [{"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": "\n".join(parts)}]


def checker_summary(report):
    lines = [report.get("status", "unknown")]
    diagnostics = report.get("diagnostics") or []
    if diagnostics:
        first = diagnostics[0]
        tag = "" if first.get("confirmed", True) else " (unconfirmed)"
        lines.append(f"first finding: event {first.get('index')} {first.get('category')}{tag}: "
                     f"{first.get('message')}")
    else:
        lines.append("no findings")
    return "\n".join(lines)


def post_chat(base_url, api_key, body, timeout):
    request = urllib.request.Request(
        base_url.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {api_key}"},
        method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:500]
        raise ExplainError(f"endpoint returned HTTP {exc.code}: {detail}") from exc
    except (urllib.error.URLError, OSError) as exc:
        raise ExplainError(f"request failed: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ExplainError(f"endpoint returned invalid JSON: {exc}") from exc
    try:
        content = payload["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        raise ExplainError("endpoint response has no choices[0].message.content") from exc
    if isinstance(content, list):  # some servers return content parts
        content = "".join(p.get("text", "") for p in content if isinstance(p, dict))
    return str(content).strip()


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace", help="path to a .sgtrace file")
    parser.add_argument("--build-dir", help="directory containing the stateguard tools")
    parser.add_argument("--checker", help="path to stateguard-check")
    parser.add_argument("--timeline", help="path to stateguard-timeline")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL,
                        help=f"OpenAI-compatible API base (default {DEFAULT_BASE_URL})")
    parser.add_argument("--model", default=DEFAULT_MODEL, help=f"model id (default {DEFAULT_MODEL})")
    parser.add_argument("--api-key-env", default=DEFAULT_KEY_ENV,
                        help=f"environment variable holding the API key (default {DEFAULT_KEY_ENV})")
    parser.add_argument("--max-trace-bytes", type=int, default=DEFAULT_MAX_TRACE_BYTES,
                        help="refuse larger traces (default 65536)")
    parser.add_argument("--timeout", type=float, default=60.0, help="request timeout in seconds")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the request with the key redacted; no network")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    try:
        size = os.path.getsize(args.trace)
        if size > args.max_trace_bytes:
            raise ExplainError(f"trace is {size} bytes, over --max-trace-bytes "
                               f"{args.max_trace_bytes}; reduce it with stateguard-reduce first")
        with open(args.trace, encoding="utf-8") as handle:
            trace_text = handle.read()
        checker = find_tool("stateguard-check", args.checker, args.build_dir)
        check_code, check_report = run_json(checker, args.trace)
        timeline_report = None
        if has_time_events(trace_text):
            timeline = find_tool("stateguard-timeline", args.timeline, args.build_dir)
            _, timeline_report = run_json(timeline, args.trace)
    except (OSError, ExplainError) as exc:
        print(f"explain_trace: {exc}", file=sys.stderr)
        return EXIT_USAGE

    print(checker_summary(check_report))
    sys.stdout.flush()

    body = {"model": args.model,
            "messages": build_messages(trace_text, check_report, timeline_report)}
    if args.dry_run:
        print(json.dumps({"url": args.base_url.rstrip("/") + "/chat/completions",
                          "headers": {"Authorization": "Bearer <redacted>",
                                      "Content-Type": "application/json"},
                          "body": body}, indent=1))
        return check_code

    api_key = os.environ.get(args.api_key_env)
    if not api_key:
        print(f"explain_trace: environment variable {args.api_key_env} is not set",
              file=sys.stderr)
        return EXIT_EXPLAIN_ERROR
    try:
        text = post_chat(args.base_url, api_key, body, args.timeout)
    except ExplainError as exc:
        print(f"explain_trace: {exc}", file=sys.stderr)
        return EXIT_EXPLAIN_ERROR
    print()
    print("LLM-assisted explanation (not evidence):")
    print(text)
    return check_code


if __name__ == "__main__":
    sys.exit(main())
