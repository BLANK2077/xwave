English | [中文](README.zh.md)

# xwave

xwave is a Synopsys NPI based FSDB waveform query CLI. It lets you query signal values, manage signal lists, locate waveform differences, analyze APB/AXI transactions, and query valid/ready/backpressure style events from the command line without starting the Verdi GUI.

The same `xwave` binary works as both the CLI client and the background daemon server. Client and server communicate through Unix domain sockets.

---

## Features

- **Single binary, two modes**: the same executable works as the CLI client and as the daemon server started through `--server`.
- **Multi-session management**: each session loads one FSDB file with an independent NPI context and Unix domain socket.
- **FSDB change recovery**: sessions record an FSDB fingerprint. If the FSDB is replaced or updated, the next access automatically restarts the daemon.
- **Single signal value query**: `xwave value <signal> <time>` supports hexadecimal by default, plus binary and decimal output.
- **Signal lists**: create, delete, inspect, and batch-query signal lists, with JSON output support.
- **Scope signal discovery**: `xwave scope <path>` lists FSDB signal names under a given hierarchy.
- **Waveform difference search**: `xwave list diff` finds the earliest time when list signals are not all equal.
- **APB analysis**: load an APB JSON config, count reads/writes, query by address/index/latest transaction, and iterate with cursors.
- **AXI analysis**: load an AXI JSON config, query read/write transactions by address or ID, and analyze latency/outstanding behavior.
- **Generic event query**: load an event JSON config and sample valid/ready/backpressure style interfaces on clock edges with expression filters.
- **Time suffixes**: all time inputs default to `ns`; `us`, `ns`, `ps`, and `fs` suffixes are supported.

---

## Architecture

```text
┌─────────────┐      Unix Domain Socket       ┌─────────────────┐
│   xwave     │  <=========================>  │  xwave --server │
│  (client)   │      TEXT protocol            │   (daemon)      │
└─────────────┘                               └─────────────────┘
                                                    │
                                                    │ NPI FSDB API
                                                    ▼
                                               ┌─────────┐
                                               │ *.fsdb  │
                                               └─────────┘
```

- **Client**: parses CLI arguments, manages the session registry, and persists local List/APB/AXI/Event configurations.
- **Server**: forks one daemon per session, loads FSDB, and reads signal values through NPI.
- **Protocol**: lightweight text protocol over Unix domain sockets.
- **Registry**:
  - `~/.xwave.registry`: persistent session records.
  - `~/.xwave.lists`: persistent signal lists for sessions.

---

## Time Argument Handling

xwave accepts `us`, `ns`, `ps`, and `fs` suffixes for time arguments. If no suffix is provided, the default unit is `ns`.

Time conversion is performed inside the daemon after the FSDB is opened. The daemon calls the NPI time conversion API for the current FSDB time scale, so users should not assume that the FSDB internal time unit is always `ps`.

Examples:

```bash
tools/xwave-env value top.clk 10ns
tools/xwave-env list diff -l my_signals -b 5ns -e 50ns
tools/xwave-env event find -n if0 -expr "vld && rdy" -b 100us
```

Invalid units such as `10abc` and negative times such as `-1ns` are rejected.

---

## LSF / bsub Usage

xwave uses a local daemon per session. The daemon is reached through a Unix domain socket, and session health checks use local PID and `/proc` state. Because of this, `open` and later commands such as `value`, `list`, `event`, `apb`, `axi`, and `session kill` must run on the same machine.

In chip-company LSF environments, avoid submitting xwave commands to a normal queue that may dispatch each command to a different host. The recommended setup is to ask IT to create a dedicated queue that contains exactly one suitable machine for xwave/xtrace-style NPI tools. Then submit all xwave commands to that queue:

```bash
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env open /path/to/waves.fsdb"
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env value top.clk 10ns -s 1"
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env session kill 1"
```

The dedicated machine should have access to the shared FSDB path and a consistent Verdi/NPI/license environment. If a dedicated single-host queue is not available, the next simplest option is to use `bsub -m <host>` to pin all commands to one host.

If fixed-machine operation is still not acceptable, the project architecture needs additional work, such as a TCP daemon, automatic remote command forwarding, or a no-daemon single-command mode. Those options are more complex and should be treated as code changes rather than normal usage.

---

## Environment Requirements

- Linux 64-bit.
- GCC with C++11 support and a libstdc++ ABI compatible with the selected Verdi/NPI libraries.
- Synopsys Verdi with `VERDI_HOME` set correctly.

### Tested Tool Versions

- Verdi: `V-2023.12-SP1-1` / `V-2023.12-SP2`
- VCS: `V-2023.12-SP2_Full64`
- G++: GCC `8.5.0`

Notes:

- xwave links Synopsys NPI L1 FSDB C++ APIs such as `npi_fsdb_sig_value_at(..., std::string&, ...)`; the symbol must use the same C++ ABI as `libnpiL1.so`.
- Verdi 2020 NPI libraries can be built directly with GCC 4.8.
- Verdi 2023 NPI libraries export new-ABI symbols such as `std::__cxx11::basic_string`, so GCC 5+ is required.
- If linking fails with `undefined reference to npi_fsdb_sig_value_at(... std::string& ...)`, first check whether `libnpiL1.so` exports a symbol containing `std::__cxx11::basic_string`. If it does, the g++ used to build xwave must also generate new-ABI symbols.
- In Verdi 2023 environments, use GCC 5+; GCC `8.5.0` has been verified. Do not use `-D_GLIBCXX_USE_CXX11_ABI=0`. If the default g++ is too old, explicitly adding `-D_GLIBCXX_USE_CXX11_ABI=1` may still generate old-ABI symbols.
- Building test waveforms with VCS on this machine requires `VCS_TARGET_ARCH=linux64`.

Prefer the `tools/xwave-env` wrapper over direct `xwave` invocation. It sets `LD_LIBRARY_PATH` automatically:

```bash
tools/xwave-env <subcommand> ...
```

---

## Build

```bash
make clean && make
```

After a successful build, the `xwave` executable is generated in the repository root.

---

## Quick Start

### 1. Open an FSDB

```bash
tools/xwave-env open /path/to/your.fsdb
```

Example output:

```text
[Session 1] Ready (FSDB: 0 ~ 200000)
[Session 1] FSDB opened: /path/to/your.fsdb
```

### 2. Query a Single Signal Value

```bash
# Hexadecimal by default.
tools/xwave-env value test_top.clk 10ns

# Binary.
tools/xwave-env value test_top.clk 10ns -b

# Decimal.
tools/xwave-env value test_top.clk 10ns -d
```

### 3. Signal List Management

```bash
# Create a list.
tools/xwave-env list new my_signals

# Add signals.
tools/xwave-env list add test_top.clk -l my_signals
tools/xwave-env list add test_top.rst_n -l my_signals

# Show a list.
tools/xwave-env list show -l my_signals

# Batch query.
tools/xwave-env list value 15ns -l my_signals
tools/xwave-env list value 15ns -l my_signals -d -json

# Validate signals in the list.
tools/xwave-env list validate -l my_signals
```

`list add` validates signal existence before insertion. `list value` prints `NOT_FOUND` and returns non-zero for invalid signals in old lists. If `-l <name>` is omitted, xwave uses the most recently modified list for the current session.

### 4. Waveform Difference Search

```bash
# Search from FSDB start to end.
tools/xwave-env list diff -l my_signals

# Search a time range.
tools/xwave-env list diff -l my_signals -b 5ns -e 50ns
```

`list diff` requires at least two signals.

### 5. APB Analysis

Prepare an `apb.json` config:

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

```bash
# Load config.
tools/xwave-env apb apb.json -n my_apb

# Count reads/writes.
tools/xwave-env apb wr -n my_apb
tools/xwave-env apb rd -n my_apb

# Filter by address.
tools/xwave-env apb wr -n my_apb -addr 0x100 -num 3 -json

# Cursor iteration.
tools/xwave-env apb begin -n my_apb -wr -json
tools/xwave-env apb next -n my_apb -wr -json
```

### 6. AXI Analysis

After loading an AXI JSON config, xwave can query read/write transactions, filter by address or ID, and analyze latency/outstanding behavior:

```bash
tools/xwave-env axi axi_cfg.json -n my_axi
tools/xwave-env axi wr -n my_axi
tools/xwave-env axi rd -n my_axi -id 0x3
tools/xwave-env axi latency -n my_axi -rd -json
tools/xwave-env axi osd -n my_axi -wr -json
```

### 7. Generic Event Query

Generic event query is intended for valid/ready/backpressure style interfaces. It samples on clock edges and evaluates expressions. Loaded event configs are bound to the current session FSDB, preventing accidental reuse when an old session ID points to another waveform.

Prepare an `if0.event.json` config:

```json
{
  "clk": "xif_tb_top.clk",
  "rst_n": "xif_tb_top.rst_n",
  "edge": "posedge",
  "signals": {
    "vld": "xif_tb_top.if0.vld",
    "rdy": "xif_tb_top.if0.rdy",
    "bp": "xif_tb_top.if0.bp",
    "pd": "xif_tb_top.if0.pd"
  },
  "fields": {
    "opcode": "pd[23:16]",
    "data": "pd[15:0]"
  }
}
```

```bash
tools/xwave-env event if0.event.json -n if0
tools/xwave-env event export -n if0 -expr "vld && rdy" -json
tools/xwave-env event export -n if0 -expr "vld && opcode == 0x10" -json
tools/xwave-env event find -n if0 -expr "vld && rdy && data != 0" -json
tools/xwave-env event find -n if0 -expr "vld && !bp" -context 200ns -axi axi0 -apb apb0 -json
```

Expressions support signal aliases, field aliases, `&&`, `||`, `!`, parentheses, `==`, `!=`, and binary/hex/decimal constants.

Validation rules:

- `edge` must be `posedge` or `negedge`; omitted `edge` defaults to `posedge`.
- `fields` bit ranges must be valid non-negative integers and must reference aliases in `signals`.
- Expressions are parsed and alias-checked before waveform scanning, so invalid expressions are reported even if the time window contains no events.
- Boolean values or comparisons containing `x/z` are treated as unknown and are not counted as matching events.
- `event export` defaults to at most 1000 rows unless `-limit` is explicitly set. Use a non-positive limit for full export.
- `-context <T>` can be combined with `-axi <name>`, `-apb <name>`, or both to attach protocol transaction context around each event hit.

### 8. Scope Signal Discovery

```bash
tools/xwave-env scope xring_tb_top.u_dut.u_pkt_fetch
tools/xwave-env scope xring_tb_top.u_dut.u_pkt_fetch -recursive -json
```

Use this to confirm real FSDB signal paths, especially for SystemVerilog arrays or generated scopes whose names may differ in VCS FSDB output.

### 9. Session Management

```bash
tools/xwave-env session list                # List all sessions.
tools/xwave-env session doctor -s 1         # Diagnose health.
tools/xwave-env session doctor -s 1 -json   # Diagnose in JSON.
tools/xwave-env session gc                  # Clean stale/idle sessions.
tools/xwave-env session kill 1              # Kill one session.
tools/xwave-env session kill all            # Kill all sessions.
```

`open` canonicalizes the FSDB path and reuses a healthy session for the same file. Sessions record FSDB mtime/size/dev/inode. If the file changes, the next query reports the change and automatically restarts the daemon while preserving the session ID and loaded configs.

---

## Command Reference

| Command | Description |
|---|---|
| `xwave open <fsdb-file>` | Open an FSDB and create a session |
| `xwave session list` | List all active sessions |
| `xwave session doctor -s <sid> [-json]` | Diagnose a session |
| `xwave session gc` | Clean stale/idle sessions |
| `xwave session kill <id\|all>` | Kill one or all sessions |
| `xwave scope <path> [-recursive] [-json] [-s <sid>]` | List FSDB signals under a scope |
| `xwave value <sig> <time> [-b\|-d] [-s <sid>]` | Query one signal value |
| `xwave list new <name> [-s <sid>]` | Create a list |
| `xwave list add <sig> [-s <sid>] [-l <name>]` | Add a signal to a list |
| `xwave list del <sig\|idx> [-s <sid>] [-l <name>]` | Delete a signal from a list |
| `xwave list show [-s <sid>] [-l <name>]` | Show list contents |
| `xwave list value <time> [-l <name>] [-b\|-d] [-json] [-s <sid>]` | Batch-query list values |
| `xwave list validate [-l <name>] [-json] [-s <sid>]` | Validate list signals |
| `xwave list diff [-l <name>] [-b T] [-e T] [-s <sid>]` | Find the earliest difference time |
| `xwave apb <json> -n <name> [-s <sid>]` | Load an APB config |
| `xwave apb list [-n <name>] [-s <sid>]` | Show APB configs |
| `xwave apb wr\|rd [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]` | APB read/write query |
| `xwave apb begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | APB cursor iteration |
| `xwave axi <json> -n <name> [-s <sid>]` | Load an AXI config |
| `xwave axi list [-n <name>] [-s <sid>]` | Show AXI configs |
| `xwave axi wr\|rd [-n <name>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]` | AXI read/write query |
| `xwave axi begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | AXI cursor iteration |
| `xwave axi latency\|osd [-rd\|-wr\|-all] [-id <id>] [-json] [-n <name>] [-s <sid>]` | AXI latency/outstanding analysis |
| `xwave event <json> -n <name> [-s <sid>]` | Load a generic event config |
| `xwave event list [-n <name>] [-s <sid>]` | Show generic event configs |
| `xwave event find -n <name> -expr <expr> [-b T] [-e T] [-context T [-axi <axi>] [-apb <apb>]] [-json] [-s <sid>]` | Find the first matching event, optionally with AXI/APB context |
| `xwave event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-context T [-axi <axi>] [-apb <apb>]] [-json] [-s <sid>]` | Export an event table, default max 1000 rows, optionally with AXI/APB context |
| `xwave ai query <json\|-\|--json JSON>` | Run an AI JSON request with a stable envelope |
| `xwave ai schema` | Print the `xwave.ai.v1` request schema |
| `xwave ai actions` | Print AI API actions |

### AI JSON API

`xwave ai` is the stable JSON entry point for AI agents and scripts. Existing human CLI commands remain unchanged. Requests use `api_version/action/target/args/limits/output`; responses use `ok/action/session/summary/data/findings/suggested_next_actions/warnings/error/meta`.

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"value.at","target":{"fsdb":"waves.fsdb","auto_open":true},"args":{"signal":"top.clk","time":"10ns"}}'
tools/xwave-env ai query -
tools/xwave-env ai schema
tools/xwave-env ai actions
```

Current AI actions cover the main existing `session/scope/value/list/apb/axi/event` capabilities, condition/expression/window/signal/handshake waveform facts, and AXI/APB protocol fact extraction.

---

## Project Layout

```text
xwave/
├── xwave                    # Main executable generated by build
├── Makefile                 # Build script
├── README.md
├── tools/
│   └── xwave-env            # Environment wrapper that sets LD_LIBRARY_PATH
└── src/
    ├── main.cpp             # CLI entry point
    ├── json.hpp             # nlohmann/json single header
    ├── protocol/
    │   └── protocol.h       # Protocol constants
    ├── session/
    │   ├── session_registry.h/.cpp
    │   └── session_manager.h/.cpp
    ├── client/
    │   └── client.h/.cpp    # Unix socket client communication
    ├── server/
    │   ├── server.h/.cpp    # Daemon loop and command dispatch
    │   └── fsdb_value_reader.h/.cpp  # NPI FSDB value reader wrapper
    ├── list/
    │   ├── signal_list.h
    │   └── list_manager.h/.cpp  # Persistent list management
    ├── apb/
    │   ├── apb_config.h
    │   ├── apb_manager.h/.cpp   # Persistent APB config management
    │   └── apb_analyzer.h/.cpp  # APB FSDB analyzer
    ├── axi/
    │   ├── axi_config.h
    │   ├── axi_manager.h/.cpp   # Persistent AXI config management
    │   └── axi_analyzer.h/.cpp  # AXI FSDB analyzer
    ├── event/
    │   ├── event_config.h
    │   ├── event_manager.h/.cpp    # Persistent generic event config management
    │   └── event_analyzer.h/.cpp   # Generic event FSDB analyzer
    ├── common/
    │   └── time_parser.h/.cpp  # Time string parser
    └── commands/
        ├── cmd_session.h/.cpp
        ├── cmd_value.h/.cpp
        ├── cmd_list.h/.cpp
        ├── cmd_apb.h/.cpp
        ├── cmd_axi.h/.cpp
        └── cmd_event.h/.cpp
```

---

## Notes

- Signal paths must exactly match the hierarchy stored in the FSDB, for example `test_top.u_data_gen.cnt_a`.
- If `-s` is omitted, xwave uses the latest session. If `-l` / `-n` is omitted, it uses the most recently modified list/config.
- `list diff` requires at least two signals.
- Sessions run as background daemons and survive terminal exits. Use `session kill` or `session gc` to clean them.
- The default idle timeout is 1800 seconds and can be overridden with `XWAVE_IDLE_TIMEOUT_SEC`.
- Default value output is hexadecimal and formatted as `'h...`.
- Event configs are bound by `Session + FSDB`. Old `.xwave.events` records without FSDB metadata are not automatically reused; rerun `xwave event <json> -n <name>` to migrate them.
