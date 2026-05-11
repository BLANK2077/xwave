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
    printf("  %s open <fsdb-file>         Open FSDB and create new session\n", prog);
    printf("  %s session list             List all active sessions\n", prog);
    printf("  %s session kill <id|all>    Kill a specific session or all\n", prog);
    printf("  %s session gc               Clean stale and idle sessions\n", prog);
    printf("  %s session doctor -s <sid> [-json]  Diagnose a session\n", prog);
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
    printf("  %s event find -n <name> -expr <expr> [-b T] [-e T] [-json] [-s <sid>]\n", prog);
    printf("  %s event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-json] [-s <sid>]\n", prog);
    printf("  %s help                     Show this help\n", prog);
    printf("\nExamples:\n");
    printf("  %s open waves.fsdb\n", prog);
    printf("  %s value top.clk 100ns -b\n", prog);
    printf("  %s session list\n", prog);
    printf("  %s session kill 1\n", prog);
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
        } else {
            fprintf(stderr, "Usage: %s session doctor -s <sid> [-json]\n", argv[0]);
            return 1;
        }
    }

    if (session_id <= 0) {
        fprintf(stderr, "Usage: %s session doctor -s <sid> [-json]\n", argv[0]);
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
