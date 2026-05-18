# xwave AI Response Dictionary

This reference collects the response fields an AI agent should expect when using `xwave ai query`. Keep the main `SKILL.md` focused on request examples and workflow; update this file when action-specific response fields change.

## Stability Rule

The top-level envelope is the stable contract across all actions:

```text
ok/action/session/summary/data/findings/suggested_next_actions/warnings/error/meta
```

Fields inside `summary`, `data`, and `findings` are action-specific. Treat this file as a practical dictionary, not a replacement for inspecting real output from the target xwave build and FSDB.

Recommended pattern:

```bash
tools/xwave-env ai query --json '<request>' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.keys()); print(d.get("summary",{})); print(d.get("data",{}).keys() if isinstance(d.get("data"),dict) else None)'
```

For production extraction, use `.get()` and defaults unless a field has been verified on the current output:

```bash
tools/xwave-env ai query --json '<request>' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); rows=d.get("data",{}).get("events",[]); print(len(rows))'
```

## Top-Level Envelope

| Field | Meaning | AI usage |
| --- | --- | --- |
| `ok` | Boolean success flag. | Check this before reading action data. |
| `action` | Echoed action name. | Verify the response matches the requested action. |
| `session` | Session metadata when relevant. | Capture `id`, `fsdb`, `reused`, or restart information for follow-up requests. |
| `summary` | Compact action-specific summary. | Good for quick decisions and user-facing summaries. |
| `data` | Action-specific structured payload. | Use for evidence extraction and statistics. |
| `findings` | List of detected issues or facts. | Use for anomaly/protocol/debug conclusions. |
| `suggested_next_actions` | Machine-readable recovery or follow-up hints. | Prefer these over inventing recovery steps. |
| `warnings` | Non-fatal warnings. | Preserve in debug reports. |
| `error` | Structured error object or null. | Read `error.code` and `recoverable`; do not parse stderr. |
| `meta` | Execution metadata. | Check `elapsed_ms`, `truncated`, and limit-related signals. |

## Common Nested Objects

### `error`

Typical fields:

```json
{
  "code": "SIGNAL_NOT_FOUND",
  "message": "signal top.ready not found",
  "recoverable": true,
  "candidates": [],
  "suggested_actions": []
}
```

AI usage:
- On `SIGNAL_NOT_FOUND`, run `scope.list` around the likely parent scope.
- On parser or request errors, fix the request before retrying.
- On recoverable session errors, try `session.doctor`, `session.gc`, or `session.open`.

### `value`

Typical fields:

```json
{
  "text": "8'hff",
  "bits": "11111111",
  "hex": "0xff",
  "unsigned": 255,
  "signed": -1,
  "known": true
}
```

Unknown values usually contain:

```json
{
  "known": false,
  "unknown_reason": "contains_x"
}
```

AI usage:
- Treat `known:false`, `status:"unknown"`, and `pass:null` as inconclusive, not failed.
- Do not compare raw display text when `bits`, `hex`, or numeric fields are available.

### Time Fields

Common forms include:

```text
time/time_ps/begin/end/begin_ps/end_ps/duration/duration_ps/window_ps/delta_ps/resolved_time/resolved_time_range
```

AI usage:
- Prefer numeric `*_ps` fields for sorting, deltas, and window checks.
- Preserve original text fields for user-facing reports.
- Prefer `resolved_time` and `resolved_time_range` when a query used TimeSpec input such as `@deadlock-20ns` or `@deadlock-10cycle(top.clk)`; do not redo cursor math in the agent.

## Action Groups

### Session Actions

Actions:

```text
session.open/session.list/session.doctor/session.gc/session.kill
```

Common payloads:
- `session.id`
- `session.fsdb`
- `session.reused`
- `session.restarted`
- `summary.count`
- `summary.healthy`
- `data.sessions`

Extraction example:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"session.list"}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print([s.get("id") for s in d.get("data",{}).get("sessions",[])])'
```

### Scope Actions

Action:

```text
scope.list
```

Common payloads:
- `summary.path`
- `summary.recursive`
- `summary.signal_count`
- `summary.truncated`
- `data.signals`
- signal fields such as `name`, `kind`, `width`

Extraction example:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"scope.list","target":{"session_id":1},"args":{"path":"top","recursive":true},"limits":{"max_rows":20}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print([s.get("name") for s in d.get("data",{}).get("signals",[])])'
```

### Cursor Actions

Actions:

```text
cursor.set/cursor.get/cursor.list/cursor.use/cursor.delete
```

Common payloads:
- `data.cursor`
- `data.cursors`
- `data.active_cursor`
- `data.resolved_time`

Extraction:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"cursor.list","target":{"session_id":1}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("data",{}).get("active_cursor"), [c.get("name") for c in d.get("data",{}).get("cursors",[])])'
```

AI usage:
- Create a cursor when an event time becomes important, then use `@name`, `@name-20ns`, `@name+5ns`, or `@name-10cycle(top.clk)` in later time fields.
- Use `cursor.use` before short active-cursor forms like `@-10ns`.
- Cycle offsets use real FSDB clock edges. `cycle(clk)` means posedge; use `posedge(clk)` or `negedge(clk)` when the edge matters.

### Value Actions

Actions:

```text
value.at/value.batch_at
```

Common payloads:
- `summary.time`
- `summary.signal_count`
- `summary.unknown_count`
- `data.value`
- `data.values`
- per-value fields: `signal`, `value`

Extraction example:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"value.batch_at","target":{"session_id":1},"args":{"time":"10ns","signals":["top.clk","top.rst_n"],"format":"hex"}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print([(v.get("signal"), v.get("value",{}).get("hex")) for v in d.get("data",{}).get("values",[])])'
```

### List Actions

Actions:

```text
list.create/list.add/list.delete/list.show/list.value_at/list.validate/list.diff
```

Common payloads:
- `summary.name`
- `summary.signal_count`
- `summary.valid`
- `summary.invalid_count`
- `summary.diff_count`
- `data.lists`
- `data.signals`
- `data.values`
- `data.invalid`
- `data.diffs`

AI usage:
- Use `list.validate` before trusting a saved list from a different FSDB.
- For `list.value_at`, missing signals may appear as `NOT_FOUND`; treat that as a non-passing diagnostic.

### Event Actions

Actions:

```text
event.config.load/event.config.list/event.find/event.export
```

Common payloads:
- `summary.name`
- `summary.expr`
- `summary.match_count`
- `summary.truncated`
- `data.events`
- event fields such as `idx`, `time`, `time_ps`, `values`, `context`

Context payloads, when requested, may include:
- `context.axi`
- `context.apb`
- `transactions`
- `window_ps`
- `delta_ps`
- `relation`

Extraction example:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"event.export","target":{"session_id":1},"args":{"name":"evt0","expr":"valid && !ready","time_range":{"begin":"0ns","end":"100us"}},"limits":{"max_rows":100}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print([e.get("time") for e in d.get("data",{}).get("events",[])])'
```

### Condition And Expression Actions

Actions:

```text
verify.conditions/expr.eval_at/window.verify
```

Common payloads:
- `summary.all_passed`
- `summary.passed`
- `summary.failed`
- `summary.unknown`
- `summary.expr_value`
- `summary.known`
- `summary.sample_count`
- `summary.failed_samples`
- `data.checks`
- `data.operands`
- `data.failures`

AI usage:
- `unknown` is a third state. Do not collapse it into false.
- Use `window.verify` for claims that must hold over a clocked range.

### Signal Inspection Actions

Actions:

```text
signal.changes/signal.stability/signal.trend/inspect_signal/detect_anomaly
```

Common payloads:
- `summary.transition_count`
- `summary.first_change`
- `summary.last_change`
- `summary.initial_value`
- `summary.final_value`
- `summary.stable`
- `summary.monotonic`
- `summary.min_value`
- `summary.max_value`
- `summary.finding_count`
- `summary.truncated`
- `data.changes`
- `findings`

AI usage:
- Use `limits.max_events` or `limits.max_findings` for large windows.
- Prefer `signal.stability` over `signal.changes` when only stability matters.

### Handshake Actions

Action:

```text
handshake.inspect
```

Common payloads:
- `summary.transfer_count`
- `summary.stall_count`
- `summary.max_stall_cycles`
- `summary.valid_without_ready_cycles`
- `summary.ready_without_valid_cycles`
- `summary.data_stability_violations`
- `findings`

AI usage:
- Use `findings` for long stalls or data stability violations.
- Use `suggested_next_actions` to bridge from waveform facts to RTL tracing.

### APB Actions

Actions:

```text
apb.config.load/apb.config.list/apb.query/apb.cursor/apb.transfer_window
```

Common payloads:
- `summary.name`
- `summary.transfer_count`
- `summary.truncated`
- `data.transactions`
- APB transaction fields such as `time`, `time_ps`, `type`, `addr`, `data`

AI usage:
- Use `apb.transfer_window` for local context around a debug time.
- Use numeric `time_ps` when joining APB facts with events.

### AXI Actions

Actions:

```text
axi.config.load/axi.config.list/axi.query/axi.cursor/axi.analysis/axi.channel_stall/axi.outstanding_timeline/axi.request_response_pair/axi.latency_outlier
```

Common payloads:
- `summary.name`
- `summary.direction`
- `summary.transaction_count`
- `summary.stall_count`
- `summary.max_stall_cycles`
- `summary.outstanding_max`
- `summary.outlier_count`
- `data.transactions`
- `data.timeline`
- `data.pairs`
- `data.outliers`
- `findings`

Latency fields are action-specific. Do not assume a fixed path such as `latency.max` or `latency.max_cycles`; inspect the real output first.

Extraction example:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"axi.latency_outlier","target":{"session_id":1},"args":{"name":"axi0","direction":"rd","threshold_cycles":100},"limits":{"max_rows":20}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("summary",{})); print(d.get("data",{}).keys() if isinstance(d.get("data"),dict) else None)'
```

## Defensive Extraction Checklist

- Check `ok` first.
- If `ok:false`, read `error.code`, `recoverable`, `candidates`, and `suggested_actions`.
- Use `.get()` for action-specific fields unless you just inspected the output.
- Use numeric `*_ps` fields for time math.
- Treat unknown value states as inconclusive.
- Honor `meta.truncated` and summary `truncated` fields before making exhaustive claims.
- Keep queries bounded with `limits` and perform custom aggregation in Python.
