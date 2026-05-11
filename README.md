# xwave

xwave 是基于 Synopsys NPI 的 FSDB 波形命令行查询工具。无需启动 Verdi 即可在命令行查询信号值、管理信号列表、定位波形差异、统计分析 APB/AXI 接口事务、按表达式查询 valid/ready/backpressure 通用事件。

同一二进制文件 `xwave` 既是 CLI 客户端也是后台 Daemon（通过 Unix Domain Socket 通信）。

---

## 特性

- **单二进制双模式**：同一个可执行文件既是 CLI 客户端，也是后台 Daemon 服务端（通过 `--server` 启动）
- **多 Session 管理**：每个 Session 独立加载一个 FSDB 文件，拥有独立的 NPI 上下文和 Unix Domain Socket
- **FSDB 变动自恢复**：Session 记录 FSDB fingerprint；文件被替换或更新后，下次访问会自动重启对应 daemon
- **信号单点查询**：`xwave value <signal> <time>`，支持十六进制（默认）、二进制、十进制
- **信号列表追踪**：创建/删除/查看 List，批量查询 List 内所有信号在某一时刻的值，支持 JSON 输出
- **Scope 信号发现**：`xwave scope <path>` 列出 FSDB 中指定层级下的信号名
- **波形差异定位**：`xwave list diff`，查找 List 中信号不全相等的最早时间点
- **APB 接口统计**：加载 APB JSON 配置后，可统计读写次数、按地址/序号/最后一次查询，支持游标式遍历
- **AXI 接口统计**：加载 AXI JSON 配置后，可统计读写事务、按地址/ID 过滤，以及延迟和 outstanding 分析
- **通用事件查询**：加载 event JSON 配置后，按时钟边沿采样 valid/ready/backpressure 风格接口，支持表达式过滤
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

- **Client**：处理命令行解析、Session 注册表管理、List/APB/AXI/Event 配置本地持久化
- **Server**：每个 Session fork 出一个 Daemon，加载 FSDB，通过 NPI 读取信号值
- **Protocol**：基于 Unix Domain Socket 的轻量级文本协议
- **Registry**：
  - `~/.xwave.registry` — Session 持久化记录
  - `~/.xwave.lists` — 各 Session 下的信号 List 持久化记录

---

## 环境依赖

- Linux 64 位
- GCC 支持 C++11
- Synopsys Verdi（需正确设置 `VERDI_HOME` 环境变量）

推荐用 `tools/xwave-env` 脚本代替直接调用 `xwave`——它会自动设置 `LD_LIBRARY_PATH`：

```bash
tools/xwave-env <子命令> ...
```

---

## 编译

```bash
make clean && make
```

编译成功后，当前目录生成 `xwave` 可执行文件。

---

## 快速开始

### 1. 打开 FSDB

```bash
tools/xwave-env open /path/to/your.fsdb
```

输出示例：

```
[Session 1] Ready (FSDB: 0 ~ 200000)
[Session 1] FSDB opened: /path/to/your.fsdb
```

### 2. 查询单个信号值

```bash
# 默认十六进制
tools/xwave-env value test_top.clk 10ns

# 二进制
tools/xwave-env value test_top.clk 10ns -b

# 十进制
tools/xwave-env value test_top.clk 10ns -d
```

### 3. 信号列表（List）管理

```bash
# 创建 List
tools/xwave-env list new my_signals

# 添加信号
tools/xwave-env list add test_top.clk -l my_signals
tools/xwave-env list add test_top.rst_n -l my_signals

# 查看 List
tools/xwave-env list show -l my_signals

# 批量查询
tools/xwave-env list value 15ns -l my_signals
tools/xwave-env list value 15ns -l my_signals -d -json

# 校验 List 中信号是否存在
tools/xwave-env list validate -l my_signals
```

`list add` 会先校验信号是否存在；`list value` 遇到旧 List 中的无效信号会输出 `NOT_FOUND` 并返回非零。若不指定 `-l <name>`，默认操作该 Session 下最近被修改的 List。

### 4. 波形差异定位

```bash
# 从 FSDB 起始到结束
tools/xwave-env list diff -l my_signals

# 指定时间范围
tools/xwave-env list diff -l my_signals -b 5ns -e 50ns
```

`list diff` 至少需要 2 个信号。

### 5. APB 接口统计

准备 `apb.json` 配置文件：

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
# 加载配置
tools/xwave-env apb apb.json -n my_apb

# 统计读写次数
tools/xwave-env apb wr -n my_apb
tools/xwave-env apb rd -n my_apb

# 按地址过滤
tools/xwave-env apb wr -n my_apb -addr 0x100 -num 3 -json

# 游标遍历
tools/xwave-env apb begin -n my_apb -wr -json
tools/xwave-env apb next -n my_apb -wr -json
```

### 6. AXI 接口统计

加载 AXI JSON 配置后，可统计读写事务、按地址/ID 过滤，以及延迟和 outstanding 分析：

```bash
tools/xwave-env axi axi_cfg.json -n my_axi
tools/xwave-env axi wr -n my_axi
tools/xwave-env axi rd -n my_axi -id 0x3
tools/xwave-env axi latency -n my_axi -rd -json
tools/xwave-env axi osd -n my_axi -wr -json
```

### 7. 通用事件查询

适用于 valid/ready/backpressure 风格接口，按时钟边沿采样并对表达式求值。加载后的 event 配置会绑定到当前 Session 的 FSDB，避免复用旧 session id 时误用其他波形的配置。

准备 `if0.event.json` 配置文件：

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
```

表达式支持：信号别名、字段别名、`&&`、`||`、`!`、括号、`==`、`!=`，以及二进制/十六进制/十进制常量。

配置校验规则：

- `edge` 只能是 `posedge` 或 `negedge`，省略时默认为 `posedge`
- `fields` 位段必须是合法非负整数，且引用已定义的 `signals` alias
- 表达式会在扫描波形前先做语法和 alias 校验，即使时间窗口内没有事件也会报告坏表达式
- 含 `x/z` 的布尔值或比较结果为 unknown，最终不会被当作匹配事件
- `event export` 未显式指定 `-limit` 时默认最多导出 1000 条；需要全量导出时显式传入非正 limit

### 8. Scope 信号发现

```bash
tools/xwave-env scope xring_tb_top.u_dut.u_pkt_fetch
tools/xwave-env scope xring_tb_top.u_dut.u_pkt_fetch -recursive -json
```

用于确认 FSDB 中真实信号路径，尤其适合排查 SystemVerilog 数组或 generate scope 在 VCS FSDB 中的命名。

### 9. Session 管理

```bash
tools/xwave-env session list                # 列出所有 Session
tools/xwave-env session doctor -s 1         # 诊断健康状态
tools/xwave-env session doctor -s 1 -json   # JSON 格式诊断
tools/xwave-env session gc                  # 清理 stale/idle Session
tools/xwave-env session kill 1              # 关闭指定 Session
tools/xwave-env session kill all            # 关闭所有 Session
```

`open` 会规范化 FSDB 路径并复用同一文件的健康 Session。Session 记录 FSDB 的 mtime/size/dev/inode；若文件发生变化，下一次查询会提示并自动重启 daemon，保留原 Session ID 和已加载配置。

---

## 命令速查

| 命令 | 说明 |
|------|------|
| `xwave open <fsdb-file>` | 打开 FSDB，创建新 Session |
| `xwave session list` | 列出所有活跃 Session |
| `xwave session doctor -s <sid> [-json]` | 诊断指定 Session 健康状态 |
| `xwave session gc` | 清理 stale/idle Session |
| `xwave session kill <id\|all>` | 关闭指定或所有 Session |
| `xwave scope <path> [-recursive] [-json] [-s <sid>]` | 列出指定 scope 下的 FSDB 信号 |
| `xwave value <sig> <time> [-b\|-d] [-s <sid>]` | 单信号值查询 |
| `xwave list new <name> [-s <sid>]` | 创建新 List |
| `xwave list add <sig> [-s <sid>] [-l <name>]` | 向 List 添加信号 |
| `xwave list del <sig\|idx> [-s <sid>] [-l <name>]` | 从 List 删除信号 |
| `xwave list show [-s <sid>] [-l <name>]` | 显示 List 内容 |
| `xwave list value <time> [-l <name>] [-b\|-d] [-json] [-s <sid>]` | 批量查询 List 值 |
| `xwave list validate [-l <name>] [-json] [-s <sid>]` | 校验 List 中信号是否存在 |
| `xwave list diff [-l <name>] [-b T] [-e T] [-s <sid>]` | 查找最早差异时间 |
| `xwave apb <json> -n <name> [-s <sid>]` | 加载 APB 配置 |
| `xwave apb list [-n <name>] [-s <sid>]` | 查看 APB 配置 |
| `xwave apb wr\|rd [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]` | APB 读写统计/查询 |
| `xwave apb begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | APB 游标遍历 |
| `xwave axi <json> -n <name> [-s <sid>]` | 加载 AXI 配置 |
| `xwave axi list [-n <name>] [-s <sid>]` | 查看 AXI 配置 |
| `xwave axi wr\|rd [-n <name>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]` | AXI 读写统计/查询 |
| `xwave axi begin\|next\|pre\|last [-rd\|-wr] [-json] [-n <name>] [-s <sid>]` | AXI 游标遍历 |
| `xwave axi latency\|osd [-rd\|-wr\|-all] [-id <id>] [-json] [-n <name>] [-s <sid>]` | AXI 延迟/outstanding 分析 |
| `xwave event <json> -n <name> [-s <sid>]` | 加载通用事件配置 |
| `xwave event list [-n <name>] [-s <sid>]` | 查看通用事件配置 |
| `xwave event find -n <name> -expr <expr> [-b T] [-e T] [-json] [-s <sid>]` | 查找第一个匹配事件 |
| `xwave event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-json] [-s <sid>]` | 导出事件表，默认最多 1000 条 |

---

## 项目结构

```
xwave/
├── xwave                    # 主可执行文件（编译生成）
├── Makefile                 # 编译脚本
├── README.md
├── tools/
│   └── xwave-env            # 环境启动脚本（自动设置 LD_LIBRARY_PATH）
└── src/
    ├── main.cpp             # CLI 入口
    ├── json.hpp             # nlohmann/json 单头文件
    ├── protocol/
    │   └── protocol.h       # 协议常量定义
    ├── session/
    │   ├── session_registry.h/.cpp
    │   └── session_manager.h/.cpp
    ├── client/
    │   └── client.h/.cpp    # Unix Socket 客户端通信
    ├── server/
    │   ├── server.h/.cpp    # Daemon 主循环与命令分发
    │   └── fsdb_value_reader.h/.cpp  # NPI FSDB 读值封装
    ├── list/
    │   ├── signal_list.h
    │   └── list_manager.h/.cpp  # List 持久化管理
    ├── apb/
    │   ├── apb_config.h
    │   ├── apb_manager.h/.cpp   # APB 配置持久化管理
    │   └── apb_analyzer.h/.cpp  # APB FSDB 分析器
    ├── axi/
    │   ├── axi_config.h
    │   ├── axi_manager.h/.cpp   # AXI 配置持久化管理
    │   └── axi_analyzer.h/.cpp  # AXI FSDB 分析器
    ├── event/
    │   ├── event_config.h
    │   ├── event_manager.h/.cpp    # 通用事件配置持久化管理
    │   └── event_analyzer.h/.cpp   # 通用事件 FSDB 分析器
    ├── common/
    │   └── time_parser.h/.cpp  # 时间字符串解析
    └── commands/
        ├── cmd_session.h/.cpp
        ├── cmd_value.h/.cpp
        ├── cmd_list.h/.cpp
        ├── cmd_apb.h/.cpp
        ├── cmd_axi.h/.cpp
        └── cmd_event.h/.cpp
```

---

## 注意事项

- 信号路径需与 FSDB 中的层级完全一致（例如 `test_top.u_data_gen.cnt_a`）
- 不指定 `-s` 时自动用最新 Session；不指定 `-l`/`-n` 时自动用最近修改的列表/配置
- `list diff` 需至少 2 个信号
- Session 以后台 daemon 运行，终端关闭不影响；用 `session kill` 或 `session gc` 清理
- 默认 idle timeout 为 1800 秒，可通过 `XWAVE_IDLE_TIMEOUT_SEC` 覆盖
- 默认输出为十六进制，格式为 `'h...`
- event 配置按 `Session + FSDB` 绑定；旧版 `.xwave.events` 记录缺少 FSDB 元数据，不会被自动复用，重新执行 `xwave event <json> -n <name>` 即可迁移
