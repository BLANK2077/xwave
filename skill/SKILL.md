---
name: xwave
description: >
  Use whenever the user needs to query FSDB waveform files from the command line
  without launching Verdi. Covers xwave — a CLI tool for reading signal values,
  managing signal lists, finding waveform diffs, and analyzing APB/AXI/generic-event
  interfaces. Also use when the user mentions xwave, xwave-env, fsdb query,
  waveform CLI, xwave open/value/list/apb/axi/event/session, FSDB signal dump,
  valid/ready handshake analysis, or APB/AXI transaction statistics from waveforms.
---

# xwave — FSDB 波形命令行工具

## 概述

xwave 是基于 Synopsys NPI 的 FSDB 命令行查询工具。无需启动 Verdi 即可在命令行：
- 查询信号值
- 管理信号列表、批量查询、定位差异
- 发现 FSDB scope 下的真实信号路径
- 统计分析 APB / AXI 接口事务
- 按表达式查询 valid/ready/backpressure 通用事件

同一个二进制文件 `xwave` 既是 CLI 客户端也是后台 Daemon（通过 Unix Domain Socket 通信）。

## 环境

- `$VERDI_HOME` 必须指向 Verdi 安装目录
- 推荐用 `tools/xwave-env` 脚本代替直接调用 `xwave`——它会自动设置 `LD_LIBRARY_PATH`

```bash
# 推荐方式
tools/xwave-env <子命令> ...
# 等价于先 export LD_LIBRARY_PATH 再直接调用 xwave
```

## 基本流程

```bash
# 1. 打开 FSDB
tools/xwave-env open /path/to/wave.fsdb
# → [Session 1] Ready (FSDB: 0 ~ 200000)

# 2. 查询 / 分析
tools/xwave-env value test_top.clk 10ns
tools/xwave-env event if0.json -n if0 -s 1
# ...

# 3. 用完关闭
tools/xwave-env session kill 1
```

## 通用选项

| 选项 | 作用 | 适用命令 |
|------|------|----------|
| `-s <sid>` | 指定 Session ID，省略则用最新 | 几乎所有命令 |
| `-b` | 二进制输出 | `value`, `list value` |
| `-d` | 十进制输出 | `value`, `list value` |
| `-json` | JSON 格式输出 | 多数查询命令 |
| `-b <T>` | 开始时间 | `list diff`, `event find/export` |
| `-e <T>` | 结束时间 | `list diff`, `event find/export` |
| `-l <name>` | 指定信号列表名，省略则用最近修改的 | `list` 子命令 |
| `-n <name>` | 指定配置名，省略则用最近修改的 | `apb`, `axi`, `event` 子命令 |

时间格式：`us`/`ns`/`ps`/`fs` 后缀（不区分大小写），默认 `ns`。例如 `10ns`, `100us`, `500ps`。

## 详细命令选项速查

### `open`

| 位置/选项 | 作用 |
|-----------|------|
| `<fsdb-file>` | 要打开的 FSDB 文件。相对路径会规范化为 canonical path；同一 canonical FSDB 已有健康 Session 时会复用。 |

`open` 会记录 FSDB 的 mtime/size/dev/inode。后续访问该 Session 时如果文件变化，xwave 会提示并自动重启 daemon，保留 Session ID。

### `session`

| 子命令/选项 | 作用 |
|-------------|------|
| `list` | 列出 Session ID、PID、RSS、创建时间、最后活跃时间和 FSDB 路径。 |
| `doctor` | 检查 registry、FSDB fingerprint、进程、socket 和 PING/PONG 健康状态。 |
| `gc` | 清理 stale Session 和超过 idle timeout 的 Session。 |
| `kill <id>` | 停止指定 Session daemon，并删除对应 registry/config 清理项。 |
| `kill all` | 停止并清理所有 Session。 |
| `-s <sid>` | `doctor` 要诊断的 Session ID。 |
| `-json` | `doctor` 输出 JSON，便于脚本判断。 |

默认 idle timeout 是 1800 秒，可用 `XWAVE_IDLE_TIMEOUT_SEC` 覆盖。

### `value`

| 位置/选项 | 作用 |
|-----------|------|
| `<信号路径>` | FSDB 中的完整信号路径。 |
| `<时间>` | 查询时间，支持 `us/ns/ps/fs`，默认 `ns`。 |
| `-b` | 二进制输出。 |
| `-d` | 十进制输出。 |
| `-s <sid>` | 指定 Session；省略则使用最新 Session。 |

默认输出为十六进制。

### `list`

| 子命令/选项 | 作用 |
|-------------|------|
| `new <列表名>` | 为当前 Session 创建信号列表。 |
| `add <信号路径>` | 加入信号；写入前会 probe 信号存在性，失败则不污染列表。 |
| `del <信号路径\|序号>` | 按信号路径或 `list show` 的 1-based 序号删除。 |
| `show` | 显示列表内容和序号。 |
| `value <时间>` | 查询列表内所有信号在指定时间的值。 |
| `validate` | 校验列表内信号是否仍存在于当前 FSDB。 |
| `diff` | 查找列表中至少两个信号值不全相等的最早时间。 |
| `-l <列表名>` | 指定列表；省略则使用当前 Session 最近修改的列表。 |
| `-s <sid>` | 指定 Session；省略则使用最新 Session。 |
| `-b` | `list value` 二进制输出。 |
| `-d` | `list value` 十进制输出。 |
| `-json` | `list value` / `list validate` 输出 JSON。 |
| `-b <T>` | `list diff` 开始时间，默认 0。 |
| `-e <T>` | `list diff` 结束时间，默认波形结束。 |

`list value` 遇到旧列表里的无效信号会显示 `NOT_FOUND` 并返回非零。`list diff` 至少需要 2 个信号。

### `scope`

| 位置/选项 | 作用 |
|-----------|------|
| `<scope路径>` | 要列出的 FSDB scope。 |
| `-recursive` | 递归列出子 scope 下的信号。 |
| `-json` | JSON 输出。 |
| `-s <sid>` | 指定 Session；省略则使用最新 Session。 |

`scope` 用于确认 VCS FSDB 中真实 signal path，特别是数组、generate scope、层次名被展开后的形态。

### `apb`

| 子命令/选项 | 作用 |
|-------------|------|
| `<json文件> -n <配置名>` | 加载 APB 配置并持久化。 |
| `list` | 查看指定 APB 配置；省略 `-n` 时查看最近配置。 |
| `wr` / `rd` | 查询 APB 写/读事务数量或指定事务。 |
| `begin` | 游标定位到第一条匹配事务。 |
| `next` | 游标移动到下一条匹配事务。 |
| `pre` | 游标移动到上一条匹配事务。 |
| `last` | 游标定位到最后一条匹配事务。 |
| `-n <配置名>` | 配置名；加载时必需，查询时省略则使用最近配置。 |
| `-s <sid>` | 指定 Session；省略则使用最新 Session。 |
| `-addr <addr>` | 按地址过滤，支持十六进制或十进制。 |
| `-num <x>` | 选择第 x 条匹配事务，1-based。 |
| `-last` | 选择最后一条匹配事务。 |
| `-rd` | 游标命令只看读事务。 |
| `-wr` | 游标命令只看写事务。 |
| `-json` | JSON 输出。 |

APB JSON 必需字段：`paddr/pwdata/prdata/pwrite/penable/psel/clk/rst_n`；`edge` 可选，默认 `posedge`。

### `axi`

| 子命令/选项 | 作用 |
|-------------|------|
| `<json文件> -n <配置名>` | 加载 AXI 配置并持久化。 |
| `list` | 查看指定 AXI 配置；省略 `-n` 时查看最近配置。 |
| `wr` / `rd` | 查询 AXI 写/读事务数量或指定事务。 |
| `begin` / `next` / `pre` / `last` | 游标遍历事务。 |
| `latency` | 统计读/写事务延迟。 |
| `osd` | 统计 outstanding 深度。 |
| `-n <配置名>` | 配置名；加载时必需，查询时省略则使用最近配置。 |
| `-s <sid>` | 指定 Session；省略则使用最新 Session。 |
| `-addr <addr>` | 按地址过滤，支持十六进制或十进制。 |
| `-id <id>` | 按 AXI ID 过滤，支持十六进制或十进制。 |
| `-num <x>` | 选择第 x 条匹配事务，1-based。 |
| `-last` | 选择最后一条匹配事务。 |
| `-rd` | 只看读事务。 |
| `-wr` | 只看写事务。 |
| `-all` | `latency/osd` 同时统计读写事务；默认行为。 |
| `-json` | JSON 输出。 |

AXI JSON 需要完整 5 通道信号、`clk`、`rst_n`；`edge` 可选，默认 `posedge`。

### `event`

| 子命令/选项 | 作用 |
|-------------|------|
| `<json文件> -n <配置名>` | 加载通用 event 配置并绑定当前 Session 的 FSDB。 |
| `list` | 不带 `-n` 时列出当前 FSDB 下的 event 配置名；带 `-n` 时打印该配置。 |
| `find` | 返回第一条表达式为 true 的 event。 |
| `export` | 导出匹配 event 列表，默认最多 1000 条。 |
| `-n <配置名>` | event 配置名；加载/find/export 必需。 |
| `-s <sid>` | 指定 Session；省略则使用最新 Session。 |
| `-expr <表达式>` | 事件表达式，支持 alias、field、`!`、`&&`、`||`、`==`、`!=`、括号和常量。 |
| `-b <T>` | 查询开始时间，默认 0。 |
| `-e <T>` | 查询结束时间，默认波形结束。 |
| `-limit N` | `export` 最大输出条数；默认 1000，非正数表示不限制。 |
| `-context <T>` | 对每个 event 附带 `[event_time - T, event_time + T]` 内的协议事务上下文。 |
| `-axi <配置名>` | 在 context 中附带 AXI 事务；必须和 `-context` 同时使用。 |
| `-apb <配置名>` | 在 context 中附带 APB 事务；必须和 `-context` 同时使用。 |
| `-json` | JSON 输出；context 模式下会增加 `context.axi` / `context.apb` 字段。 |

`-context` 必须搭配至少一个 `-axi` 或 `-apb`；`-axi/-apb` 不能脱离 `-context` 单独使用。表达式会在扫描前做语法和 alias 校验；含 `x/z` 的比较结果为 unknown，最终不会匹配 event。

---

## 子命令参考

### open — 打开 FSDB

```bash
xwave open <fsdb-file>
```

输出 Session ID 和时间范围。`open` 会规范化 FSDB 路径；同一文件已有健康 Session 时会复用。Session 会记录 FSDB 的 mtime/size/dev/inode，后续查询发现文件变化时会保留 Session ID 并自动重启 daemon。

### value — 单信号值查询

```bash
xwave value <信号路径> <时间> [-b|-d] [-s <sid>]
```

默认十六进制 (`'h...`)，`-b` 二进制 (`'b...`)，`-d` 十进制 (`'d...`)。

### session — 会话管理

```bash
xwave session list                      # 列出所有 Session
xwave session doctor -s <sid> [-json]   # 诊断健康状态
xwave session gc                        # 清理 stale/idle Session
xwave session kill <id|all>             # 关闭 Session
```

默认 idle timeout 为 1800 秒，可通过 `XWAVE_IDLE_TIMEOUT_SEC` 覆盖。

### list — 信号列表

```bash
xwave list new <列表名> [-s <sid>]
xwave list add <信号路径> [-s <sid>] [-l <列表名>]
xwave list del <信号路径|序号> [-s <sid>] [-l <列表名>]
xwave list show [-s <sid>] [-l <列表名>]
xwave list value <时间> [-l <列表名>] [-b|-d] [-json] [-s <sid>]
xwave list validate [-l <列表名>] [-json] [-s <sid>]
xwave list diff [-l <列表名>] [-b <T>] [-e <T>] [-s <sid>]
```

`list add` 会校验信号是否存在；`list value` 遇到旧 List 中的无效信号会输出 `NOT_FOUND` 并返回非零。

`list diff` 找到时间范围内最早一个值不全相等的时刻，需要列表中至少 2 个信号。

### scope — 信号发现

```bash
xwave scope <scope路径> [-recursive] [-json] [-s <sid>]
```

用于列出 FSDB 中指定 scope 下的信号名，适合排查 VCS 对 SystemVerilog 数组、generate scope 的实际命名。

### apb — APB 接口

**配置文件格式 (JSON)：**

```json
{
  "paddr":   "test_top.apb.paddr",
  "pwdata":  "test_top.apb.pwdata",
  "prdata":  "test_top.apb.prdata",
  "pwrite":  "test_top.apb.pwrite",
  "penable": "test_top.apb.penable",
  "psel":    "test_top.apb.psel",
  "clk":     "test_top.clk",
  "rst_n":   "test_top.rst_n",
  "edge":    "posedge"
}
```

- 前 8 个字段必需，`edge` 可选（默认 `"posedge"`，也可 `"negedge"`）
- 所有值都是 FSDB 中的完整信号路径

**命令：**

```bash
xwave apb <json文件> -n <配置名> [-s <sid>]       # 加载配置
xwave apb list [-n <配置名>] [-s <sid>]           # 查看配置
xwave apb wr|rd [-n <名>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]
xwave apb begin|next|pre|last [-rd|-wr] [-json] [-n <名>] [-s <sid>]
```

- `wr`/`rd` 统计写/读事务数；加 `-addr` 过滤地址；加 `-num N` 查第 N 次；加 `-last` 查最后一次
- `begin`/`next`/`pre`/`last` 游标遍历，`-wr`/`-rd` 过滤方向

### axi — AXI 接口

配置文件需要完整的 AXI5 5 通道信号（30 个信号 + clk/rst_n/edge）。

**命令：**

```bash
xwave axi <json文件> -n <配置名> [-s <sid>]       # 加载配置
xwave axi list [-n <配置名>] [-s <sid>]           # 查看配置
xwave axi wr|rd [-n <名>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]
xwave axi begin|next|pre|last [-rd|-wr] [-json] [-n <名>] [-s <sid>]
xwave axi latency|osd [-rd|-wr|-all] [-id <id>] [-json] [-n <名>] [-s <sid>]
```

- `latency` 统计读/写延迟，`osd` 统计 outstanding 数量
- `-rd`/`-wr`/`-all` 过滤方向，`-all` 为默认

### event — 通用事件

适用于 valid/ready/backpressure 风格接口，在时钟边沿采样并对表达式求值。event 配置绑定到当前 Session 打开的 FSDB；如果重新打开了不同波形，需要重新加载 event JSON。

**配置文件格式 (JSON)：**

```json
{
  "clk":   "xif_tb_top.clk",
  "rst_n": "xif_tb_top.rst_n",
  "edge":  "posedge",
  "signals": {
    "vld": "xif_tb_top.if0.vld",
    "rdy": "xif_tb_top.if0.rdy",
    "bp":  "xif_tb_top.if0.bp",
    "pd":  "xif_tb_top.if0.pd"
  },
  "fields": {
    "opcode": "pd[23:16]",
    "data":   "pd[15:0]"
  }
}
```

- `clk` 和 `signals` 必需；`rst_n` 可选（不配则无复位门控）；`edge` 可选（默认 `posedge`，只能取 `posedge`/`negedge`）
- `signals`：键 = 别名，值 = FSDB 完整路径
- `fields`：可选，定义位段别名。格式 `"<signal_alias>[<high>:<low>]"`，也支持 `{"signal": "...", "left": N, "right": M}` 对象格式；位段必须是非负整数，且 signal alias 必须已在 `signals` 中定义

**表达式语法：**

| 元素 | 示例 |
|------|------|
| 信号别名 | `vld`, `rdy`, `bp` |
| 字段别名 | `opcode`, `data` |
| 逻辑运算 | `&&`, `||`, `!` |
| 比较运算 | `==`, `!=` |
| 常量 | `0b1010`, `0xff`, `'hff`, `10` |
| 括号 | `(vld || rdy) && !bp` |

典型表达式：
- `vld && rdy` — handshake 完成
- `vld && !bp` — 有效且无背压
- `vld && opcode == 0x5a` — 特定 opcode
- `vld && rdy && data == 0xa55a` — 特定 payload

**命令：**

```bash
xwave event <json文件> -n <配置名> [-s <sid>]     # 加载配置
xwave event list [-n <配置名>] [-s <sid>]         # 查看配置
xwave event find -n <名> -expr <表达式> [-b <T>] [-e <T>] [-context <T> [-axi <名>] [-apb <名>]] [-json] [-s <sid>]
xwave event export -n <名> -expr <表达式> [-b <T>] [-e <T>] [-limit N] [-context <T> [-axi <名>] [-apb <名>]] [-json] [-s <sid>]
```

- `find` 返回第一个匹配事件
- `export` 导出匹配事件，未指定 `-limit` 时默认最多 1000 条；用 `-limit` 覆盖数量
- `-context <T>` 可搭配 `-axi <名>`、`-apb <名>` 或二者同时使用，在每条 event 前后 `T` 时间窗口内附带协议事务上下文
- 表达式会先做语法和 alias 校验；含 `x/z` 的比较结果为 unknown，不会作为匹配事件
- 旧版 `.xwave.events` 缺少 FSDB 绑定信息时不会被复用，重新加载配置即可迁移

---

## 典型场景

### 快速查信号值

```bash
tools/xwave-env open my_wave.fsdb
tools/xwave-env value test_top.clk 10ns
tools/xwave-env value test_top.rst_n 5ns -b
tools/xwave-env value test_top.cnt 100ns -d
```

### 批量监控一组信号

```bash
tools/xwave-env list new my_signals
tools/xwave-env list add test_top.clk -l my_signals
tools/xwave-env list add test_top.rst_n -l my_signals
tools/xwave-env list add test_top.cnt -l my_signals
tools/xwave-env list value 100ns -l my_signals
tools/xwave-env list diff -l my_signals -b 10ns -e 50ns
```

### APB 寄存器访问统计

```bash
tools/xwave-env apb apb_cfg.json -n my_apb
tools/xwave-env apb wr -n my_apb            # 总写次数
tools/xwave-env apb rd -n my_apb            # 总读次数
tools/xwave-env apb wr -n my_apb -addr 0x100  # 特定地址
tools/xwave-env apb wr -n my_apb -addr 0x100 -num 3 -json  # 第3次
tools/xwave-env apb begin -n my_apb -wr -json  # 游标定位第一次写
tools/xwave-env apb next -n my_apb -wr -json   # 下一个写
```

### AXI 性能分析

```bash
tools/xwave-env axi axi_cfg.json -n my_axi
tools/xwave-env axi wr -n my_axi              # 写事务总数
tools/xwave-env axi rd -n my_axi -id 0x3      # 特定 ID
tools/xwave-env axi latency -n my_axi -rd -json   # 读延迟统计
tools/xwave-env axi osd -n my_axi -wr -json       # 写 outstanding
```

### valid/ready 握手事件

```bash
tools/xwave-env event if0.event.json -n if0
tools/xwave-env event export -n if0 -expr "vld && rdy" -json
tools/xwave-env event export -n if0 -expr "vld && !bp" -limit 10 -json
tools/xwave-env event find -n if0 -expr "vld && !bp" -context 200ns -axi axi0 -apb apb0 -json
tools/xwave-env event export -n if0 -expr "vld && opcode == 0x10" -json
tools/xwave-env event find -n if0 -expr "vld && rdy && data != 0" -json
```

### Session 维护

```bash
tools/xwave-env session list
tools/xwave-env session doctor -s 1
tools/xwave-env session doctor -s 1 -json
tools/xwave-env session kill 1
tools/xwave-env session kill all
```

---

## 注意事项

- **信号路径必须与 FSDB 中完全一致**，含完整层级（如 `test_top.u_data_gen.cnt_a`）
- 不指定 `-s` 时自动用最新 Session；不指定 `-l`/`-n` 时自动用最近修改的列表/配置
- `list diff` 需至少 2 个信号
- Session 以后台 daemon 运行，终端关闭不影响；用 `session kill` 清理
- 默认输出为十六进制，无 `0x` 前缀，格式为 `'h...`
