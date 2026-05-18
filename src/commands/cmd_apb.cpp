#include "cmd_apb.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../session/session_manager.h"
#include "../protocol/protocol.h"
#include "../apb/apb_manager.h"
#include "../json.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdint>

namespace xwave {

static bool resolve_session_id(const std::string& sid, std::string& out_sid) {
    SessionManager manager;
    SessionInfo info;
    if (!sid.empty()) {
        if (!manager.get_session(sid, info)) return false;
    } else if (!manager.get_latest_session(info)) {
        return false;
    }
    if (!manager.ensure_session_current(info.session_id)) return false;
    out_sid = info.session_id;
    return true;
}

static bool resolve_apb_name(ApbManager& am, const std::string& session_id, const char* explicit_name, std::string& out_name) {
    if (explicit_name) {
        out_name = explicit_name;
        return true;
    }
    if (!am.get_latest_apb(session_id, out_name)) {
        fprintf(stderr, "Error: No APB configs found for session %s\n", session_id.c_str());
        return false;
    }
    return true;
}

static bool read_file_to_string(const char* path, std::string& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return true;
}

static bool load_json_config(const char* json_path, ApbConfig& config) {
    std::string text;
    if (!read_file_to_string(json_path, text)) {
        fprintf(stderr, "Error: Cannot read %s\n", json_path);
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: Failed to parse JSON in %s\n", json_path);
        return false;
    }
    if (!j.is_object()) {
        fprintf(stderr, "Error: JSON root must be an object in %s\n", json_path);
        return false;
    }

    auto get = [&j](const char* key) -> std::string {
        auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return "";
        return it->get<std::string>();
    };

    config.paddr   = get("paddr");
    config.pwdata  = get("pwdata");
    config.prdata  = get("prdata");
    config.pwrite  = get("pwrite");
    config.penable = get("penable");
    config.psel    = get("psel");
    config.clk     = get("clk");
    config.rst_n   = get("rst_n");
    const std::string edge = get("edge");
    config.posedge = (edge.empty() || edge == "posedge");

    if (config.paddr.empty() || config.pwdata.empty() || config.prdata.empty() ||
        config.pwrite.empty() || config.penable.empty() || config.psel.empty() ||
        config.clk.empty() || config.rst_n.empty()) {
        fprintf(stderr, "Error: Missing required field in APB JSON\n");
        return false;
    }
    return true;
}

static void print_apb_config(const ApbConfig& config) {
    printf("{\n");
    printf("  \"paddr\": \"%s\",\n", config.paddr.c_str());
    printf("  \"pwdata\": \"%s\",\n", config.pwdata.c_str());
    printf("  \"prdata\": \"%s\",\n", config.prdata.c_str());
    printf("  \"pwrite\": \"%s\",\n", config.pwrite.c_str());
    printf("  \"penable\": \"%s\",\n", config.penable.c_str());
    printf("  \"psel\": \"%s\",\n", config.psel.c_str());
    printf("  \"clk\": \"%s\",\n", config.clk.c_str());
    printf("  \"rst_n\": \"%s\",\n", config.rst_n.c_str());
    printf("  \"edge\": \"%s\"\n", config.posedge ? "posedge" : "negedge");
    printf("}\n");
}

int cmd_apb(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s apb <json-file> -n <name> [-s <sid>]\n", argv[0]);
        fprintf(stderr, "       %s apb list [-n <name>] [-s <sid>]\n", argv[0]);
        fprintf(stderr, "       %s apb wr [-n <name>] [-s <sid>] [-addr <addr>] [-num <x>] [-last] [-json]\n", argv[0]);
        fprintf(stderr, "       %s apb rd [-n <name>] [-s <sid>] [-addr <addr>] [-num <x>] [-last] [-json]\n", argv[0]);
        fprintf(stderr, "       %s apb begin [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s apb next [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s apb pre [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s apb last [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        return 1;
    }

    const char* subcmd_or_file = argv[2];

    // --- Load: xwave apb <json-file> -n <name> [-s <sid>] ---
    if (strcmp(subcmd_or_file, "wr") != 0 &&
        strcmp(subcmd_or_file, "rd") != 0 &&
        strcmp(subcmd_or_file, "begin") != 0 &&
        strcmp(subcmd_or_file, "next") != 0 &&
        strcmp(subcmd_or_file, "pre") != 0 &&
        strcmp(subcmd_or_file, "last") != 0 &&
        strcmp(subcmd_or_file, "list") != 0) {
        const char* json_file = subcmd_or_file;
        std::string session_id;
        const char* name = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = argv[++i];
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        }
        if (!name) {
            fprintf(stderr, "Error: -n <name> is required for loading APB config\n");
            return 1;
        }
        if (!resolve_session_id(session_id, session_id)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ApbConfig config;
        if (!load_json_config(json_file, config)) return 1;
        config.name = name;
        ApbManager am;
        if (!am.create_apb(session_id, config)) {
            fprintf(stderr, "Error: Failed to create APB config '%s'\n", name);
            return 1;
        }
        printf("APB config '%s' loaded for session %s.\n", name, session_id.c_str());
        return 0;
    }

    // --- list ---
    if (strcmp(subcmd_or_file, "list") == 0) {
        std::string session_id;
        const char* name = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = argv[++i];
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        }
        if (!resolve_session_id(session_id, session_id)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        ApbManager am;
        std::string apb_name;
        if (!resolve_apb_name(am, session_id, name, apb_name)) return 1;
        ApbConfig config;
        if (!am.get_apb(session_id, apb_name, config)) {
            fprintf(stderr, "Error: APB config '%s' not found\n", apb_name.c_str());
            return 1;
        }
        print_apb_config(config);
        return 0;
    }

    // Common query parsing
    const char* subcmd = subcmd_or_file;
    std::string session_id;
    const char* name = nullptr;
    const char* addr_str = nullptr;
    int num_val = -1;
    bool has_last = false;
    bool json = false;
    bool filter_rd = false;
    bool filter_wr = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "-addr") == 0 && i + 1 < argc) addr_str = argv[++i];
        else if (strcmp(argv[i], "-num") == 0 && i + 1 < argc) num_val = atoi(argv[++i]);
        else if (strcmp(argv[i], "-last") == 0) has_last = true;
        else if (strcmp(argv[i], "-json") == 0) json = true;
        else if (strcmp(argv[i], "-rd") == 0) filter_rd = true;
        else if (strcmp(argv[i], "-wr") == 0) filter_wr = true;
    }

    if (!resolve_session_id(session_id, session_id)) {
        fprintf(stderr, "Error: No active sessions\n");
        return 1;
    }
    ApbManager am;
    std::string apb_name;
    if (!resolve_apb_name(am, session_id, name, apb_name)) return 1;

    std::string protocol_cmd;

    if (strcmp(subcmd, "wr") == 0 || strcmp(subcmd, "rd") == 0) {
        protocol_cmd = std::string(strcmp(subcmd, "wr") == 0 ? CMD_APB_WR : CMD_APB_RD);
        protocol_cmd += " " + apb_name;
        if (addr_str) {
            protocol_cmd += " addr ";
            protocol_cmd += addr_str;
        }
        if (num_val > 0) {
            protocol_cmd += " num ";
            protocol_cmd += std::to_string(num_val);
        }
        if (has_last) {
            protocol_cmd += " last";
        }
        if (json) protocol_cmd += " json";
    } else {
        const char* cmd_const = nullptr;
        if (strcmp(subcmd, "begin") == 0) cmd_const = CMD_APB_BEGIN;
        else if (strcmp(subcmd, "next") == 0) cmd_const = CMD_APB_NEXT;
        else if (strcmp(subcmd, "pre") == 0) cmd_const = CMD_APB_PREV;
        else if (strcmp(subcmd, "last") == 0) cmd_const = CMD_APB_LAST;
        else {
            fprintf(stderr, "Unknown apb subcommand: %s\n", subcmd);
            return 1;
        }
        protocol_cmd = std::string(cmd_const) + " " + apb_name;
        if (filter_wr) protocol_cmd += " wr";
        else if (filter_rd) protocol_cmd += " rd";
        else protocol_cmd += " all";
        if (json) protocol_cmd += " json";
    }

    if (!send_command_and_print(session_id, protocol_cmd.c_str())) {
        return 1;
    }
    return 0;
}

} // namespace xwave
