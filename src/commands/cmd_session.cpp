#include "cmd_session.h"
#include "../session/session_manager.h"
#include "../client/client.h"
#include "../protocol/protocol.h"
#include "../json.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <fstream>
#include <sstream>

namespace xwave {

void print_help(const char* prog) {
    printf("XWave - NPI-based FSDB Waveform Query Tool\n\n");
    printf("Usage:\n");
    printf("  %s open <fsdb-file> [--debug]  Open FSDB and create new session\n", prog);
    printf("  %s session list             List all active sessions\n", prog);
    printf("  %s session kill <id|all> [--debug]  Kill a specific session or all\n", prog);
    printf("  %s session gc [--debug]     Clean stale and idle sessions\n", prog);
    printf("  %s session doctor -s <sid> [-json] [--debug]  Diagnose a session\n", prog);
    printf("  %s value <sig> <time> [-b|-d] [-s <sid>]  Query signal value\n", prog);
    printf("  %s list new <name> [-s <sid>]             Create a signal list\n", prog);
    printf("  %s list add <sig> [-s <sid>] [-l <name>]  Add signal to list\n", prog);
    printf("  %s list del <sig|idx> [-s <sid>] [-l <name>]  Delete from list\n", prog);
    printf("  %s list show [-s <sid>] [-l <name>]       Show list contents\n", prog);
    printf("  %s list value <time> [-l <name>] [-b|-d] [-json] [-s <sid>]\n", prog);
    printf("  %s list diff [-l <name>] [-b T] [-e T] [-s <sid>]\n", prog);
    printf("  %s list validate [-l <name>] [-json] [-s <sid>]\n", prog);
    printf("  %s scope <path> [-recursive] [-json] [-s <sid>]\n", prog);
    printf("  %s apb <json> -n <name> [-s <sid>]        Load APB config\n", prog);
    printf("  %s apb list [-n <name>] [-s <sid>]        Show APB config\n", prog);
    printf("  %s apb wr [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]\n", prog);
    printf("  %s apb rd [-n <name>] [-addr <a>] [-num <x>] [-last] [-json] [-s <sid>]\n", prog);
    printf("  %s apb begin|next|pre|last [-rd|-wr] [-json] [-n <name>] [-s <sid>]\n", prog);
    printf("  %s axi <json> -n <name> [-s <sid>]        Load AXI config\n", prog);
    printf("  %s axi list [-n <name>] [-s <sid>]        Show AXI config\n", prog);
    printf("  %s axi wr [-n <name>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]\n", prog);
    printf("  %s axi rd [-n <name>] [-addr <a>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]\n", prog);
    printf("  %s axi begin|next|pre|last [-rd|-wr] [-json] [-n <name>] [-s <sid>]\n", prog);
    printf("  %s axi latency|osd [-rd|-wr|-all] [-id <id>] [-json] [-n <name>] [-s <sid>]\n", prog);
    printf("  %s event <json> -n <name> [-s <sid>]     Load generic event config\n", prog);
    printf("  %s event list [-n <name>] [-s <sid>]     Show event configs\n", prog);
    printf("  %s event find -n <name> -expr <expr> [-b T] [-e T] [-context T [-axi <name>] [-apb <name>]] [-json] [-s <sid>]\n", prog);
    printf("  %s event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-context T [-axi <name>] [-apb <name>]] [-json] [-s <sid>]\n", prog);
    printf("  %s ai query <json|-|--json JSON>  Run AI-oriented JSON request\n", prog);
    printf("  %s ai schema|actions          Show AI JSON schema or supported actions\n", prog);
    printf("  %s help [topic]             Show this help or detailed topic help\n", prog);
    printf("\nExamples:\n");
    printf("  %s open waves.fsdb\n", prog);
    printf("  %s value top.clk 100ns -b\n", prog);
    printf("  %s session list\n", prog);
    printf("  %s session kill 1\n", prog);
    printf("\nDetailed help topics:\n");
    printf("  %s help open|session|value|list|scope|apb|axi|event|ai\n", prog);
    printf("\nTime arguments accept us/ns/ps/fs suffixes. Default unit is ns.\n");
}

static void print_open_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s open <fsdb-file> [--debug]\n\n", prog);
    printf("Description:\n");
    printf("  Open an FSDB waveform and create or reuse a daemon session.\n");
    printf("  The FSDB path is canonicalized; if a healthy session already owns the same\n");
    printf("  file, xwave reuses it instead of opening another daemon.\n\n");
    printf("Arguments:\n");
    printf("  <fsdb-file>  FSDB file path. Relative paths are resolved to canonical paths.\n\n");
    printf("Options:\n");
    printf("  --debug      Print session creation diagnostics to stderr. Also enabled by XWAVE_DEBUG=1.\n");
    printf("               Server startup diagnostics are written to ~/.xwave.<sid>.debug.log.\n\n");
    printf("Session behavior:\n");
    printf("  xwave records mtime, size, device, and inode. Later session commands compare\n");
    printf("  this fingerprint; if the file changed, the daemon is restarted in place.\n");
    printf("  XWAVE_SESSION_START_TIMEOUT_SEC controls server startup wait time; default is 60 seconds.\n");
}

static void print_session_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s session list\n", prog);
    printf("  %s session doctor -s <sid> [-json] [--debug]\n", prog);
    printf("  %s session gc [--debug]\n", prog);
    printf("  %s session kill <id|all> [--debug]\n\n", prog);
    printf("Subcommands:\n");
    printf("  list              Show session ID, PID, RSS, created time, last active time, and FSDB path.\n");
    printf("  doctor            Check registry, FSDB fingerprint, process, socket, and PING/PONG health.\n");
    printf("  gc                Remove stale sessions and sessions idle longer than the timeout.\n");
    printf("  kill <id|all>     Stop one daemon or all daemons and remove related session records.\n\n");
    printf("Options:\n");
    printf("  -s <sid>          Session ID to diagnose. Required by session doctor.\n");
    printf("  -json             Print doctor output as JSON.\n");
    printf("  --debug           Print session lifecycle diagnostics to stderr. Also enabled by XWAVE_DEBUG=1.\n\n");
    printf("Environment:\n");
    printf("  XWAVE_IDLE_TIMEOUT_SEC overrides the default 1800-second idle timeout used by gc.\n");
    printf("  XWAVE_SESSION_START_TIMEOUT_SEC overrides the default 60-second startup wait.\n");
    printf("  XWAVE_DEBUG=1 enables debug diagnostics for session commands.\n");
}

static void print_value_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s value <signal> <time> [-b|-d] [-s <sid>]\n\n", prog);
    printf("Arguments:\n");
    printf("  <signal>          Full FSDB signal path.\n");
    printf("  <time>            Query time. Supports us/ns/ps/fs suffixes; default is ns.\n\n");
    printf("Options:\n");
    printf("  -b                Print binary value.\n");
    printf("  -d                Print decimal value.\n");
    printf("  -s <sid>          Use this session. If omitted, the latest session is used.\n\n");
    printf("Default output radix is hexadecimal.\n");
}

static void print_list_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s list new <name> [-s <sid>]\n", prog);
    printf("  %s list add <sig> [-s <sid>] [-l <name>]\n", prog);
    printf("  %s list del <sig|idx> [-s <sid>] [-l <name>]\n", prog);
    printf("  %s list show [-s <sid>] [-l <name>]\n", prog);
    printf("  %s list value <time> [-l <name>] [-b|-d] [-json] [-s <sid>]\n", prog);
    printf("  %s list validate [-l <name>] [-json] [-s <sid>]\n", prog);
    printf("  %s list diff [-l <name>] [-b T] [-e T] [-s <sid>]\n\n", prog);
    printf("Subcommands:\n");
    printf("  new <name>        Create a named signal list for a session.\n");
    printf("  add <sig>         Add a signal after checking it exists in the current FSDB.\n");
    printf("  del <sig|idx>     Delete by signal path or 1-based index from list show.\n");
    printf("  show              Print list contents with 1-based indices.\n");
    printf("  value <time>      Query all list signals at one time.\n");
    printf("  validate          Check whether every list signal still exists in the FSDB.\n");
    printf("  diff              Find the earliest time where at least two list signals differ.\n\n");
    printf("Options:\n");
    printf("  -l <name>         Select a list. If omitted, the most recently modified list is used.\n");
    printf("  -s <sid>          Select a session. If omitted, the latest session is used.\n");
    printf("  -b                Binary output for list value.\n");
    printf("  -d                Decimal output for list value.\n");
    printf("  -json             JSON output for value or validate.\n");
    printf("  -b T              Begin time for diff. Default is 0.\n");
    printf("  -e T              End time for diff. Default is waveform end.\n\n");
    printf("Notes:\n");
    printf("  list value prints NOT_FOUND and exits non-zero if an old list contains a missing signal.\n");
    printf("  list diff requires at least two signals.\n");
}

static void print_scope_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s scope <path> [-recursive] [-json] [-s <sid>]\n\n", prog);
    printf("Arguments:\n");
    printf("  <path>            FSDB scope path to inspect.\n\n");
    printf("Options:\n");
    printf("  -recursive        Include signals under child scopes recursively.\n");
    printf("  -json             Print structured JSON output.\n");
    printf("  -s <sid>          Select a session. If omitted, the latest session is used.\n\n");
    printf("Use scope to discover real dumped signal names, especially generated scopes or arrays.\n");
}

static void print_apb_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s apb <json-file> -n <name> [-s <sid>]\n", prog);
    printf("  %s apb list [-n <name>] [-s <sid>]\n", prog);
    printf("  %s apb wr|rd [-n <name>] [-addr <addr>] [-num <x>] [-last] [-json] [-s <sid>]\n", prog);
    printf("  %s apb begin|next|pre|last [-rd|-wr] [-json] [-n <name>] [-s <sid>]\n\n", prog);
    printf("Config JSON fields:\n");
    printf("  Required: paddr, pwdata, prdata, pwrite, penable, psel, clk, rst_n.\n");
    printf("  Optional: edge, with posedge as default. Use negedge for falling-edge sampling.\n\n");
    printf("Subcommands:\n");
    printf("  <json-file>       Load and persist an APB config under -n <name>.\n");
    printf("  list              Show a named config, or the latest APB config if -n is omitted.\n");
    printf("  wr|rd             Count or select write/read transactions.\n");
    printf("  begin             Move cursor to the first matching transaction.\n");
    printf("  next              Move cursor to the next matching transaction.\n");
    printf("  pre               Move cursor to the previous matching transaction.\n");
    printf("  last              Move cursor to the last matching transaction.\n\n");
    printf("Options:\n");
    printf("  -n <name>         Config name. Required when loading; latest config is used for queries if omitted.\n");
    printf("  -s <sid>          Select a session. If omitted, the latest session is used.\n");
    printf("  -addr <addr>      Filter transactions by address. Hex and decimal are accepted.\n");
    printf("  -num <x>          Select the x-th matching transaction, 1-based.\n");
    printf("  -last             Select the last matching transaction.\n");
    printf("  -rd               Cursor direction filter: read transactions only.\n");
    printf("  -wr               Cursor direction filter: write transactions only.\n");
    printf("  -json             Print JSON output.\n");
}

static void print_axi_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s axi <json-file> -n <name> [-s <sid>]\n", prog);
    printf("  %s axi list [-n <name>] [-s <sid>]\n", prog);
    printf("  %s axi wr|rd [-n <name>] [-addr <addr>] [-id <id>] [-num <x>] [-last] [-json] [-s <sid>]\n", prog);
    printf("  %s axi begin|next|pre|last [-rd|-wr] [-json] [-n <name>] [-s <sid>]\n", prog);
    printf("  %s axi latency|osd [-rd|-wr|-all] [-id <id>] [-json] [-n <name>] [-s <sid>]\n\n", prog);
    printf("Config JSON fields:\n");
    printf("  Required: awaddr, awid, awlen, awsize, awburst, awvalid, awready,\n");
    printf("            wdata, wstrb, wlast, wvalid, wready, bid, bresp, bvalid, bready,\n");
    printf("            araddr, arid, arlen, arsize, arburst, arvalid, arready,\n");
    printf("            rid, rdata, rresp, rlast, rvalid, rready, clk, rst_n.\n");
    printf("  Optional: edge, with posedge as default. Use negedge for falling-edge sampling.\n\n");
    printf("Subcommands:\n");
    printf("  <json-file>       Load and persist an AXI config under -n <name>.\n");
    printf("  list              Show a named config, or the latest AXI config if -n is omitted.\n");
    printf("  wr|rd             Count or select write/read transactions.\n");
    printf("  begin|next|pre|last  Cursor navigation through transactions.\n");
    printf("  latency           Report read/write latency statistics.\n");
    printf("  osd               Report outstanding depth statistics.\n\n");
    printf("Options:\n");
    printf("  -n <name>         Config name. Required when loading; latest config is used for queries if omitted.\n");
    printf("  -s <sid>          Select a session. If omitted, the latest session is used.\n");
    printf("  -addr <addr>      Filter transactions by address. Hex and decimal are accepted.\n");
    printf("  -id <id>          Filter by AXI ID. Hex and decimal are accepted.\n");
    printf("  -num <x>          Select the x-th matching transaction, 1-based.\n");
    printf("  -last             Select the last matching transaction.\n");
    printf("  -rd               Filter read transactions.\n");
    printf("  -wr               Filter write transactions.\n");
    printf("  -all              Include read and write transactions for latency/osd. This is the default.\n");
    printf("  -json             Print JSON output.\n");
}

static void print_event_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s event <json-file> -n <name> [-s <sid>]\n", prog);
    printf("  %s event list [-n <name>] [-s <sid>]\n", prog);
    printf("  %s event find -n <name> -expr <expr> [-b T] [-e T] [-context T [-axi <name>] [-apb <name>]] [-json] [-s <sid>]\n", prog);
    printf("  %s event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-context T [-axi <name>] [-apb <name>]] [-json] [-s <sid>]\n\n", prog);
    printf("Config JSON fields:\n");
    printf("  Required: clk and signals. signals maps aliases to full FSDB paths.\n");
    printf("  Optional: rst_n, edge, fields. edge must be posedge or negedge.\n");
    printf("  fields can be \"alias[high:low]\" strings or objects with signal/left/right.\n\n");
    printf("Subcommands:\n");
    printf("  <json-file>       Load and persist a generic event config under -n <name>.\n");
    printf("  list              List event config names, or print one config with -n.\n");
    printf("  find              Print the first clock edge where the expression is true.\n");
    printf("  export            Print matching events. Default limit is 1000 rows.\n\n");
    printf("Options:\n");
    printf("  -n <name>         Event config name. Required for load/find/export; latest config is used by list if omitted.\n");
    printf("  -s <sid>          Select a session. If omitted, the latest session is used.\n");
    printf("  -expr <expr>      Boolean expression over signal aliases and field aliases.\n");
    printf("  -b T              Begin time. Default is 0.\n");
    printf("  -e T              End time. Default is waveform end.\n");
    printf("  -limit N          Max export rows. Default is 1000; non-positive means no limit.\n");
    printf("  -context T        Attach protocol context in [event_time - T, event_time + T].\n");
    printf("  -axi <name>       Include AXI transactions from this AXI config. Requires -context.\n");
    printf("  -apb <name>       Include APB transactions from this APB config. Requires -context.\n");
    printf("  -json             Print JSON output.\n\n");
    printf("Expression syntax:\n");
    printf("  Supports aliases, fields, !, &&, ||, ==, !=, parentheses, and constants such as 0xff or 0b1010.\n");
    printf("  Comparisons containing x/z evaluate to unknown and do not match events.\n");
}

static void print_ai_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s ai query <request.json>\n", prog);
    printf("  %s ai query -\n", prog);
    printf("  %s ai query --json '<json>'\n", prog);
    printf("  %s ai schema\n", prog);
    printf("  %s ai actions\n\n", prog);
    printf("Description:\n");
    printf("  AI-oriented JSON API for scriptable waveform facts. Existing human CLI\n");
    printf("  commands are unchanged; this entry wraps session, scope, value, list,\n");
    printf("  APB, AXI, event, waveform verification, signal inspection, handshake,\n");
    printf("  and protocol fact actions in a stable envelope.\n\n");
    printf("Request envelope:\n");
    printf("  api_version: \"xwave.ai.v1\"\n");
    printf("  action:      e.g. value.at, event.find, window.verify, handshake.inspect\n");
    printf("  target:      {\"fsdb\": \"waves.fsdb\", \"auto_open\": true} or {\"session_id\": 1}\n");
    printf("  args:        action-specific arguments\n");
    printf("  limits:      max_rows/max_events/max_samples/timeout_ms where applicable\n\n");
    printf("Example:\n");
    printf("  %s ai query --json '{\"api_version\":\"xwave.ai.v1\",\"action\":\"value.at\",\"target\":{\"fsdb\":\"waves.fsdb\",\"auto_open\":true},\"args\":{\"signal\":\"top.clk\",\"time\":\"10ns\"}}'\n", prog);
    printf("\nImplemented waveform-fact actions:\n");
    printf("  verify.conditions, expr.eval_at, window.verify, signal.changes,\n");
    printf("  signal.stability, signal.trend, inspect_signal, detect_anomaly,\n");
    printf("  handshake.inspect, axi.channel_stall, axi.outstanding_timeline,\n");
    printf("  axi.request_response_pair, axi.latency_outlier, apb.transfer_window\n");
}

void print_help_topic(const char* prog, const char* topic) {
    if (!topic || strcmp(topic, "all") == 0) {
        print_help(prog);
        return;
    }
    if (strcmp(topic, "open") == 0) print_open_help(prog);
    else if (strcmp(topic, "session") == 0) print_session_help(prog);
    else if (strcmp(topic, "value") == 0) print_value_help(prog);
    else if (strcmp(topic, "list") == 0) print_list_help(prog);
    else if (strcmp(topic, "scope") == 0) print_scope_help(prog);
    else if (strcmp(topic, "apb") == 0) print_apb_help(prog);
    else if (strcmp(topic, "axi") == 0) print_axi_help(prog);
    else if (strcmp(topic, "event") == 0) print_event_help(prog);
    else if (strcmp(topic, "ai") == 0) print_ai_help(prog);
    else {
        fprintf(stderr, "Unknown help topic: %s\n\n", topic);
        print_help(prog);
    }
}

static std::string format_epoch(time_t t) {
    if (t <= 0) return "-";
    char buf[64];
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static long read_rss_kb(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream ifs(path);
    std::string key;
    while (ifs >> key) {
        if (key == "VmRSS:") {
            long kb = 0;
            ifs >> kb;
            return kb;
        }
        std::string rest;
        std::getline(ifs, rest);
    }
    return -1;
}

int cmd_open(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s open <fsdb-file>\n\n", argv[0]);
        print_help(argv[0]);
        return 1;
    }

    const char* fsdb_file = argv[2];
    SessionManager manager;
    int session_id = manager.create_session(fsdb_file);

    if (session_id <= 0) {
        fprintf(stderr, "Error: Failed to create session\n");
        return 1;
    }

    printf("[Session %d] FSDB opened: %s\n", session_id, fsdb_file);
    return 0;
}

int cmd_session_list() {
    SessionManager manager;
    std::vector<SessionInfo> sessions = manager.list_sessions();

    if (sessions.empty()) {
        printf("No active sessions.\n");
        return 0;
    }

    printf("ID  | PID     | RSS(KB) | Created             | Last Active         | FSDB File\n");
    printf("----|---------|---------|---------------------|---------------------|------------------------------\n");

    for (const auto& s : sessions) {
        long rss = read_rss_kb(s.server_pid);
        printf("%-3d | %-7d | %-7ld | %-19s | %-19s | %s\n",
               s.session_id,
               s.server_pid,
               rss,
               format_epoch(s.created_at).c_str(),
               format_epoch(s.last_active).c_str(),
               s.fsdb_file.c_str());
    }

    printf("\nTotal: %zu session(s)\n", sessions.size());
    return 0;
}

int cmd_session_gc() {
    SessionManager manager;
    manager.gc_sessions();
    printf("Session GC completed.\n");
    return 0;
}

int cmd_session_kill(const char* id_str) {
    if (strcmp(id_str, "all") == 0) {
        SessionManager manager;
        printf("Killing all sessions...\n");
        manager.kill_all_sessions();
        printf("All sessions killed.\n");
        return 0;
    }

    int session_id = atoi(id_str);
    if (session_id <= 0) {
        fprintf(stderr, "Error: Invalid session ID: %s\n", id_str);
        return 1;
    }

    SessionManager manager;
    SessionHealth health = manager.diagnose_session(session_id);
    if (health.status == SessionHealthStatus::RegistryMissing) {
        fprintf(stderr, "Error: Session %d is not in registry\n", session_id);
        return 1;
    }

    if (!health.healthy) {
        printf("Cleaning stale session %d (%s: %s)...\n",
               session_id,
               session_health_status_name(health.status),
               health.message.c_str());
    } else {
        printf("Killing session %d...\n", session_id);
    }

    if (manager.kill_session(session_id)) {
        printf("Session %d removed.\n", session_id);
        return 0;
    } else {
        fprintf(stderr, "Error: Failed to kill session %d\n", session_id);
        return 1;
    }
}

int cmd_session_doctor(int argc, char** argv) {
    int session_id = -1;
    bool json = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            // main() already enables XWAVE_DEBUG; accept the option here.
        } else {
            fprintf(stderr, "Usage: %s session doctor -s <sid> [-json] [--debug]\n", argv[0]);
            return 1;
        }
    }

    if (session_id <= 0) {
        fprintf(stderr, "Usage: %s session doctor -s <sid> [-json] [--debug]\n", argv[0]);
        fprintf(stderr, "Error: session doctor requires -s <sid>\n");
        return 1;
    }

    SessionManager manager;
    SessionHealth health = manager.diagnose_session(session_id);

    if (json) {
        nlohmann::ordered_json out;
        out["session_id"] = health.session_id;
        out["healthy"] = health.healthy;
        out["status"] = session_health_status_name(health.status);
        out["message"] = health.message;
        out["pid"] = health.info.server_pid;
        out["socket_path"] = health.info.socket_path;
        out["fsdb_file"] = health.info.fsdb_file;
        printf("%s\n", out.dump(2).c_str());
    } else {
        if (health.healthy) {
            printf("Session %d healthy\n", session_id);
            printf("  status: %s\n", session_health_status_name(health.status));
            printf("  pid: %d\n", health.info.server_pid);
            printf("  socket_path: %s\n", health.info.socket_path.c_str());
            printf("  fsdb_file: %s\n", health.info.fsdb_file.c_str());
        } else {
            printf("Session %d unhealthy\n", session_id);
            printf("  status: %s\n", session_health_status_name(health.status));
            printf("  message: %s\n", health.message.c_str());
            if (health.info.session_id > 0) {
                printf("  pid: %d\n", health.info.server_pid);
                printf("  socket_path: %s\n", health.info.socket_path.c_str());
                printf("  fsdb_file: %s\n", health.info.fsdb_file.c_str());
            }
        }
    }

    return health.healthy ? 0 : 1;
}

} // namespace xwave
