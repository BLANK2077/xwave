#include "cmd_value.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../session/session_manager.h"
#include "../protocol/protocol.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace xwave {

int cmd_value(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s value <signal> <time_spec> [-b|-d] [-s <sid>]\n", argv[0]);
        fprintf(stderr, "       %s value <signal> --at <time_spec> [-b|-d] [-s <sid>]\n\n", argv[0]);
        print_help(argv[0]);
        return 1;
    }

    const char* signal = argv[2];
    const char* time_str = argv[3];
    char fmt = 'H';
    std::string session_id;

    int start = 4;
    if (strcmp(argv[3], "--at") == 0 && argc >= 5) {
        time_str = argv[4];
        start = 5;
    }

    for (int i = start; i < argc; ++i) {
        if (strcmp(argv[i], "-b") == 0) fmt = 'B';
        else if (strcmp(argv[i], "-d") == 0) fmt = 'D';
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            session_id = argv[++i];
        }
    }

    if (session_id.empty()) {
        SessionManager manager;
        SessionInfo info;
        if (!manager.get_latest_session(info)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        session_id = info.session_id;
    }
    {
        SessionManager manager;
        if (!manager.ensure_session_current(session_id)) {
            fprintf(stderr, "Error: Session %s unavailable\n", session_id.c_str());
            return 1;
        }
    }

    std::string cmd = std::string(CMD_VALUE) + " " + signal + " " + time_str + " " + fmt;
    if (!send_command_and_print(session_id, cmd.c_str())) {
        return 1;
    }
    return 0;
}

} // namespace xwave
