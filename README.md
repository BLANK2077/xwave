English | [中文](README.zh.md)

# xwave

xwave is a Synopsys NPI based FSDB waveform query tool. It lets you query waveform facts from the command line without launching the Verdi GUI: signal values, scope discovery, signal lists, APB/AXI transactions, generic valid/ready events, and AI-friendly JSON facts.

The same `xwave` binary works as both the CLI client and the background daemon. The client starts one local daemon per FSDB session and talks to it through a Unix domain socket.

## Quick Start

Use the wrapper unless you have already set all Verdi/NPI library paths yourself:

```bash
tools/xwave-env <command> ...
```

Open an FSDB:

```bash
tools/xwave-env open /path/to/waves.fsdb --name case_a
```

Example output:

```text
[Session case_a] Ready (FSDB: 0 ~ 200000)
[Session case_a] FSDB opened: /path/to/waves.fsdb
```

Query values:

```bash
tools/xwave-env value top.clk 10ns       # hex by default
tools/xwave-env value top.clk 10ns -b    # binary
tools/xwave-env value top.count 10ns -d  # decimal
```

Discover dumped signals:

```bash
tools/xwave-env scope top.u_dut -recursive -json
```

Manage a small signal list:

```bash
tools/xwave-env list new if0
tools/xwave-env list add top.u_dut.valid -l if0
tools/xwave-env list add top.u_dut.ready -l if0
tools/xwave-env list value 42us -l if0 -json
tools/xwave-env list diff -l if0 -b 0ns -e 100us
```

Clean up when done:

```bash
tools/xwave-env session kill case_a
```

## Core Concepts

### Sessions

`open` requires an explicit session name:

```bash
tools/xwave-env open /path/to/waves.fsdb --name case_a
```

The name is the session ID used by `-s <name>` and AI `target.session_id`. It may be up to 256 characters and may contain letters, digits, `_`, `.`, and `-`. Creating a session with an existing name fails with `SESSION_ID_EXISTS`; use a different name or kill the old session first.

`open` canonicalizes the FSDB path and creates a named daemon session. Each session owns:

- one FSDB file and one NPI context
- one daemon process
- one Unix domain socket under `~/.xwave/sessions/<hashed-session-dir>/socket`
- one registry record in `~/.xwave/registry.json`

Sessions record the FSDB mtime, size, device, and inode. If the FSDB is replaced or updated, the next access reports the change and restarts the daemon while preserving the session ID and loaded configs.

Sessions also have an idle timeout. The default is 1800 seconds and can be overridden with `XWAVE_IDLE_TIMEOUT_SEC`. For long interactive debug runs, use a larger value, for example:

```bash
export XWAVE_IDLE_TIMEOUT_SEC=28800
```

After an idle timeout, the daemon exits and releases the FSDB/NPI handle. Running `open <fsdb> --name <new-name>` creates a fresh session. If it does not, use `--debug` to see the exact failed stage.

### TimeSpec And Cursors

Commands that need a time accept a `TimeSpec`. A TimeSpec can be an absolute time, a saved cursor, a cursor plus/minus a duration, or a cursor plus/minus clock cycles:

```text
100ns
@deadlock
@deadlock-20ns
@deadlock+5ns
@-10ns
@+5ns
@deadlock-10cycle(top.clk)
@deadlock+5posedge(top.clk)
@deadlock-2negedge(top.clk)
```

Absolute times accept `us`, `ns`, `ps`, and `fs`. If no suffix is provided, the default is `ns`. `@` means the active cursor. `cycle(clk)` uses posedges by default; use `posedge(clk)` or `negedge(clk)` to choose the edge explicitly.

AI JSON range actions also accept `around`, `before`, and `after`. Explicit `time_range.begin/end` still takes priority:

```json
{"around":"@deadlock","before":"100ns","after":"20cycle(top.clk)"}
```

Time conversion happens inside the daemon after the FSDB is opened. The daemon uses the NPI time conversion API for the current FSDB time scale, so do not assume the internal FSDB unit is always `ps`.

Examples:

```bash
tools/xwave-env value top.clk 10ns
tools/xwave-env cursor set deadlock 120340ns -note rready_stall_start
tools/xwave-env value top.rready --at @deadlock-20ns
tools/xwave-env value top.rready --at @deadlock-10cycle(top.clk)
tools/xwave-env list diff -l if0 -b 5ns -e 50ns
tools/xwave-env event find -n if0 -expr "valid && ready" -b 100us
```

Invalid units such as `10abc` and negative times such as `-1ns` are rejected.

### LSF / bsub

xwave sessions are local to one machine. The daemon is reached through a Unix domain socket, and health checks use local PID and `/proc` state. In LSF environments, `open` and all later commands must run on the same host.

Recommended setup: ask IT for a dedicated queue containing exactly one suitable machine for xwave/xtrace style NPI tools.

```bash
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env open /path/to/waves.fsdb --name case_a"
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env value top.clk 10ns -s 1"
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env session kill case_a"
```

The dedicated host must have access to the shared FSDB path and the same Verdi/NPI/license environment. If a single-host queue is not available, the next simplest option is `bsub -m <host>`. If fixed-host usage is still not acceptable, xwave needs architecture work such as a TCP daemon, remote forwarding, or a no-daemon single-command mode.

## Main Workflows

### Signal Lists

```bash
tools/xwave-env list new my_signals
tools/xwave-env list add test_top.clk -l my_signals
tools/xwave-env list add test_top.rst_n -l my_signals
tools/xwave-env list show -l my_signals
tools/xwave-env list value 15ns -l my_signals -d -json
tools/xwave-env list validate -l my_signals
tools/xwave-env list diff -l my_signals -b 5ns -e 50ns
```

`list add` probes that the signal exists before saving it. `list value` prints `NOT_FOUND` and returns non-zero if an old list contains a missing signal. `list diff` requires at least two signals.

### APB Transactions

APB config:

```json
{
  "paddr": "test_top.apb.paddr",
  "pwdata": "test_top.apb.pwdata",
  "prdata": "test_top.apb.prdata",
  "pwrite": "test_top.apb.pwrite",
  "penable": "test_top.apb.penable",
  "psel": "test_top.apb.psel",
  "clk": "test_top.clk",
  "rst_n": "test_top.rst_n",
  "edge": "posedge"
}
```

Commands:

```bash
tools/xwave-env apb apb.json -n apb0
tools/xwave-env apb wr -n apb0
tools/xwave-env apb rd -n apb0
tools/xwave-env apb wr -n apb0 -addr 0x100 -num 3 -json
tools/xwave-env apb begin -n apb0 -wr -json
tools/xwave-env apb next -n apb0 -wr -json
```

### AXI Transactions

After loading an AXI JSON config, xwave can query read/write transactions, filter by address or ID, and analyze latency/outstanding behavior.

```bash
tools/xwave-env axi axi_cfg.json -n axi0
tools/xwave-env axi wr -n axi0
tools/xwave-env axi rd -n axi0 -id 0x3
tools/xwave-env axi latency -n axi0 -rd -json
tools/xwave-env axi osd -n axi0 -wr -json
```

### Generic Events

Generic events are for valid/ready/backpressure style interfaces. xwave samples aliases on clock edges and evaluates a Boolean expression.

Event config:

```json
{
  "clk": "xif_tb_top.clk",
  "rst_n": "xif_tb_top.rst_n",
  "edge": "posedge",
  "signals": {
    "valid": "xif_tb_top.if0.valid",
    "ready": "xif_tb_top.if0.ready",
    "bp": "xif_tb_top.if0.bp",
    "payload": "xif_tb_top.if0.payload"
  },
  "fields": {
    "opcode": "payload[23:16]",
    "data": "payload[15:0]"
  }
}
```

Commands:

```bash
tools/xwave-env event if0.event.json -n if0
tools/xwave-env event export -n if0 -expr "valid && ready" -json
tools/xwave-env event export -n if0 -expr "valid && opcode == 0x10" -json
tools/xwave-env event find -n if0 -expr "valid && ready && data != 0" -json
tools/xwave-env event find -n if0 -expr "valid && !bp" -context 200ns -axi axi0 -apb apb0 -json
```

Rules:

- `edge` must be `posedge` or `negedge`; omitted `edge` defaults to `posedge`.
- `fields` ranges must be valid non-negative integers and must reference aliases in `signals`.
- Expressions are parsed and alias-checked before waveform scanning.
- Boolean values or comparisons containing `x/z` evaluate to unknown and do not match events.
- `event export` defaults to 1000 rows unless `-limit` is specified.
- `-context <T>` can be combined with `-axi <name>`, `-apb <name>`, or both.

## AI JSON API

`xwave ai` is the stable JSON entry point for AI agents and scripts. Human-oriented CLI commands remain unchanged.

```bash
tools/xwave-env ai query request.json
tools/xwave-env ai query -
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"value.at","target":{"fsdb":"waves.fsdb","auto_open":true},"args":{"signal":"top.clk","time":"10ns"}}'
tools/xwave-env ai schema
tools/xwave-env ai actions
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

Important: AI JSON responses are compact by default. Compact output intentionally omits `tool`, `session`, empty `warnings`, empty `suggested_next_actions`, and `meta.elapsed_ms`. Use `output.verbosity` when you need more:

```json
{"output":{"verbosity":"compact"}}
{"output":{"verbosity":"full"}}
{"output":{"verbosity":"debug"}}
```

`compact` is best for normal AI workflows. `full` returns the complete compatibility envelope. `debug` keeps session/socket/PID/fingerprint details for daemon or environment diagnosis. Error responses always keep structured `error.code/message`, and non-empty recovery suggestions are preserved.

For scripts, parse JSON instead of human text:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"session.list"}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ok"], d.get("summary", {}))'
```

AI value objects are intentionally small:

```json
{"value": "'h12", "known": true}
```

Use the request `format` field (`hex`, `binary`, `decimal`, or `auto`) to choose the value string representation. Values containing `x/z` return `known:false`.

For event counts, prefer built-in aggregation instead of exporting every row and counting in Python:

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"event.export","target":{"session_id":"case_a"},"args":{"name":"if0","expr":"valid && ready","time_range":{"begin":"0ns","end":"100us"},"aggregate":{"count":true,"group_by":["qid"],"events":false}}}'
```

Current AI actions cover `session/cursor/scope/value/list/apb/axi/event`, condition and expression checks, window verification, signal statistics, signal inspection, anomaly detection, handshake inspection, and protocol debug facts.

## Session Debugging

Use `--debug` or `XWAVE_DEBUG=1` when session creation or reconnect fails.

```bash
tools/xwave-env open /path/to/waves.fsdb --name case_a --debug
tools/xwave-env session doctor -s 1 --debug
tools/xwave-env session gc --debug
```

Debug output goes to stderr. Server startup details are written to:

```text
~/.xwave/sessions/<hashed-session-dir>/debug.log
```

The trace identifies stages such as FSDB stat, registry lock, fork/exec, `npi_init`, `npi_fsdb_open`, socket bind/listen, connect, PING, idle timeout, and cleanup reason.

Common causes of `Failed to create session`:

- inaccessible FSDB path
- missing Verdi/NPI runtime or license
- `npi_fsdb_open` failure
- large FSDB taking longer than `XWAVE_SESSION_START_TIMEOUT_SEC` (default 60 seconds)
- `$HOME` cannot host registry/socket files
- LSF dispatching later commands to a different host

## Command Reference

| Command | Description |
|---|---|
| `xwave open <fsdb-file> --name <name> [--debug]` | Open an FSDB and create a named session |
| `xwave session list` | List active sessions |
| `xwave session doctor -s <sid> [-json] [--debug]` | Diagnose a session |
| `xwave session gc [--debug]` | Clean stale/idle sessions |
| `xwave session kill <id\|all> [--debug]` | Kill one or all sessions |
| `xwave cursor set|get|list|delete|use ...` | Manage session time cursors |
| `xwave scope <path> [-recursive] [-json] [-s <sid>]` | List signals under a scope |
| `xwave value <sig> <time_spec>\|--at <time_spec> [-b\|-d] [-s <sid>]` | Query one signal value |
| `xwave list new <name> [-s <sid>]` | Create a signal list |
| `xwave list add <sig> [-s <sid>] [-l <name>]` | Add a signal |
| `xwave list del <sig\|idx> [-s <sid>] [-l <name>]` | Delete by signal path or index |
| `xwave list show [-s <sid>] [-l <name>]` | Show list contents |
| `xwave list value <time_spec>\|--at <time_spec> [-l <name>] [-b\|-d] [-json] [-s <sid>]` | Batch-query list values |
| `xwave list validate [-l <name>] [-json] [-s <sid>]` | Validate list signals |
| `xwave list diff [-l <name>] [-b T] [-e T] [-s <sid>]` | Find the earliest difference time |
| `xwave apb <json> -n <name> [-s <sid>]` | Load an APB config |
| `xwave apb list [-n <name>] [-s <sid>]` | Show APB config |
| `xwave apb wr\|rd [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]` | Query APB reads/writes |
| `xwave apb begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | APB cursor navigation |
| `xwave axi <json> -n <name> [-s <sid>]` | Load an AXI config |
| `xwave axi list [-n <name>] [-s <sid>]` | Show AXI config |
| `xwave axi wr\|rd [-n <name>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]` | Query AXI reads/writes |
| `xwave axi begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | AXI cursor navigation |
| `xwave axi latency\|osd [-rd\|-wr\|-all] [-id <id>] [-json] [-n <name>] [-s <sid>]` | AXI latency/outstanding analysis |
| `xwave event <json> -n <name> [-s <sid>]` | Load an event config |
| `xwave event list [-n <name>] [-s <sid>]` | Show event configs |
| `xwave event find -n <name> -expr <expr> [-b T] [-e T] [-context T [-axi <axi>] [-apb <apb>]] [-json] [-s <sid>]` | Find the first matching event |
| `xwave event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-context T [-axi <axi>] [-apb <apb>]] [-json] [-s <sid>]` | Export matching events |
| `xwave ai query <json\|-\|--json JSON>` | Run an AI JSON request |
| `xwave ai schema` | Print the `xwave.ai.v1` request schema |
| `xwave ai actions` | Print AI API actions |

Use `xwave help <topic>` for detailed second-level help, for example:

```bash
tools/xwave-env help event
tools/xwave-env help session
tools/xwave-env help ai
```

## Build And Environment

Requirements:

- Linux 64-bit
- GCC with C++11 support
- Synopsys Verdi with `VERDI_HOME` set
- libstdc++ ABI compatible with the selected Verdi/NPI libraries

Verified versions:

- Verdi: `V-2023.12-SP1-1` / `V-2023.12-SP2`
- VCS: `V-2023.12-SP2_Full64`
- G++: GCC `8.5.0`

Build:

```bash
make clean && make
```

Notes:

- xwave links Synopsys NPI L1 FSDB C++ APIs such as `npi_fsdb_sig_value_at(..., std::string&, ...)`.
- Verdi 2023 NPI libraries export new-ABI symbols such as `std::__cxx11::basic_string`; use GCC 5+.
- Do not use `-D_GLIBCXX_USE_CXX11_ABI=0` with Verdi 2023 NPI libraries.
- Building test waveforms with VCS on this machine requires `VCS_TARGET_ARCH=linux64`.

## Architecture And Files

```text
┌─────────────┐      Unix Domain Socket       ┌─────────────────┐
│   xwave     │  <=========================>  │  xwave --server │
│  (client)   │      text protocol            │   (daemon)      │
└─────────────┘                               └─────────────────┘
                                                    │
                                                    │ NPI FSDB API
                                                    ▼
                                                 *.fsdb
```

Persistent files:

- `~/.xwave/registry.json`: active session records
- `~/.xwave/registry.lock`: registry lock
- `~/.xwave/sessions/<hashed-session-dir>/session.json`: per-session metadata
- `~/.xwave/sessions/<hashed-session-dir>/socket`: session socket
- `~/.xwave/sessions/<hashed-session-dir>/debug.log`: server debug log
- `~/.xwave/sessions/<hashed-session-dir>/lists.json`: signal lists
- `~/.xwave/sessions/<hashed-session-dir>/apb.json`: APB configs
- `~/.xwave/sessions/<hashed-session-dir>/axi.json`: AXI configs
- `~/.xwave/sessions/<hashed-session-dir>/events.json`: event configs
- `~/.xwave/sessions/<hashed-session-dir>/cursors.json`: session time cursors

New versions use only the `~/.xwave/` JSON directory layout; older top-level maintenance files are not part of the supported format.

Source layout:

```text
src/
├── main.cpp
├── commands/     # CLI command handlers
├── session/      # session registry and lifecycle
├── client/       # Unix socket client
├── server/       # daemon loop and command dispatch
├── list/         # persistent signal lists
├── apb/          # APB config and analyzer
├── axi/          # AXI config and analyzer
├── event/        # generic event config and analyzer
└── common/       # shared helpers such as time parsing
```
