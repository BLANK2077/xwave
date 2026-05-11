#include "cmd_scope.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace xwave {

static int resolve_session_id(int sid) {
    SessionManager manager;
    SessionInfo info;
    bool ok = sid >= 0 ? manager.get_session(sid, info) : manager.get_latest_session(info);
    if (!ok) return -1;
    if (!manager.ensure_session_current(info.session_id)) return -1;
    return info.session_id;
}

int cmd_scope(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s scope <path> [-recursive] [-json] [-s <sid>]\n\n", argv[0]);
        print_help(argv[0]);
        return 1;
    }

    const char* scope_path = argv[2];
    int session_id = -1;
    bool recursive = false;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
        else if (strcmp(argv[i], "-recursive") == 0) recursive = true;
        else if (strcmp(argv[i], "-json") == 0) json = true;
        else {
            fprintf(stderr, "Usage: %s scope <path> [-recursive] [-json] [-s <sid>]\n", argv[0]);
            return 1;
        }
    }

    session_id = resolve_session_id(session_id);
    if (session_id < 0) {
        fprintf(stderr, "Error: No active sessions\n");
        return 1;
    }

    std::string cmd = std::string(CMD_SCOPE) + " " + scope_path + " " + (recursive ? "1" : "0");
    cmd += json ? " json" : " text";
    if (!send_command_and_print(session_id, cmd.c_str())) return 1;
    return 0;
}

} // namespace xwave
