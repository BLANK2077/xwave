#include "cmd_axi.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../session/session_manager.h"
#include "../protocol/protocol.h"
#include "../axi/axi_manager.h"
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

static bool resolve_axi_name(AxiManager& am, const std::string& session_id, const char* explicit_name, std::string& out_name) {
    if (explicit_name) {
        out_name = explicit_name;
        return true;
    }
    if (!am.get_latest_axi(session_id, out_name)) {
        fprintf(stderr, "Error: No AXI configs found for session %s\n", session_id.c_str());
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

static bool load_json_config(const char* json_path, AxiConfig& config) {
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

    config.awaddr   = get("awaddr");
    config.awid     = get("awid");
    config.awlen    = get("awlen");
    config.awsize   = get("awsize");
    config.awburst  = get("awburst");
    config.awvalid  = get("awvalid");
    config.awready  = get("awready");
    config.wdata    = get("wdata");
    config.wstrb    = get("wstrb");
    config.wlast    = get("wlast");
    config.wvalid   = get("wvalid");
    config.wready   = get("wready");
    config.bid      = get("bid");
    config.bresp    = get("bresp");
    config.bvalid   = get("bvalid");
    config.bready   = get("bready");
    config.araddr   = get("araddr");
    config.arid     = get("arid");
    config.arlen    = get("arlen");
    config.arsize   = get("arsize");
    config.arburst  = get("arburst");
    config.arvalid  = get("arvalid");
    config.arready  = get("arready");
    config.rid      = get("rid");
    config.rdata    = get("rdata");
    config.rresp    = get("rresp");
    config.rlast    = get("rlast");
    config.rvalid   = get("rvalid");
    config.rready   = get("rready");
    config.clk      = get("clk");
    config.rst_n    = get("rst_n");
    const std::string edge = get("edge");
    config.posedge = (edge.empty() || edge == "posedge");

    if (config.awaddr.empty() || config.awid.empty() || config.awlen.empty() ||
        config.awsize.empty() || config.awburst.empty() || config.awvalid.empty() ||
        config.awready.empty() || config.wdata.empty() || config.wstrb.empty() ||
        config.wlast.empty() || config.wvalid.empty() || config.wready.empty() ||
        config.bid.empty() || config.bresp.empty() || config.bvalid.empty() ||
        config.bready.empty() || config.araddr.empty() || config.arid.empty() ||
        config.arlen.empty() || config.arsize.empty() || config.arburst.empty() ||
        config.arvalid.empty() || config.arready.empty() || config.rid.empty() ||
        config.rdata.empty() || config.rresp.empty() || config.rlast.empty() ||
        config.rvalid.empty() || config.rready.empty() || config.clk.empty() ||
        config.rst_n.empty()) {
        fprintf(stderr, "Error: Missing required field in AXI JSON\n");
        return false;
    }
    return true;
}

static void print_axi_config(const AxiConfig& config) {
    printf("{\n");
    printf("  \"awaddr\": \"%s\",\n", config.awaddr.c_str());
    printf("  \"awid\": \"%s\",\n", config.awid.c_str());
    printf("  \"awlen\": \"%s\",\n", config.awlen.c_str());
    printf("  \"awsize\": \"%s\",\n", config.awsize.c_str());
    printf("  \"awburst\": \"%s\",\n", config.awburst.c_str());
    printf("  \"awvalid\": \"%s\",\n", config.awvalid.c_str());
    printf("  \"awready\": \"%s\",\n", config.awready.c_str());
    printf("  \"wdata\": \"%s\",\n", config.wdata.c_str());
    printf("  \"wstrb\": \"%s\",\n", config.wstrb.c_str());
    printf("  \"wlast\": \"%s\",\n", config.wlast.c_str());
    printf("  \"wvalid\": \"%s\",\n", config.wvalid.c_str());
    printf("  \"wready\": \"%s\",\n", config.wready.c_str());
    printf("  \"bid\": \"%s\",\n", config.bid.c_str());
    printf("  \"bresp\": \"%s\",\n", config.bresp.c_str());
    printf("  \"bvalid\": \"%s\",\n", config.bvalid.c_str());
    printf("  \"bready\": \"%s\",\n", config.bready.c_str());
    printf("  \"araddr\": \"%s\",\n", config.araddr.c_str());
    printf("  \"arid\": \"%s\",\n", config.arid.c_str());
    printf("  \"arlen\": \"%s\",\n", config.arlen.c_str());
    printf("  \"arsize\": \"%s\",\n", config.arsize.c_str());
    printf("  \"arburst\": \"%s\",\n", config.arburst.c_str());
    printf("  \"arvalid\": \"%s\",\n", config.arvalid.c_str());
    printf("  \"arready\": \"%s\",\n", config.arready.c_str());
    printf("  \"rid\": \"%s\",\n", config.rid.c_str());
    printf("  \"rdata\": \"%s\",\n", config.rdata.c_str());
    printf("  \"rresp\": \"%s\",\n", config.rresp.c_str());
    printf("  \"rlast\": \"%s\",\n", config.rlast.c_str());
    printf("  \"rvalid\": \"%s\",\n", config.rvalid.c_str());
    printf("  \"rready\": \"%s\",\n", config.rready.c_str());
    printf("  \"clk\": \"%s\",\n", config.clk.c_str());
    printf("  \"rst_n\": \"%s\",\n", config.rst_n.c_str());
    printf("  \"edge\": \"%s\"\n", config.posedge ? "posedge" : "negedge");
    printf("}\n");
}

int cmd_axi(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s axi <json-file> -n <name> [-s <sid>]\n", argv[0]);
        fprintf(stderr, "       %s axi list [-n <name>] [-s <sid>]\n", argv[0]);
        fprintf(stderr, "       %s axi wr [-n <name>] [-s <sid>] [-addr <addr>] [-id <id>] [-num <x>] [-last] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi rd [-n <name>] [-s <sid>] [-addr <addr>] [-id <id>] [-num <x>] [-last] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi begin [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi next [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi pre [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi last [-n <name>] [-s <sid>] [-rd|-wr] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi latency [-n <name>] [-s <sid>] [-rd|-wr|-all] [-id <id>] [-json]\n", argv[0]);
        fprintf(stderr, "       %s axi osd [-n <name>] [-s <sid>] [-rd|-wr|-all] [-id <id>] [-json]\n", argv[0]);
        return 1;
    }

    const char* subcmd_or_file = argv[2];

    // --- Load: xwave axi <json-file> -n <name> [-s <sid>] ---
    if (strcmp(subcmd_or_file, "wr") != 0 &&
        strcmp(subcmd_or_file, "rd") != 0 &&
        strcmp(subcmd_or_file, "begin") != 0 &&
        strcmp(subcmd_or_file, "next") != 0 &&
        strcmp(subcmd_or_file, "pre") != 0 &&
        strcmp(subcmd_or_file, "last") != 0 &&
        strcmp(subcmd_or_file, "latency") != 0 &&
        strcmp(subcmd_or_file, "osd") != 0 &&
        strcmp(subcmd_or_file, "list") != 0) {
        const char* json_file = subcmd_or_file;
        std::string session_id;
        const char* name = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = argv[++i];
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        }
        if (!name) {
            fprintf(stderr, "Error: -n <name> is required for loading AXI config\n");
            return 1;
        }
        if (!resolve_session_id(session_id, session_id)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        AxiConfig config;
        if (!load_json_config(json_file, config)) return 1;
        config.name = name;
        AxiManager am;
        if (!am.create_axi(session_id, config)) {
            fprintf(stderr, "Error: Failed to create AXI config '%s'\n", name);
            return 1;
        }
        printf("AXI config '%s' loaded for session %s.\n", name, session_id.c_str());
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
        AxiManager am;
        std::string axi_name;
        if (!resolve_axi_name(am, session_id, name, axi_name)) return 1;
        AxiConfig config;
        if (!am.get_axi(session_id, axi_name, config)) {
            fprintf(stderr, "Error: AXI config '%s' not found\n", axi_name.c_str());
            return 1;
        }
        print_axi_config(config);
        return 0;
    }

    // Common query parsing
    const char* subcmd = subcmd_or_file;
    std::string session_id;
    const char* name = nullptr;
    const char* addr_str = nullptr;
    const char* id_str = nullptr;
    int num_val = -1;
    bool has_last = false;
    bool json = false;
    bool filter_rd = false;
    bool filter_wr = false;
    bool filter_all = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "-addr") == 0 && i + 1 < argc) addr_str = argv[++i];
        else if (strcmp(argv[i], "-id") == 0 && i + 1 < argc) id_str = argv[++i];
        else if (strcmp(argv[i], "-num") == 0 && i + 1 < argc) num_val = atoi(argv[++i]);
        else if (strcmp(argv[i], "-last") == 0) has_last = true;
        else if (strcmp(argv[i], "-json") == 0) json = true;
        else if (strcmp(argv[i], "-rd") == 0) filter_rd = true;
        else if (strcmp(argv[i], "-wr") == 0) filter_wr = true;
        else if (strcmp(argv[i], "-all") == 0) filter_all = true;
    }

    if (!resolve_session_id(session_id, session_id)) {
        fprintf(stderr, "Error: No active sessions\n");
        return 1;
    }
    AxiManager am;
    std::string axi_name;
    if (!resolve_axi_name(am, session_id, name, axi_name)) return 1;

    std::string protocol_cmd;

    if (strcmp(subcmd, "wr") == 0 || strcmp(subcmd, "rd") == 0) {
        protocol_cmd = std::string(strcmp(subcmd, "wr") == 0 ? CMD_AXI_WR : CMD_AXI_RD);
        protocol_cmd += " " + axi_name;
        if (addr_str) {
            protocol_cmd += " addr ";
            protocol_cmd += addr_str;
        }
        if (id_str) {
            protocol_cmd += " id ";
            protocol_cmd += id_str;
        }
        if (num_val > 0) {
            protocol_cmd += " num ";
            protocol_cmd += std::to_string(num_val);
        }
        if (has_last) {
            protocol_cmd += " last";
        }
        if (json) protocol_cmd += " json";
    } else if (strcmp(subcmd, "latency") == 0 || strcmp(subcmd, "osd") == 0) {
        protocol_cmd = std::string(strcmp(subcmd, "latency") == 0 ? CMD_AXI_LATENCY : CMD_AXI_OSD) + " " + axi_name;
        if (filter_wr) protocol_cmd += " wr";
        else if (filter_rd) protocol_cmd += " rd";
        else if (filter_all) protocol_cmd += " all";
        else protocol_cmd += " all";
        if (id_str) {
            protocol_cmd += " id ";
            protocol_cmd += id_str;
        }
        if (json) protocol_cmd += " json";
    } else {
        const char* cmd_const = nullptr;
        if (strcmp(subcmd, "begin") == 0) cmd_const = CMD_AXI_BEGIN;
        else if (strcmp(subcmd, "next") == 0) cmd_const = CMD_AXI_NEXT;
        else if (strcmp(subcmd, "pre") == 0) cmd_const = CMD_AXI_PREV;
        else if (strcmp(subcmd, "last") == 0) cmd_const = CMD_AXI_LAST;
        else {
            fprintf(stderr, "Unknown axi subcommand: %s\n", subcmd);
            return 1;
        }
        protocol_cmd = std::string(cmd_const) + " " + axi_name;
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
