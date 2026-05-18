#include "cmd_cursor.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../json.hpp"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace xwave {

using Json = nlohmann::ordered_json;

static bool resolve_session_id(const std::string& explicit_sid, std::string& sid) {
    SessionManager manager;
    SessionInfo info;
    bool ok = !explicit_sid.empty() ? manager.get_session(explicit_sid, info) : manager.get_latest_session(info);
    if (!ok) return false;
    if (!manager.ensure_session_current(info.session_id)) return false;
    sid = info.session_id;
    return true;
}

static bool send_ai_cursor_request(const std::string& session_id, const Json& req, Json& data, std::string& err) {
    std::string raw;
    std::string cmd = std::string(CMD_AI_QUERY) + " " + req.dump();
    if (!send_command_capture(session_id, cmd.c_str(), raw)) {
        const char* prefix = ERROR_PREFIX;
        if (raw.compare(0, strlen(prefix), prefix) == 0) err = raw.substr(strlen(prefix));
        else err = raw.empty() ? "cursor command failed" : raw;
        return false;
    }
    try {
        data = Json::parse(raw);
    } catch (...) {
        err = "failed to parse cursor response";
        return false;
    }
    return true;
}

static void print_cursor_text(const Json& cursor) {
    printf("%s %s", cursor.value("name", "").c_str(), cursor.value("time_text", "").c_str());
    if (cursor.contains("time")) printf(" time=%llu", cursor["time"].get<unsigned long long>());
    std::string note = cursor.value("note", "");
    if (!note.empty()) printf(" note=%s", note.c_str());
    printf("\n");
}

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s cursor set <name> <time_spec> [-note <text>] [-s <sid>]\n", prog);
    fprintf(stderr, "       %s cursor get <name> [-json] [-s <sid>]\n", prog);
    fprintf(stderr, "       %s cursor list [-json] [-s <sid>]\n", prog);
    fprintf(stderr, "       %s cursor delete <name> [-s <sid>]\n", prog);
    fprintf(stderr, "       %s cursor use <name> [-s <sid>]\n", prog);
}

int cmd_cursor(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    std::string subcmd = argv[2];
    std::string explicit_sid;
    bool json = false;
    std::string note;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) explicit_sid = argv[++i];
        else if (strcmp(argv[i], "-json") == 0) json = true;
        else if (strcmp(argv[i], "-note") == 0 && i + 1 < argc) note = argv[++i];
    }

    std::string sid;
    if (!resolve_session_id(explicit_sid, sid)) {
        fprintf(stderr, "Error: No active sessions\n");
        return 1;
    }

    Json req;
    req["api_version"] = "xwave.ai.v1";
    req["target"]["session_id"] = sid;

    if (subcmd == "set") {
        if (argc < 5) {
            usage(argv[0]);
            return 1;
        }
        req["action"] = "cursor.set";
        req["args"]["name"] = argv[3];
        req["args"]["time"] = argv[4];
        if (!note.empty()) req["args"]["note"] = note;
    } else if (subcmd == "get") {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }
        req["action"] = "cursor.get";
        req["args"]["name"] = argv[3];
    } else if (subcmd == "list") {
        req["action"] = "cursor.list";
    } else if (subcmd == "delete" || subcmd == "del" || subcmd == "rm") {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }
        req["action"] = "cursor.delete";
        req["args"]["name"] = argv[3];
    } else if (subcmd == "use") {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }
        req["action"] = "cursor.use";
        req["args"]["name"] = argv[3];
    } else {
        fprintf(stderr, "Unknown cursor subcommand: %s\n", subcmd.c_str());
        usage(argv[0]);
        return 1;
    }

    Json out;
    std::string err;
    if (!send_ai_cursor_request(sid, req, out, err)) {
        fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }
    if (json) {
        printf("%s\n", out.dump(2).c_str());
        return 0;
    }

    if (subcmd == "list") {
        Json cursors = out.value("cursors", Json::array());
        for (const auto& c : cursors) print_cursor_text(c);
        std::string active = out.value("active_cursor", "");
        if (!active.empty()) printf("active: %s\n", active.c_str());
    } else if (out.contains("cursor")) {
        print_cursor_text(out["cursor"]);
    } else {
        printf("%s\n", out.dump().c_str());
    }
    return 0;
}

} // namespace xwave
