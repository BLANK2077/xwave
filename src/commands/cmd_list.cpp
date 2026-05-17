#include "cmd_list.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../session/session_manager.h"
#include "../protocol/protocol.h"
#include "../list/list_manager.h"
#include <cstdio>
#include <cstring>

namespace xwave {

static int resolve_session_id(int sid) {
    SessionManager manager;
    SessionInfo info;
    if (sid >= 0) {
        if (!manager.get_session(sid, info)) return -1;
    } else if (!manager.get_latest_session(info)) {
        return -1;
    }
    if (!manager.ensure_session_current(info.session_id)) return -1;
    return info.session_id;
}

static bool resolve_list_name(ListManager& lm, int session_id, const char* explicit_name, std::string& out_name) {
    if (explicit_name) {
        out_name = explicit_name;
        return true;
    }
    if (!lm.get_latest_list(session_id, out_name)) {
        fprintf(stderr, "Error: No lists found for session %d\n", session_id);
        return false;
    }
    return true;
}

int cmd_list(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s list <new|add|del|show|value|diff|validate> ...\n\n", argv[0]);
        print_help(argv[0]);
        return 1;
    }

    const char* subcmd = argv[2];

    // --- list new <name> [-s <sid>] ---
    if (strcmp(subcmd, "new") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s list new <name> [-s <sid>]\n\n", argv[0]);
            print_help(argv[0]);
            return 1;
        }
        int session_id = -1;
        for (int i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
                session_id = atoi(argv[++i]);
            }
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        if (!lm.create_list(session_id, argv[3])) {
            fprintf(stderr, "Error: Failed to create list '%s'\n", argv[3]);
            return 1;
        }
        printf("List '%s' created for session %d.\n", argv[3], session_id);
        return 0;
    }

    // --- list show [-s <sid>] [-l <name>] ---
    if (strcmp(subcmd, "show") == 0) {
        int session_id = -1;
        const char* list_name = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) list_name = argv[++i];
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        std::string name;
        if (!resolve_list_name(lm, session_id, list_name, name)) return 1;

        SignalList list;
        if (!lm.get_list(session_id, name, list)) {
            fprintf(stderr, "Error: List '%s' not found\n", name.c_str());
            return 1;
        }
        for (size_t i = 0; i < list.signals.size(); ++i) {
            printf("%zu: %s\n", i + 1, list.signals[i].c_str());
        }
        return 0;
    }

    // --- list add [-s <sid>] [-l <name>] <sig> ---
    if (strcmp(subcmd, "add") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s list add [-s <sid>] [-l <name>] <sig>\n\n", argv[0]);
            print_help(argv[0]);
            return 1;
        }
        int session_id = -1;
        const char* list_name = nullptr;
        const char* signal = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) list_name = argv[++i];
            else if (!signal && argv[i][0] != '-') signal = argv[i];
        }
        if (!signal) {
            fprintf(stderr, "Error: Missing signal argument\n");
            return 1;
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        std::string name;
        if (!resolve_list_name(lm, session_id, list_name, name)) return 1;
        std::string check_payload;
        std::string check_cmd = std::string(CMD_SIGNAL_CHECK) + " " + signal;
        if (!send_command_capture(session_id, check_cmd.c_str(), check_payload)) {
            fprintf(stderr, "Error: Signal not found: %s\n", signal);
            return 1;
        }
        if (!lm.add_signal(session_id, name, signal)) {
            fprintf(stderr, "Error: Failed to add signal to list '%s'\n", name.c_str());
            return 1;
        }
        printf("Added '%s' to list '%s'.\n", signal, name.c_str());
        return 0;
    }

    // --- list validate [-l <name>] [-json] [-s <sid>] ---
    if (strcmp(subcmd, "validate") == 0) {
        int session_id = -1;
        const char* list_name = nullptr;
        bool json = false;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) list_name = argv[++i];
            else if (strcmp(argv[i], "-json") == 0) json = true;
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        std::string name;
        if (!resolve_list_name(lm, session_id, list_name, name)) return 1;
        std::string cmd = std::string(CMD_LIST_VALIDATE) + " " + name;
        if (json) cmd += " json";
        if (!send_command_and_print(session_id, cmd.c_str())) return 1;
        return 0;
    }

    // --- list del [-s <sid>] [-l <name>] <sig|idx> ---
    if (strcmp(subcmd, "del") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s list del [-s <sid>] [-l <name>] <sig|idx>\n\n", argv[0]);
            print_help(argv[0]);
            return 1;
        }
        int session_id = -1;
        const char* list_name = nullptr;
        const char* target = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) list_name = argv[++i];
            else if (!target && argv[i][0] != '-') target = argv[i];
        }
        if (!target) {
            fprintf(stderr, "Error: Missing signal or index argument\n");
            return 1;
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        std::string name;
        if (!resolve_list_name(lm, session_id, list_name, name)) return 1;
        if (!lm.del_signal(session_id, name, target)) {
            fprintf(stderr, "Error: Failed to delete '%s' from list '%s'\n", target, name.c_str());
            return 1;
        }
        printf("Deleted '%s' from list '%s'.\n", target, name.c_str());
        return 0;
    }

    // --- list value <time_spec> [-l <name>] [-b|-d] [-json] [-s <sid>] ---
    if (strcmp(subcmd, "value") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s list value <time_spec> [-l <name>] [-b|-d] [-json] [-s <sid>]\n", argv[0]);
            fprintf(stderr, "       %s list value --at <time_spec> [-l <name>] [-b|-d] [-json] [-s <sid>]\n\n", argv[0]);
            print_help(argv[0]);
            return 1;
        }
        const char* time_str = argv[3];
        int session_id = -1;
        const char* list_name = nullptr;
        char fmt = 'H';
        bool json = false;
        int start = 4;
        if (strcmp(argv[3], "--at") == 0 && argc >= 5) {
            time_str = argv[4];
            start = 5;
        }
        for (int i = start; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) list_name = argv[++i];
            else if (strcmp(argv[i], "-b") == 0) fmt = 'B';
            else if (strcmp(argv[i], "-d") == 0) fmt = 'D';
            else if (strcmp(argv[i], "-json") == 0) json = true;
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        std::string name;
        if (!resolve_list_name(lm, session_id, list_name, name)) return 1;

        std::string cmd = std::string(CMD_LIST_VALUE) + " " + name + " " + time_str + " " + fmt;
        if (json) cmd += " json";
        if (!send_command_and_print(session_id, cmd.c_str())) {
            return 1;
        }
        return 0;
    }

    // --- list diff [-l <name>] [-b T] [-e T] [-s <sid>] ---
    if (strcmp(subcmd, "diff") == 0) {
        int session_id = -1;
        const char* list_name = nullptr;
        const char* begin_time_str = nullptr;
        const char* end_time_str = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) list_name = argv[++i];
            else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) begin_time_str = argv[++i];
            else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) end_time_str = argv[++i];
        }
        session_id = resolve_session_id(session_id);
        if (session_id < 0) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ListManager lm;
        std::string name;
        if (!resolve_list_name(lm, session_id, list_name, name)) return 1;

        std::string cmd = std::string(CMD_LIST_DIFF) + " " + name + " "
                        + (begin_time_str ? begin_time_str : "0") + " "
                        + (end_time_str ? end_time_str : "max");
        if (!send_command_and_print(session_id, cmd.c_str())) {
            return 1;
        }
        return 0;
    }

    fprintf(stderr, "Unknown list subcommand: %s\n\n", subcmd);
    print_help(argv[0]);
    return 1;
}

} // namespace xwave
