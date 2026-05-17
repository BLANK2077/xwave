[English](README.md) | 中文

# xwave

xwave 是基于 Synopsys NPI 的 FSDB 波形查询工具。它可以在不启动 Verdi GUI 的情况下，从命令行查询波形事实：信号值、scope 信号、信号列表、APB/AXI 事务、valid/ready 通用事件，以及 AI 友好的 JSON 结果。

同一个 `xwave` 二进制同时承担 CLI 客户端和后台 daemon。客户端为每个 FSDB Session 启动一个本机 daemon，并通过 Unix domain socket 通信。

## 快速开始

除非你已经手动设置好了所有 Verdi/NPI 动态库路径，否则推荐始终使用包装脚本：

```bash
tools/xwave-env <command> ...
```

打开 FSDB：

```bash
tools/xwave-env open /path/to/waves.fsdb
```

输出示例：

```text
[Session 1] Ready (FSDB: 0 ~ 200000)
[Session 1] FSDB opened: /path/to/waves.fsdb
```

查询信号值：

```bash
tools/xwave-env value top.clk 10ns       # 默认十六进制
tools/xwave-env value top.clk 10ns -b    # 二进制
tools/xwave-env value top.count 10ns -d  # 十进制
```

发现 FSDB 中真实 dump 的信号：

```bash
tools/xwave-env scope top.u_dut -recursive -json
```

管理一个小信号列表：

```bash
tools/xwave-env list new if0
tools/xwave-env list add top.u_dut.valid -l if0
tools/xwave-env list add top.u_dut.ready -l if0
tools/xwave-env list value 42us -l if0 -json
tools/xwave-env list diff -l if0 -b 0ns -e 100us
```

结束后清理 Session：

```bash
tools/xwave-env session kill 1
```

## 核心概念

### Session

`open` 会规范化 FSDB 路径，并复用同一文件的健康 Session。每个 Session 拥有：

- 一个 FSDB 文件和一个 NPI 上下文
- 一个 daemon 进程
- 一个位于 `~/.xwave/sessions/<sid>/socket` 的 Unix domain socket
- 一条 `~/.xwave/registry.json` 记录

Session 会记录 FSDB 的 mtime、size、device、inode。若 FSDB 被替换或更新，下一次访问会提示文件变化，并在保留 Session ID 和已加载配置的情况下重启 daemon。

Session 也有 idle timeout。默认 1800 秒，可通过 `XWAVE_IDLE_TIMEOUT_SEC` 覆盖。长时间交互 debug 建议设置更大值，例如：

```bash
export XWAVE_IDLE_TIMEOUT_SEC=28800
```

idle timeout 后 daemon 会退出并释放 FSDB/NPI 句柄。重新执行 `open <fsdb>` 会创建新的 Session；如果没有成功，请使用 `--debug` 查看具体失败阶段。

### TimeSpec 与游标

所有需要时间的命令都接受 `TimeSpec`。TimeSpec 可以是绝对时间、已保存游标，或游标加减时间偏移：

```text
100ns
@deadlock
@deadlock-20ns
@deadlock+5ns
@-10ns
@+5ns
```

绝对时间支持 `us`、`ns`、`ps`、`fs`。如果不写单位，默认按 `ns` 处理。`@` 表示当前 active cursor。`@deadlock-10cycle(top.clk)` 这类 cycle offset 语法已预留，当前版本会返回 `CLOCK_OFFSET_UNSUPPORTED`。

时间转换发生在 daemon 侧。daemon 打开 FSDB 后，会基于当前 FSDB 的 time scale 调用 NPI 时间转换 API，因此不要假设 FSDB 内部时间单位总是 `ps`。

示例：

```bash
tools/xwave-env value top.clk 10ns
tools/xwave-env cursor set deadlock 120340ns -note rready_stall_start
tools/xwave-env value top.rready --at @deadlock-20ns
tools/xwave-env list diff -l if0 -b 5ns -e 50ns
tools/xwave-env event find -n if0 -expr "valid && ready" -b 100us
```

非法单位（如 `10abc`）和负时间（如 `-1ns`）会被拒绝。

### LSF / bsub

xwave Session 是本机资源。daemon 通过 Unix domain socket 连接，健康检查依赖本机 PID 和 `/proc` 状态。因此在 LSF 环境中，`open` 和所有后续命令必须运行在同一台机器。

推荐方案：联系 IT 配置一个只包含一台合适机器的专用队列，给 xwave/xtrace 这类 NPI 工具使用。

```bash
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env open /path/to/waves.fsdb"
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env value top.clk 10ns -s 1"
bsub -q <xwave_queue> -I "cd <workdir> && tools/xwave-env session kill 1"
```

这台专用机器需要能访问共享 FSDB 路径，并拥有一致的 Verdi/NPI/license 环境。如果暂时没有单机专用队列，次选方案是 `bsub -m <host>`。如果固定机器仍不能接受，就需要做架构改造，例如 TCP daemon、远程转发，或无 daemon 的单次查询模式。

## 主要工作流

### 信号列表

```bash
tools/xwave-env list new my_signals
tools/xwave-env list add test_top.clk -l my_signals
tools/xwave-env list add test_top.rst_n -l my_signals
tools/xwave-env list show -l my_signals
tools/xwave-env list value 15ns -l my_signals -d -json
tools/xwave-env list validate -l my_signals
tools/xwave-env list diff -l my_signals -b 5ns -e 50ns
```

`list add` 会先探测信号是否存在再保存。`list value` 遇到旧列表里的缺失信号会输出 `NOT_FOUND` 并返回非零。`list diff` 至少需要两个信号。

### APB 事务

APB 配置：

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

命令：

```bash
tools/xwave-env apb apb.json -n apb0
tools/xwave-env apb wr -n apb0
tools/xwave-env apb rd -n apb0
tools/xwave-env apb wr -n apb0 -addr 0x100 -num 3 -json
tools/xwave-env apb begin -n apb0 -wr -json
tools/xwave-env apb next -n apb0 -wr -json
```

### AXI 事务

加载 AXI JSON 配置后，xwave 可以查询读写事务、按地址或 ID 过滤，并分析 latency/outstanding。

```bash
tools/xwave-env axi axi_cfg.json -n axi0
tools/xwave-env axi wr -n axi0
tools/xwave-env axi rd -n axi0 -id 0x3
tools/xwave-env axi latency -n axi0 -rd -json
tools/xwave-env axi osd -n axi0 -wr -json
```

### 通用事件

通用事件适合 valid/ready/backpressure 风格接口。xwave 会在时钟边沿采样 alias，并对布尔表达式求值。

Event 配置：

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

命令：

```bash
tools/xwave-env event if0.event.json -n if0
tools/xwave-env event export -n if0 -expr "valid && ready" -json
tools/xwave-env event export -n if0 -expr "valid && opcode == 0x10" -json
tools/xwave-env event find -n if0 -expr "valid && ready && data != 0" -json
tools/xwave-env event find -n if0 -expr "valid && !bp" -context 200ns -axi axi0 -apb apb0 -json
```

规则：

- `edge` 只能是 `posedge` 或 `negedge`；省略时默认为 `posedge`
- `fields` 位段必须是合法非负整数，且引用 `signals` 中定义的 alias
- 表达式会在扫描波形前先做语法和 alias 校验
- 含 `x/z` 的布尔值或比较结果为 unknown，不会被当作匹配事件
- `event export` 未指定 `-limit` 时默认最多 1000 条
- `-context <T>` 可搭配 `-axi <name>`、`-apb <name>` 或二者同时使用

## AI JSON 接口

`xwave ai` 是给 AI Agent 和脚本使用的稳定 JSON 入口；面向人的旧 CLI 行为保持不变。

```bash
tools/xwave-env ai query request.json
tools/xwave-env ai query -
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"value.at","target":{"fsdb":"waves.fsdb","auto_open":true},"args":{"signal":"top.clk","time":"10ns"}}'
tools/xwave-env ai schema
tools/xwave-env ai actions
```

请求 envelope：

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
  "output": {}
}
```

响应 envelope：

```text
ok/action/session/summary/data/findings/suggested_next_actions/warnings/error/meta
```

脚本里应解析 JSON，而不是解析人类文本：

```bash
tools/xwave-env ai query --json '{"api_version":"xwave.ai.v1","action":"session.list"}' \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ok"], d.get("summary", {}))'
```

当前 AI action 覆盖 `session/cursor/scope/value/list/apb/axi/event`，以及 condition、expression、window、signal inspection、anomaly detection、handshake 和协议调试事实。

## Session 排障

Session 创建或重连失败时，使用 `--debug` 或 `XWAVE_DEBUG=1`。

```bash
tools/xwave-env open /path/to/waves.fsdb --debug
tools/xwave-env session doctor -s 1 --debug
tools/xwave-env session gc --debug
```

debug 输出写入 stderr。server 启动细节写入：

```text
~/.xwave/sessions/<sid>/debug.log
```

诊断信息会标出 FSDB stat、registry lock、fork/exec、`npi_init`、`npi_fsdb_open`、socket bind/listen、connect、PING、idle timeout 和 cleanup reason 等阶段。

`Failed to create session` 的常见原因：

- FSDB 路径不可访问
- Verdi/NPI 运行环境或 license 缺失
- `npi_fsdb_open` 失败
- 大 FSDB 打开时间超过 `XWAVE_SESSION_START_TIMEOUT_SEC`（默认 60 秒）
- `$HOME` 无法创建 registry/socket 文件
- LSF 把后续命令调度到了不同 host

## 命令速查

| 命令 | 说明 |
|---|---|
| `xwave open <fsdb-file> [--debug]` | 打开 FSDB，创建或复用 Session |
| `xwave session list` | 列出活跃 Session |
| `xwave session doctor -s <sid> [-json] [--debug]` | 诊断 Session |
| `xwave session gc [--debug]` | 清理 stale/idle Session |
| `xwave session kill <id\|all> [--debug]` | 关闭一个或所有 Session |
| `xwave cursor set|get|list|delete|use ...` | 管理 Session 时间游标 |
| `xwave scope <path> [-recursive] [-json] [-s <sid>]` | 列出 scope 下信号 |
| `xwave value <sig> <time_spec>\|--at <time_spec> [-b\|-d] [-s <sid>]` | 查询单个信号值 |
| `xwave list new <name> [-s <sid>]` | 创建信号列表 |
| `xwave list add <sig> [-s <sid>] [-l <name>]` | 添加信号 |
| `xwave list del <sig\|idx> [-s <sid>] [-l <name>]` | 按路径或序号删除 |
| `xwave list show [-s <sid>] [-l <name>]` | 显示列表内容 |
| `xwave list value <time_spec>\|--at <time_spec> [-l <name>] [-b\|-d] [-json] [-s <sid>]` | 批量查询列表值 |
| `xwave list validate [-l <name>] [-json] [-s <sid>]` | 校验列表信号 |
| `xwave list diff [-l <name>] [-b T] [-e T] [-s <sid>]` | 查找最早差异时间 |
| `xwave apb <json> -n <name> [-s <sid>]` | 加载 APB 配置 |
| `xwave apb list [-n <name>] [-s <sid>]` | 查看 APB 配置 |
| `xwave apb wr\|rd [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]` | 查询 APB 读写 |
| `xwave apb begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | APB 游标 |
| `xwave axi <json> -n <name> [-s <sid>]` | 加载 AXI 配置 |
| `xwave axi list [-n <name>] [-s <sid>]` | 查看 AXI 配置 |
| `xwave axi wr\|rd [-n <name>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]` | 查询 AXI 读写 |
| `xwave axi begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | AXI 游标 |
| `xwave axi latency\|osd [-rd\|-wr\|-all] [-id <id>] [-json] [-n <name>] [-s <sid>]` | AXI 延迟/outstanding 分析 |
| `xwave event <json> -n <name> [-s <sid>]` | 加载 event 配置 |
| `xwave event list [-n <name>] [-s <sid>]` | 查看 event 配置 |
| `xwave event find -n <name> -expr <expr> [-b T] [-e T] [-context T [-axi <axi>] [-apb <apb>]] [-json] [-s <sid>]` | 查找第一个匹配事件 |
| `xwave event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-context T [-axi <axi>] [-apb <apb>]] [-json] [-s <sid>]` | 导出匹配事件 |
| `xwave ai query <json\|-\|--json JSON>` | 执行 AI JSON 请求 |
| `xwave ai schema` | 输出 `xwave.ai.v1` 请求 schema |
| `xwave ai actions` | 输出 AI action 列表 |

使用 `xwave help <topic>` 查看二级 help，例如：

```bash
tools/xwave-env help event
tools/xwave-env help session
tools/xwave-env help ai
```

## 构建与环境

环境要求：

- Linux 64 位
- 支持 C++11 的 GCC
- 正确设置 `VERDI_HOME` 的 Synopsys Verdi
- libstdc++ ABI 与所选 Verdi/NPI 库兼容

已验证版本：

- Verdi: `V-2023.12-SP1-1` / `V-2023.12-SP2`
- VCS: `V-2023.12-SP2_Full64`
- G++: GCC `8.5.0`

构建：

```bash
make clean && make
```

说明：

- xwave 链接 Synopsys NPI L1 FSDB C++ API，例如 `npi_fsdb_sig_value_at(..., std::string&, ...)`
- Verdi 2023 NPI 库导出 `std::__cxx11::basic_string` 等 new ABI 符号，需要 GCC 5+
- 不要在 Verdi 2023 NPI 库环境下使用 `-D_GLIBCXX_USE_CXX11_ABI=0`
- 使用 VCS 构建测试波形时，本机需要设置 `VCS_TARGET_ARCH=linux64`

## 架构与文件

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

持久化文件：

- `~/.xwave/registry.json`：活跃 Session 记录
- `~/.xwave/registry.lock`：registry 锁
- `~/.xwave/sessions/<sid>/session.json`：单个 Session 元数据
- `~/.xwave/sessions/<sid>/socket`：Session socket
- `~/.xwave/sessions/<sid>/debug.log`：server debug log
- `~/.xwave/sessions/<sid>/lists.json`：信号列表
- `~/.xwave/sessions/<sid>/apb.json`：APB 配置
- `~/.xwave/sessions/<sid>/axi.json`：AXI 配置
- `~/.xwave/sessions/<sid>/events.json`：event 配置
- `~/.xwave/sessions/<sid>/cursors.json`：Session 时间游标

旧版顶层维护文件（例如 `~/.xwave.registry`、`~/.xwave.lists`）在新 JSON 文件不存在且存在匹配的旧 registry 记录时会被只读迁移。新版本只写入 `~/.xwave/` 目录。

源码目录：

```text
src/
├── main.cpp
├── commands/     # CLI 命令处理
├── session/      # Session registry 和生命周期
├── client/       # Unix socket client
├── server/       # daemon 主循环和命令分发
├── list/         # 持久化信号列表
├── apb/          # APB 配置和 analyzer
├── axi/          # AXI 配置和 analyzer
├── event/        # 通用事件配置和 analyzer
└── common/       # 时间解析等共享工具
```
