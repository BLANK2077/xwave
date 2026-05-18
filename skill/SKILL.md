---
name: xwave
description: >
  Use when an AI agent needs structured JSON access to FSDB waveform facts through xwave,
  including sessions, scope discovery, values, signal lists, APB/AXI transactions,
  generic events, condition checks, expression/window verification, signal inspection,
  anomaly detection, handshake inspection, or protocol debug facts.
---

# xwave AI JSON Interface

This skill is for AI agents using `xwave ai ...` as a structured waveform fact API. Prefer the AI JSON entry point over the human CLI whenever you need machine-readable output, deterministic errors, or multi-step debug evidence.

Human-oriented legacy CLI details were moved to [references/cli-reference.md](references/cli-reference.md). Load that file only when the user explicitly asks about non-AI command syntax.

## Entry Point

Use one of these forms:

```bash
tools/xwave-env ai query request.json
tools/xwave-env ai query -
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"value.at","target":{"fsdb":"waves.fsdb","auto_open":true},"args":{"signal":"top.clk","time":"10ns"}}'
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"cursor.set","target":{"session_id":"case_a"},"args":{"name":"deadlock","time":"120340ns","note":"stall start"}}'
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"value.at","target":{"session_id":"case_a"},"args":{"signal":"top.ready","at":"@deadlock-20ns","format":"hex"}}'
tools/xwave-env ai schema
tools/xwave-env ai actions
```

## Output Verbosity

Important: `xwave ai query` defaults to compact output. Compact output deliberately omits `tool`, `session`, empty `warnings`, empty `suggested_next_actions`, and `meta.elapsed_ms`. Do not assume those fields exist unless you request them.

Use compact for normal AI workflows:

```json
{"output":{"verbosity":"compact"}}
```

Use full when maintaining older scripts or discovering exact response fields:

```json
{"output":{"verbosity":"full"}}
```

Use debug when diagnosing session/daemon/socket/FSDB fingerprint issues:

```json
{"output":{"verbosity":"debug"}}
```

Errors still include structured `error.code/message`, and non-empty recovery hints remain present in compact output.

For scripted extraction, pipe JSON output into `python3` instead of parsing human text. This is the recommended way for AI agents to pull specific fields or compute custom statistics:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"session.list"}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ok"], d.get("summary", {}))'
```

For larger summaries, keep the xwave query bounded and do the aggregation in Python:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"event.export","target":{"session_id":"case_a"},"args":{"name":"if0","expr":"valid && !ready","time_range":{"begin":"0ns","end":"100us"}},"limits":{"max_rows":1000}}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); rows=d.get("data",{}).get("events",[]); print(len(rows))'
```

Request envelope:

```json
{
  "api_version": "xwave.ai.v1",
  "request_id": "optional-id",
  "action": "value.at",
  "target": {
    "fsdb": "/path/to/waves.fsdb",
    "auto_open": true
  },
  "args": {},
  "limits": {
    "max_rows": 1000,
    "max_events": 1000,
    "max_samples": 1000000
  },
  "output": {
    "verbosity": "compact"
  }
}
```

Response envelope always contains:

```text
compact: ok/action plus non-empty summary/data/findings/error/suggested_next_actions/meta.truncated
full/debug: ok/action/session/summary/data/findings/suggested_next_actions/warnings/error/meta
```

Only `ok/action/error` and action-specific key data should be treated as always relevant. Field presence depends on `output.verbosity`; compact output will not include session/meta/tool scaffolding. Field names inside `summary`, `data`, and `findings` are action-specific and may differ by action. Do not guess detailed keys such as latency subfields from memory. For a field dictionary and extraction guidance, see [references/ai-response-dictionary.md](references/ai-response-dictionary.md). For exact fields on a specific build and FSDB, run `tools/xwave-env ai schema` when available, or issue a small bounded query and inspect the returned JSON before writing extraction code.

AI usage rules:
- Start with `session.open` for repeated work, then use `target.session_id` as a string.
- For one-shot queries, use `target.fsdb + auto_open:true`.
- Always inspect `ok` and `error.code`; do not parse human text.
- Prefer `python3 -c 'import json,sys; ...'` pipelines for extracting fields or computing statistics from `xwave ai query` output.
- Use `cursor.set` after finding an important event time, then use `@name`, `@name-20ns`, `@name+5ns`, `@name-10cycle(top.clk)`, or active cursor forms such as `@-10ns` in later time fields.
- Time fields accept absolute TimeSpec strings directly. Cycle offsets use real FSDB clock edges: `cycle(clk)` means posedge, and `posedge(clk)` / `negedge(clk)` choose the edge explicitly.
- For range actions, prefer `around/before/after` when investigating context around a cursor, for example `{"around":"@deadlock","before":"100cycle(top.clk)","after":"20cycle(top.clk)"}`.
- Treat `known:false`, `status:"unknown"`, and `pass:null` as inconclusive waveform facts, not failures.
- Use `scope.list` after `SIGNAL_NOT_FOUND`.
- Use `limits.max_rows/max_events/max_samples` for broad scans.
- Load APB/AXI/Event configs before protocol or event actions.
- Runtime state and persisted configs live under `~/.xwave/`: `registry.json` plus `sessions/<hashed-session-dir>/session.json`, `socket`, `debug.log`, `lists.json`, `apb.json`, `axi.json`, `events.json`, and `cursors.json`.

## Session Actions

### `session.open`

Open a named session for an FSDB. The session name is required, is the real session id, and is reused by every later `target.session_id` field. Names may be up to 256 characters and may contain letters, digits, `_`, `.`, and `-`; duplicate names fail.

```json
{"api_version":"xwave.ai.v1","action":"session.open","target":{"fsdb":"/path/to/waves.fsdb"},"args":{"name":"case_a"}}
```

### `session.list`

List known sessions. Use before reusing an existing session.

```json
{"api_version":"xwave.ai.v1","action":"session.list"}
```

### `session.doctor`

Check daemon, socket, PID, FSDB fingerprint, and health for a session.

```json
{"api_version":"xwave.ai.v1","action":"session.doctor","target":{"session_id":"case_a"}}
```

### `session.gc`

Clean stale or idle sessions.

```json
{"api_version":"xwave.ai.v1","action":"session.gc"}
```

### `session.kill`

Stop one session or all sessions.

```json
{"api_version":"xwave.ai.v1","action":"session.kill","args":{"id":"all"}}
```

## Cursor Actions

Cursor actions store named session-local times. Use them when a debug flow has a key event time and later queries need context before or after that point. All later time fields can use `@name`, `@name-20ns`, `@name+5ns`, `@name-10cycle(top.clk)`, `@-10ns`, or `@+5ns`.

### `cursor.set`

Create or replace a cursor. The response includes `data.resolved_time`, so use that as the canonical time for evidence.

```json
{"api_version":"xwave.ai.v1","action":"cursor.set","target":{"session_id":"case_a"},"args":{"name":"deadlock","time":"120340ns","note":"rready stall starts"}}
```

### `cursor.get`

Fetch one cursor.

```json
{"api_version":"xwave.ai.v1","action":"cursor.get","target":{"session_id":"case_a"},"args":{"name":"deadlock"}}
```

### `cursor.list`

List cursors and the active cursor.

```json
{"api_version":"xwave.ai.v1","action":"cursor.list","target":{"session_id":"case_a"}}
```

### `cursor.use`

Set the active cursor, enabling short forms such as `@-20ns`.

```json
{"api_version":"xwave.ai.v1","action":"cursor.use","target":{"session_id":"case_a"},"args":{"name":"deadlock"}}
```

### `cursor.delete`

Delete a cursor.

```json
{"api_version":"xwave.ai.v1","action":"cursor.delete","target":{"session_id":"case_a"},"args":{"name":"deadlock"}}
```

## Scope And Value Actions

### `scope.list`

List available FSDB signals under a scope. Use this to recover from missing or ambiguous paths.

```json
{"api_version":"xwave.ai.v1","action":"scope.list","target":{"session_id":"case_a"},"args":{"path":"top.u_dut","recursive":true},"limits":{"max_rows":200}}
```

### `value.at`

Read one signal at one time. Use for point evidence.

```json
{"api_version":"xwave.ai.v1","action":"value.at","target":{"session_id":"case_a"},"args":{"signal":"top.u_dut.ready","at":"@deadlock-20ns","format":"hex"}}
```

### `value.batch_at`

Read many signals at the same time. Prefer this over many `value.at` calls.

```json
{"api_version":"xwave.ai.v1","action":"value.batch_at","target":{"session_id":"case_a"},"args":{"at":"@deadlock","signals":["top.u_dut.valid","top.u_dut.ready","top.u_dut.fifo_full"],"format":"hex"}}
```

## Signal List Actions

### `list.create`

Create a named signal list.

```json
{"api_version":"xwave.ai.v1","action":"list.create","target":{"session_id":"case_a"},"args":{"name":"if0"}}
```

### `list.add`

Add a signal after probing that it exists.

```json
{"api_version":"xwave.ai.v1","action":"list.add","target":{"session_id":"case_a"},"args":{"name":"if0","signal":"top.u_dut.valid"}}
```

### `list.delete`

Remove by signal path or list index.

```json
{"api_version":"xwave.ai.v1","action":"list.delete","target":{"session_id":"case_a"},"args":{"name":"if0","index":"2"}}
```

### `list.show`

Show list members.

```json
{"api_version":"xwave.ai.v1","action":"list.show","target":{"session_id":"case_a"},"args":{"name":"if0"}}
```

### `list.value_at`

Read all list signals at one time.

```json
{"api_version":"xwave.ai.v1","action":"list.value_at","target":{"session_id":"case_a"},"args":{"name":"if0","time":"42us","format":"binary"}}
```

### `list.validate`

Check whether every signal in a list exists in the current FSDB.

```json
{"api_version":"xwave.ai.v1","action":"list.validate","target":{"session_id":"case_a"},"args":{"name":"if0"}}
```

### `list.diff`

Find the earliest time where list values are not all equal.

```json
{"api_version":"xwave.ai.v1","action":"list.diff","target":{"session_id":"case_a"},"args":{"name":"if0","begin":"0ns","end":"100us"}}
```

## APB Actions

### `apb.config.load`

Load and persist an APB config. `config_path` points to JSON, or `config` may inline the object.

```json
{"api_version":"xwave.ai.v1","action":"apb.config.load","target":{"session_id":"case_a"},"args":{"name":"apb0","config_path":"apb.json"}}
```

### `apb.config.list`

Return the saved APB config.

```json
{"api_version":"xwave.ai.v1","action":"apb.config.list","target":{"session_id":"case_a"},"args":{"name":"apb0"}}
```

### `apb.query`

Query APB reads or writes. Use `direction:"wr"` or `direction:"rd"`, with optional `address`, `num`, or `last`.

```json
{"api_version":"xwave.ai.v1","action":"apb.query","target":{"session_id":"case_a"},"args":{"name":"apb0","direction":"wr","address":"0x100","num":1}}
```

### `apb.cursor`

Iterate APB transactions with `op:"begin"`, `next`, `pre`, or `last`.

```json
{"api_version":"xwave.ai.v1","action":"apb.cursor","target":{"session_id":"case_a"},"args":{"name":"apb0","op":"begin","direction":"all"}}
```

### `apb.transfer_window`

Return APB transactions whose transfer time is inside a time range.

```json
{"api_version":"xwave.ai.v1","action":"apb.transfer_window","target":{"session_id":"case_a"},"args":{"name":"apb0","time_range":{"begin":"40us","end":"45us"},"direction":"all","limit":50}}
```

## AXI Actions

### `axi.config.load`

Load and persist an AXI config. Use a full five-channel config with `clk`, `rst_n`, and optional `edge`.

```json
{"api_version":"xwave.ai.v1","action":"axi.config.load","target":{"session_id":"case_a"},"args":{"name":"axi0","config_path":"axi.json"}}
```

### `axi.config.list`

Return the saved AXI config.

```json
{"api_version":"xwave.ai.v1","action":"axi.config.list","target":{"session_id":"case_a"},"args":{"name":"axi0"}}
```

### `axi.query`

Query AXI reads or writes. Use optional `address`, `id`, `num`, or `last`.

```json
{"api_version":"xwave.ai.v1","action":"axi.query","target":{"session_id":"case_a"},"args":{"name":"axi0","direction":"rd","id":"0x3","num":1}}
```

### `axi.cursor`

Iterate AXI transactions with `op:"begin"`, `next`, `pre`, or `last`.

```json
{"api_version":"xwave.ai.v1","action":"axi.cursor","target":{"session_id":"case_a"},"args":{"name":"axi0","op":"next","direction":"rd"}}
```

### `axi.analysis`

Return AXI latency or outstanding statistics. Use `analysis:"latency"` or `analysis:"osd"` and optional `direction` or `id`.

```json
{"api_version":"xwave.ai.v1","action":"axi.analysis","target":{"session_id":"case_a"},"args":{"name":"axi0","analysis":"latency","direction":"all"}}
```

### `axi.channel_stall`

Inspect one AXI channel valid/ready stall behavior. Channels: `aw`, `w`, `b`, `ar`, `r`.

```json
{"api_version":"xwave.ai.v1","action":"axi.channel_stall","target":{"session_id":"case_a"},"args":{"name":"axi0","channel":"r","time_range":{"begin":"40us","end":"45us"},"rules":{"max_wait_cycles":16}}}
```

### `axi.outstanding_timeline`

Return outstanding samples over a window. Use for buildup/root-cause evidence.

```json
{"api_version":"xwave.ai.v1","action":"axi.outstanding_timeline","target":{"session_id":"case_a"},"args":{"name":"axi0","time_range":{"begin":"40us","end":"45us"},"direction":"all","limit":100}}
```

### `axi.request_response_pair`

Return AXI transactions whose request or response time is inside a window.

```json
{"api_version":"xwave.ai.v1","action":"axi.request_response_pair","target":{"session_id":"case_a"},"args":{"name":"axi0","time_range":{"begin":"40us","end":"45us"},"direction":"rd","limit":20}}
```

### `axi.latency_outlier`

Return transactions with largest request-response latency.

```json
{"api_version":"xwave.ai.v1","action":"axi.latency_outlier","target":{"session_id":"case_a"},"args":{"name":"axi0","time_range":{"begin":"0ns","end":"100us"},"direction":"all","top_n":5,"limit":500}}
```

## Event Actions

### `event.config.load`

Load and persist a generic event config bound to the current FSDB.

```json
{"api_version":"xwave.ai.v1","action":"event.config.load","target":{"session_id":"case_a"},"args":{"name":"if0","config_path":"if0.event.json"}}
```

### `event.config.list`

List event configs or return one config.

```json
{"api_version":"xwave.ai.v1","action":"event.config.list","target":{"session_id":"case_a"},"args":{"name":"if0"}}
```

### `event.find`

Find the first clock-sampled event matching an expression.

```json
{"api_version":"xwave.ai.v1","action":"event.find","target":{"session_id":"case_a"},"args":{"name":"if0","expr":"valid && !ready","time_range":{"begin":"40us","end":"45us"}}}
```

With protocol context:

```json
{"api_version":"xwave.ai.v1","action":"event.find","target":{"session_id":"case_a"},"args":{"name":"if0","expr":"valid && !ready","time_range":{"begin":"40us","end":"45us"},"context":{"window":"200ns","axi":"axi0","apb":"apb0"}}}
```

### `event.export`

Export matching events. Always set a limit unless you intentionally need a large scan.

```json
{"api_version":"xwave.ai.v1","action":"event.export","target":{"session_id":"case_a"},"args":{"name":"if0","expr":"valid && !ready","time_range":{"begin":"40us","end":"45us"},"limit":100}}
```

## Wave Fact Verification Actions

### `verify.conditions`

Check point-time conditions. Use this to verify hypotheses generated from RTL or protocol reasoning.

```json
{"api_version":"xwave.ai.v1","action":"verify.conditions","target":{"session_id":"case_a"},"args":{"time":"42us","conditions":[{"signal":"top.u_dut.fifo_full","op":"==","value":"1"},{"signal":"top.u_dut.state","op":"!=","value":"0"}]}}
```

### `expr.eval_at`

Evaluate a Boolean expression at one time using aliases.

```json
{"api_version":"xwave.ai.v1","action":"expr.eval_at","target":{"session_id":"case_a"},"args":{"time":"42us","signals":{"valid":"top.u_dut.valid","ready":"top.u_dut.ready"},"expr":"valid && !ready"}}
```

### `window.verify`

Verify expressions sampled on clock edges over a window. `mode` may be `always`, `never`, or `eventually`.

```json
{"api_version":"xwave.ai.v1","action":"window.verify","target":{"session_id":"case_a"},"args":{"clock":"top.clk","sampling":"posedge","time_range":{"begin":"42us","end":"44us"},"conditions":[{"expr":"valid && !ready","signals":{"valid":"top.u_dut.valid","ready":"top.u_dut.ready"},"mode":"always"}]}}
```

### `signal.changes`

Return value changes for a signal in a range.

```json
{"api_version":"xwave.ai.v1","action":"signal.changes","target":{"session_id":"case_a"},"args":{"signal":"top.u_dut.fifo_full","time_range":{"begin":"40us","end":"45us"},"limit":20,"format":"binary"}}
```

### `signal.stability`

Check whether a signal remains stable in a range.

```json
{"api_version":"xwave.ai.v1","action":"signal.stability","target":{"session_id":"case_a"},"args":{"signal":"top.u_dut.active_count","time_range":{"begin":"42us","end":"44us"}}}
```

### `signal.trend`

Sample a numeric signal on clock edges and summarize monotonicity, min, max, and stable state.

```json
{"api_version":"xwave.ai.v1","action":"signal.trend","target":{"session_id":"case_a"},"args":{"signal":"top.u_dut.active_count","clock":"top.clk","sampling":"posedge","time_range":{"begin":"40us","end":"45us"},"max_samples":10000}}
```

### `inspect_signal`

Summarize signal transitions, period-like intervals, and glitches.

```json
{"api_version":"xwave.ai.v1","action":"inspect_signal","target":{"session_id":"case_a"},"args":{"signal":"top.clk","time_range":{"begin":"0ns","end":"1us"},"glitch_threshold":"1ns","limit":1000}}
```

### `detect_anomaly`

Find waveform anomalies such as glitches, stuck values, and x/z.

```json
{"api_version":"xwave.ai.v1","action":"detect_anomaly","target":{"session_id":"case_a"},"args":{"signals":["top.u_dut.ready","top.u_dut.valid","top.u_dut.data"],"time_range":{"begin":"0ns","end":"100us"},"checks":[{"type":"glitch","min_pulse_width":"1ns"},{"type":"stuck","min_duration":"1us"},{"type":"unknown_xz"}],"max_findings":50}}
```

### `handshake.inspect`

Analyze a generic valid/ready interface sampled on a clock.

```json
{"api_version":"xwave.ai.v1","action":"handshake.inspect","target":{"session_id":"case_a"},"args":{"clock":"top.clk","valid":"top.u_dut.valid","ready":"top.u_dut.ready","data":["top.u_dut.data"],"time_range":{"begin":"40us","end":"45us"},"rules":{"max_wait_cycles":100,"check_data_stable_when_stalled":true}}}
```

## Recommended AI Workflows

### Point fact verification

1. `session.open`
2. `value.batch_at`
3. If missing signal: `scope.list`
4. Store `data.values[]` as evidence.

### Valid/ready stall debug

1. `handshake.inspect` for generic interfaces, or `axi.channel_stall` for AXI channels.
2. Use `value.batch_at` at stall begin/mid/end for related control signals.
3. Use `window.verify` to prove the blocking condition persists.
4. Use `event.find` or `event.export` when you need exact event samples.

### Protocol context around an event

1. `axi.config.load` and/or `apb.config.load`
2. `event.config.load`
3. `event.find` with `context.window` plus `context.axi` and/or `context.apb`
4. Use returned transactions and `delta_ps` as evidence.

### Performance-safe scanning

Use bounded ranges and limits:

```json
{"limits":{"max_rows":100,"max_events":100,"max_samples":100000}}
```

Prefer:
- `value.batch_at` over repeated `value.at`
- `event.find` before `event.export`
- `axi.channel_stall` before full AXI transaction inspection
- `signal.stability` before full `signal.changes`

## Error Handling

Common `error.code` values:

| Code | AI response |
|---|---|
| `SIGNAL_NOT_FOUND` | Run `scope.list` near the parent scope and retry with a candidate path. |
| `SESSION_NOT_FOUND` / `SESSION_UNHEALTHY` | Run `session.list`, `session.doctor`, or reopen the FSDB. |
| `INVALID_REQUEST` / `MISSING_FIELD` | Fix the JSON request shape before retrying. |
| `EXPR_PARSE_FAILED` | Simplify or validate aliases in the expression. |
| `WAVE_QUERY_FAILED` | Check FSDB, config binding, and time range. |

If `ok:false`, do not use `data` as evidence. If `ok:true` but a value has `known:false`, preserve it as unknown evidence rather than converting it to pass/fail.
