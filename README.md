# xwave

xwave 是一个基于 Synopsys NPI（Native Programming Interface）的 FSDB 波形查询工具。它采用客户端/服务端架构，支持多 Session 并发管理，提供单信号查询、信号列表（List）批量查询、波形差异定位等功能。

---

## 特性

- **单二进制双模式**：同一个可执行文件既是 CLI 客户端，也是后台 Daemon 服务端（通过 `--server` 启动）
- **多 Session 管理**：每个 Session 独立加载一个 FSDB 文件，拥有独立的 NPI 上下文和 Unix Domain Socket
- **信号单点查询**：`xwave value <signal> <time>`，支持十六进制（默认）、二进制、十进制
- **信号列表追踪**：
  - 创建/删除/查看 List
  - 批量查询 List 内所有信号在某一时刻的值
  - 自动支持 JSON 格式输出
- **波形差异定位**：`xwave list diff`，查找 List 中信号不全相等的最早时间点
- **APB 接口统计**：加载 APB JSON 配置后，可统计读写次数、按地址/序号/最后一次查询，并支持游标式遍历（`begin`/`next`/`pre`/`last`）
- **时间单位后缀**：所有时间输入默认单位为 `ns`，支持 `us`、`ns`、`ps`、`fs` 后缀

---

## 架构概览

```
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

- **Client**：处理命令行解析、Session 注册表管理、List 本地持久化
- **Server**：每个 Session fork 出一个 Daemon，加载 FSDB，通过 NPI 读取信号值
- **Protocol**：基于 Unix Domain Socket 的轻量级文本协议
- **Registry**：
  - `~/.xwave.registry` — Session 持久化记录
  - `~/.xwave.lists` — 各 Session 下的信号 List 持久化记录

---

## 编译构建

### 环境依赖

- Linux 64 位
- GCC 支持 C++11
- Synopsys Verdi（需正确设置 `VERDI_HOME` 环境变量）
- VCS（仅当需要自行编译产生 FSDB 测试文件时）

### 编译命令

```bash
cd xwave
make clean && make
```

编译成功后，当前目录生成 `xwave` 可执行文件。

---

## 快速开始

### 1. 打开 FSDB

```bash
./xwave open /path/to/your.fsdb
```

输出示例：

```
[Session 1] Ready (FSDB: 0 ~ 200000)
[Session 1] FSDB opened: /path/to/your.fsdb
```

### 2. 查询单个信号值

```bash
# 默认十六进制
./xwave value test_top.clk 10ns

# 二进制
./xwave value test_top.clk 10ns -b

# 十进制
./xwave value test_top.clk 10ns -d
```

输出示例：

```
'h0
'b0
'd0
```

### 3. 信号列表（List）管理

#### 创建 List

```bash
./xwave list new my_signals -s 1
```

#### 添加信号

```bash
./xwave list add test_top.clk -s 1 -l my_signals
./xwave list add test_top.rst_n -s 1 -l my_signals
```

> 若不指定 `-l <name>`，默认操作该 Session 下**最近被修改**的 List。

#### 查看 List 内容

```bash
./xwave list show -s 1 -l my_signals
```

输出示例：

```
1: test_top.clk
2: test_top.rst_n
```

#### 删除信号

支持两种删除方式：

```bash
# 按序号删除（1-based）
./xwave list del -s 1 -l my_signals 1

# 按信号路径删除
./xwave list del -s 1 -l my_signals test_top.rst_n
```

#### 批量查询 List 信号值

```bash
# 默认格式
./xwave list value 15ns -s 1 -l my_signals

# 二进制
./xwave list value 15ns -s 1 -l my_signals -b

# 十进制 JSON
./xwave list value 15ns -s 1 -l my_signals -d -json
```

默认格式输出示例：

```
test_top.clk:'h1
test_top.rst_n:'h1
```

### 4. 波形差异定位

查找 List 中信号不全相同的最早时间：

```bash
# 从 FSDB 起始时间到结束时间
./xwave list diff -s 1 -l my_signals

# 指定起始和结束范围
./xwave list diff -s 1 -l my_signals -b 5ns -e 50ns
```

输出示例：

```
1ns
5ns
```

### 5. APB 接口统计

首先准备一个 `apb.json`，包含各信号路径、`clk`、`rst_n` 和 `edge`（`posedge` 或 `negedge`）：

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

#### 加载配置

```bash
./xwave apb apb.json -n my_apb -s 1
```

#### 查看配置

```bash
./xwave apb list -n my_apb -s 1
```

#### 统计读写次数

```bash
./xwave apb wr -n my_apb -s 1
./xwave apb rd -n my_apb -s 1
```

#### 按地址、序号、最后一次查询

```bash
# 第一次写入地址 0x100
./xwave apb wr -n my_apb -s 1 -addr 0x100

# 该地址第 3 次写入
./xwave apb wr -n my_apb -s 1 -addr 0x100 -num 3

# 全部写入中的最后一次
./xwave apb wr -n my_apb -s 1 -last

# 第 5 次读取（JSON 输出）
./xwave apb rd -n my_apb -s 1 -num 5 -json
```

#### 游标遍历

```bash
# 第一次访问
./xwave apb begin -n my_apb -s 1

# 下一个访问（只显示写）
./xwave apb next -n my_apb -s 1 -wr

# 上一个访问（只显示读）
./xwave apb pre -n my_apb -s 1 -rd

# 最后一次访问
./xwave apb last -n my_apb -s 1
```

> 未指定 `-n` 时，自动使用该 Session 下最新加载的 APB 配置。

输出示例：

```
time=15ns addr='h00000002 data='h00000000
time=15ns type=RD addr='h00000002 data='h00000000
```

### 6. Session 管理

```bash
# 查看所有 Session
./xwave session list

# 检查指定 Session 健康状态
./xwave session doctor -s 1
./xwave session doctor -s 1 -json

# 关闭指定 Session
./xwave session kill 1

# 关闭所有 Session
./xwave session kill all
```

`session doctor` 必须显式指定 `-s <sid>`。文本输出用于人工排查，`-json` 输出固定包含
`session_id`、`healthy`、`status`、`message`、`pid`、`socket_path`、`fsdb_file`。
健康时 exit code 为 0，不健康或参数错误时为非零。

常见 `status`：

- `registry_missing`：registry 中没有该 Session。
- `process_exited`：registry 存在，但 server 进程已退出。
- `socket_missing`：server 进程存在，但 socket 文件缺失。
- `connect_failed`：socket 文件存在，但无法连接。
- `ping_failed`：socket 可连接，但 server 未响应 `PING`。
- `healthy`：server 可连接且 `PING/PONG` 正常。

---

## 命令速查

| 命令 | 说明 |
|------|------|
| `xwave open <fsdb-file>` | 打开 FSDB，创建新 Session |
| `xwave session list` | 列出所有活跃 Session |
| `xwave session doctor -s <sid> [-json]` | 诊断指定 Session 健康状态 |
| `xwave session kill <id|all>` | 关闭指定或所有 Session |
| `xwave value <sig> <time> [-b\|-d] [-s <sid>]` | 单信号值查询 |
| `xwave list new <name> [-s <sid>]` | 创建新 List |
| `xwave list add <sig> [-s <sid>] [-l <name>]` | 向 List 添加信号 |
| `xwave list del <sig\|idx> [-s <sid>] [-l <name>]` | 从 List 删除信号 |
| `xwave list show [-s <sid>] [-l <name>]` | 显示 List 内容 |
| `xwave list value <time> [-l <name>] [-b\|-d] [-json] [-s <sid>]` | 批量查询 List 值 |
| `xwave list diff [-l <name>] [-b T] [-e T] [-s <sid>]` | 查找最早差异时间 |
| `xwave apb <json> -n <name> [-s <sid>]` | 加载 APB 配置 |
| `xwave apb list [-n <name>] [-s <sid>]` | 查看 APB 配置 |
| `xwave apb wr [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]` | APB 写统计/查询 |
| `xwave apb rd [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]` | APB 读统计/查询 |
| `xwave apb begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | APB 游标遍历 |
| `xwave event <json> -n <name> [-s <sid>]` | 加载通用事件配置 |
| `xwave event list [-n <name>] [-s <sid>]` | 查看通用事件配置 |
| `xwave event find -n <name> -expr <expr> [-b T] [-e T] [-json] [-s <sid>]` | 查找第一个事件 |
| `xwave event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-json] [-s <sid>]` | 导出事件表 |
| `xwave help` | 显示帮助信息 |

---

## 项目结构

```
xwave/
├── xwave                        # 主可执行文件（编译生成）
├── Makefile                     # 编译脚本
├── README.md                    # 项目说明
├── testbench/                   # 独立测试用 Verilog 环境
│   ├── apb_if.sv
│   ├── ctrl_reg.sv
│   ├── data_gen.sv
│   ├── test_top.sv
│   ├── filelist.f
│   └── Makefile
├── docs/
│   └── NPI_FSDB_API.md          # NPI FSDB API 调研文档
└── src/
    ├── main.cpp                 # CLI 入口
    ├── protocol/
    │   └── protocol.h           # 协议常量定义
    ├── session/
    │   ├── session_registry.h/.cpp
    │   └── session_manager.h/.cpp
    ├── client/
    │   └── client.h/.cpp        # Unix Socket 客户端通信
    ├── server/
    │   ├── server.h/.cpp        # Daemon 主循环与命令分发
    │   └── fsdb_value_reader.h/.cpp   # NPI FSDB 读值封装
    ├── list/
    │   ├── signal_list.h
    │   └── list_manager.h/.cpp  # List 持久化管理
    ├── apb/
    │   ├── apb_config.h         # APB 配置结构体
    │   ├── apb_manager.h/.cpp   # APB 配置持久化管理
    │   └── apb_analyzer.h/.cpp  # APB FSDB 分析器
    ├── json.hpp                 # nlohmann/json 单头文件
    ├── common/
    │   └── time_parser.h/.cpp   # 时间字符串解析
    └── commands/
        ├── cmd_session.h/.cpp   # Session 命令
        ├── cmd_value.h/.cpp     # Value 命令
        ├── cmd_list.h/.cpp      # List 命令
        └── cmd_apb.h/.cpp       # APB 命令
```

---

## 测试

项目自带了一个独立的 SystemVerilog Testbench，可用于生成本地 FSDB 文件并进行全量功能验证。

### 通用事件查询

`event` 命令用于按时钟边沿采样任意 valid/ready/backpressure 风格接口。配置文件示例：

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

常用查询：

```bash
tools/xwave-env open /path/to/wave.fsdb
tools/xwave-env event testdata/xif_agent_event/if0.event.json -n if0 -s 1
tools/xwave-env event export -n if0 -expr "vld && rdy" -limit 3 -json -s 1
tools/xwave-env event export -n if0 -expr "vld && opcode == 0x10" -json -s 1
```

表达式 v1 支持 alias、字段 alias、`&&`、`||`、`!`、括号、`==`、`!=`，以及二进制/十六进制/十进制常量。`tools/xwave-env` 会按 `VERDI_HOME` 自动补齐 NPI `LD_LIBRARY_PATH`。

### 生成测试 FSDB

```bash
cd testbench
make clean && make comp
make run FSDB_NAME="test1.fsdb"
make run FSDB_NAME="test2.fsdb"
```

### 功能验证

建议验证以下场景：

1. 连续打开两个 FSDB，验证多 Session 共存
2. 在不同 Session 下创建同名 List，验证隔离性
3. 在同一 Session 下创建多个 List，验证独立管理
4. `value` 命令的三种进制输出
5. `list value` 的默认输出、二进制、JSON、十进制 JSON
6. `list del` 的按序号删除与按路径删除
7. `list diff` 的带范围与不带范围查询
8. `session kill` 的指定关闭与全部关闭
9. `apb` 配置加载与 `list` 查看
10. `apb wr` / `apb rd` 的计数与 `-addr` / `-num` / `-last` 查询
11. `apb begin` / `next` / `pre` / `last` 的游标遍历及 `-rd` / `-wr` 过滤
12. `apb` 命令的 `-json` 输出格式
13. `event find/export` 的文本输出、JSON 输出、字段过滤和无匹配结果

---

## 注意事项

- 所有时间参数在内部以 **1 ps** 为基准单位进行转换与存储。
- 信号路径需与 FSDB 中的层级完全一致（例如 `test_top.u_data_gen.cnt_a`）。
- `list diff` 至少需要 2 个信号才有意义；若只有 1 个信号，结果恒为 `(no diff found)`。
- 每个 Session 对应一个独立的 Daemon 进程，异常退出时会自动清理 Socket 文件。
