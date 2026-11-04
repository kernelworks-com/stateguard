# Agent fix loop results

Live runs of `scripts/agent_fix.py` on 2026-11-04 with the default model,
`muse-spark-1.3-contributor`, on the Meta Model API, five attempts allowed per
bug. Each row comes from the transcript in [`transcripts/`](transcripts/); the
loop is described in the
[technical reference](../REFERENCE.md#agent-fix-loop-experimental).

| Bug | Configuration | Result | Attempts | Input tokens | Output tokens | Cost | Wall time | Planted: schedules passing | Schedules explored for the accepted patch |
|---|---|---|---|---|---|---|---|---|---|
| [`premature-reclaim`](transcripts/premature-reclaim.json) | `shared-prefix` | accepted | 1 | 3,555 | 10,130 | $0.0024 | 84.9 s | 0 of 15,982 | 15,982 |
| [`counted-redelivery`](transcripts/counted-redelivery.json) | `redelivery` | accepted | 1 | 3,169 | 8,597 | $0.0020 | 65.5 s | 2,704 of 15,982 | 15,982 |
| [`failed-load-publish`](transcripts/failed-load-publish.json) | `failed-retry` | accepted | 1 | 3,212 | 12,648 | $0.0029 | 99.6 s | 0 of 15,982 | 41,550 |
| [`tag-only-attribution`](transcripts/tag-only-attribution.json) | `reconnect-replay` | accepted | 1 | 3,811 | 7,964 | $0.0020 | 70.3 s | 3 of 68 | 24 |
| [`reclaim-on-timeout`](transcripts/reclaim-on-timeout.json) | `timeout-wait` | accepted | 1 | 3,581 | 8,470 | $0.0021 | 73.7 s | 0 of 15 | 15 |
| **Total** | | 5 of 5 | 5 | 17,328 | 47,809 | $0.0113 | 393.9 s | | |

Cost is computed from the `usage` the API returned, at $0.10 per million input
tokens and $0.20 per million output tokens. Almost all output tokens are
reasoning: the API reported 7,780 to 12,369 `reasoning_tokens` per reply, and
the visible replies (the diffs) were 101 to 279 tokens. Wall time covers the request (62 to
96 s of each attempt) plus applying the diff, rebuilding the scratch harness,
the exhaustive exploration, and the two replays.

The schedule counts differ between configurations because the scenarios
differ: the shared-prefix scenario has 15,982 completion orders, its retried
failed load 41,550 (the retry adds a copy), the reconnect scenario with
replayed notifications 24 for a correct host (68 for the planted one, whose
lost and extra completions change the choice points), and the timeout scenario
15.

What the model changed, from the accepted diffs in the transcripts:

- `premature-reclaim`: `release()` returns early unless the policy is
  `Immediate` or the buffer has no holds.
- `counted-redelivery`: `poll()` skips a repeated notification when
  `idempotent_notifications` is set.
- `failed-load-publish`: `poll()` retries a failed load unless
  `publish_failed_loads` is set, and publishes otherwise.
- `tag-only-attribution`: `attribute()` looks the notification up by its full
  route when attribution is `Route`.
- `reclaim-on-timeout`: `expire()` drops the holds only under
  `TimeoutPolicy::Reclaim`.

All five are one-site changes to the function the bug was planted in, and
each restores the option switch the planted change removed. Afterwards, each
accepted scratch harness was run once on all 13 configurations of
`stateguard-async-copy-explore`; the pass and schedule counts matched the
unmodified harness on every one. That check was done by hand and is not part
of the acceptance rule.

Limits of this result: five bugs, one run each, every one a single-site
change of a few lines, planted in code that keeps the matching option switch
nearby as a hint. It shows that the loop's plumbing and acceptance rule work
end to end; it is not a measure of how the model does on unplanted bugs.

Replay a run without calling the API:

```sh
python3 scripts/agent_fix.py --replay docs/agent-fix/transcripts/premature-reclaim.json
```
