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

---

## 子命令参考

### open — 打开 FSDB

```bash
xwave open <fsdb-file>
```

输出 Session ID 和时间范围。每次 `open` 会 fork 一个后台 daemon 进程。

### value — 单信号值查询

```bash
xwave value <信号路径> <时间> [-b|-d] [-s <sid>]
```

默认十六进制 (`'h...`)，`-b` 二进制 (`'b...`)，`-d` 十进制 (`'d...`)。

### session — 会话管理

```bash
xwave session list                      # 列出所有 Session
xwave session doctor -s <sid> [-json]   # 诊断健康状态
xwave session kill <id|all>             # 关闭 Session
```

### list — 信号列表

```bash
xwave list new <列表名> [-s <sid>]
xwave list add <信号路径> [-s <sid>] [-l <列表名>]
xwave list del <信号路径|序号> [-s <sid>] [-l <列表名>]
xwave list show [-s <sid>] [-l <列表名>]
xwave list value <时间> [-l <列表名>] [-b|-d] [-json] [-s <sid>]
xwave list diff [-l <列表名>] [-b <T>] [-e <T>] [-s <sid>]
```

`list diff` 找到时间范围内最早一个值不全相等的时刻，需要列表中至少 2 个信号。

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

适用于 valid/ready/backpressure 风格接口，在时钟边沿采样并对表达式求值。

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

- `clk` 和 `signals` 必需；`rst_n` 可选（不配则无复位门控）；`edge` 可选（默认 `posedge`）
- `signals`：键 = 别名，值 = FSDB 完整路径
- `fields`：可选，定义位段别名。格式 `"<signal_alias>[<high>:<low>]"`，也支持 `{"signal": "...", "left": N, "right": M}` 对象格式

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
xwave event find -n <名> -expr <表达式> [-b <T>] [-e <T>] [-json] [-s <sid>]
xwave event export -n <名> -expr <表达式> [-b <T>] [-e <T>] [-limit N] [-json] [-s <sid>]
```

- `find` 返回第一个匹配事件
- `export` 导出所有匹配事件，用 `-limit` 限制数量

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
