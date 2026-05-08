#include "commands/cmd_session.h"
#include "commands/cmd_value.h"
#include "commands/cmd_list.h"
#include "commands/cmd_apb.h"
#include "commands/cmd_axi.h"
#include "commands/cmd_event.h"
#include "server/server.h"
#include <cstring>
#include <cstdio>
#include <vector>

using namespace xwave;

int main(int argc, char** argv) {
    if (argc < 2) {
        print_help(argv[0]);
        return 0;
    }

    // Check for server mode first
    if (strcmp(argv[1], "--server") == 0) {
        // Strip "--server": pass argv[0], argv[2..] to server_main
        std::vector<char*> srv_argv;
        srv_argv.push_back(argv[0]);
        for (int i = 2; i < argc; i++)
            srv_argv.push_back(argv[i]);
        srv_argv.push_back(nullptr);
        return server_main((int)srv_argv.size() - 1, srv_argv.data());
    }

    const char* cmd = argv[1];

    // Help
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    // Open - create new session
    if (strcmp(cmd, "open") == 0) {
        return cmd_open(argc, argv);
    }

    // Session commands
    if (strcmp(cmd, "session") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s session <list|kill|doctor>\n", argv[0]);
            return 1;
        }

        const char* subcmd = argv[2];

        if (strcmp(subcmd, "list") == 0) {
            return cmd_session_list();
        }

        if (strcmp(subcmd, "kill") == 0) {
            if (argc < 4) {
                fprintf(stderr, "Usage: %s session kill <id|all>\n", argv[0]);
                return 1;
            }
            return cmd_session_kill(argv[3]);
        }

        if (strcmp(subcmd, "doctor") == 0) {
            return cmd_session_doctor(argc, argv);
        }

        fprintf(stderr, "Unknown session subcommand: %s\n\n", subcmd);
        print_help(argv[0]);
        return 1;
    }

    // Value query
    if (strcmp(cmd, "value") == 0) {
        return cmd_value(argc, argv);
    }

    // List commands
    if (strcmp(cmd, "list") == 0) {
        return cmd_list(argc, argv);
    }

    // APB commands
    if (strcmp(cmd, "apb") == 0) {
        return cmd_apb(argc, argv);
    }

    // AXI commands
    if (strcmp(cmd, "axi") == 0) {
        return cmd_axi(argc, argv);
    }

    // Generic event commands
    if (strcmp(cmd, "event") == 0) {
        return cmd_event(argc, argv);
    }

    // Unknown command
    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    print_help(argv[0]);
    return 1;
}
