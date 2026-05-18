#include "axi_manager.h"
#include "../common/xwave_paths.h"
#include "../json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

namespace xwave {

using Json = nlohmann::ordered_json;

AxiManager::AxiManager() {
}

AxiManager::~AxiManager() {
}

static bool lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

static bool unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

static Json axi_to_json(const AxiConfig& c) {
    return {
        {"name", c.name},
        {"awaddr", c.awaddr}, {"awid", c.awid}, {"awlen", c.awlen}, {"awsize", c.awsize}, {"awburst", c.awburst},
        {"awvalid", c.awvalid}, {"awready", c.awready},
        {"wdata", c.wdata}, {"wstrb", c.wstrb}, {"wlast", c.wlast}, {"wvalid", c.wvalid}, {"wready", c.wready},
        {"bid", c.bid}, {"bresp", c.bresp}, {"bvalid", c.bvalid}, {"bready", c.bready},
        {"araddr", c.araddr}, {"arid", c.arid}, {"arlen", c.arlen}, {"arsize", c.arsize}, {"arburst", c.arburst},
        {"arvalid", c.arvalid}, {"arready", c.arready},
        {"rid", c.rid}, {"rdata", c.rdata}, {"rresp", c.rresp}, {"rlast", c.rlast}, {"rvalid", c.rvalid}, {"rready", c.rready},
        {"clk", c.clk}, {"rst_n", c.rst_n}, {"edge", c.posedge ? "posedge" : "negedge"}
    };
}

static bool json_to_axi(const Json& j, AxiConfig& c) {
    if (!j.is_object()) return false;
    c.name = j.value("name", "");
    c.awaddr = j.value("awaddr", ""); c.awid = j.value("awid", ""); c.awlen = j.value("awlen", ""); c.awsize = j.value("awsize", ""); c.awburst = j.value("awburst", "");
    c.awvalid = j.value("awvalid", ""); c.awready = j.value("awready", "");
    c.wdata = j.value("wdata", ""); c.wstrb = j.value("wstrb", ""); c.wlast = j.value("wlast", ""); c.wvalid = j.value("wvalid", ""); c.wready = j.value("wready", "");
    c.bid = j.value("bid", ""); c.bresp = j.value("bresp", ""); c.bvalid = j.value("bvalid", ""); c.bready = j.value("bready", "");
    c.araddr = j.value("araddr", ""); c.arid = j.value("arid", ""); c.arlen = j.value("arlen", ""); c.arsize = j.value("arsize", ""); c.arburst = j.value("arburst", "");
    c.arvalid = j.value("arvalid", ""); c.arready = j.value("arready", "");
    c.rid = j.value("rid", ""); c.rdata = j.value("rdata", ""); c.rresp = j.value("rresp", ""); c.rlast = j.value("rlast", ""); c.rvalid = j.value("rvalid", ""); c.rready = j.value("rready", "");
    c.clk = j.value("clk", ""); c.rst_n = j.value("rst_n", "");
    c.posedge = j.value("edge", "posedge") != "negedge";
    return !c.name.empty();
}

bool AxiManager::parse_legacy_line(const char* line, AxiConfig& config, int& session_id) {
    std::vector<std::string> fields;
    const char* start = line;
    const char* p = line;
    while (*p) {
        if (*p == '|') {
            fields.emplace_back(start, p - start);
            start = p + 1;
        }
        ++p;
    }
    fields.emplace_back(start);
    if (fields.size() != 34) return false;

    session_id       = atoi(fields[0].c_str());
    config.name      = fields[1];
    config.awaddr    = fields[2];
    config.awid      = fields[3];
    config.awlen     = fields[4];
    config.awsize    = fields[5];
    config.awburst   = fields[6];
    config.awvalid   = fields[7];
    config.awready   = fields[8];
    config.wdata     = fields[9];
    config.wstrb     = fields[10];
    config.wlast     = fields[11];
    config.wvalid    = fields[12];
    config.wready    = fields[13];
    config.bid       = fields[14];
    config.bresp     = fields[15];
    config.bvalid    = fields[16];
    config.bready    = fields[17];
    config.araddr    = fields[18];
    config.arid      = fields[19];
    config.arlen     = fields[20];
    config.arsize    = fields[21];
    config.arburst   = fields[22];
    config.arvalid   = fields[23];
    config.arready   = fields[24];
    config.rid       = fields[25];
    config.rdata     = fields[26];
    config.rresp     = fields[27];
    config.rlast     = fields[28];
    config.rvalid    = fields[29];
    config.rready    = fields[30];
    config.clk       = fields[31];
    config.rst_n     = fields[32];
    config.posedge   = (fields[33] == "posedge");
    return true;
}

bool AxiManager::migrate_legacy(const std::string& session_id, std::vector<AxiConfig>& configs) {
    (void)session_id;
    (void)configs;
    return false;
}

bool AxiManager::load_session(const std::string& session_id, std::vector<AxiConfig>& configs) {
    configs.clear();
    int fd = open(xwave_axi_path(session_id).c_str(), O_RDONLY);
    if (fd < 0) return migrate_legacy(session_id, configs);
    if (!lock_file(fd)) {
        close(fd);
        return false;
    }
    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        unlock_file(fd);
        close(fd);
        return false;
    }
    std::string text;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) text += buf;
    fclose(fp);
    if (text.empty()) return true;
    try {
        Json root = Json::parse(text);
        if (!root.is_object() || !root.value("configs", Json::array()).is_array()) return true;
        for (const auto& item : root["configs"]) {
            AxiConfig config;
            if (json_to_axi(item, config)) configs.push_back(config);
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool AxiManager::save_session(const std::string& session_id, const std::vector<AxiConfig>& configs) {
    if (!xwave_ensure_session_dir(session_id)) return false;
    int fd = open(xwave_axi_path(session_id).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (!lock_file(fd)) {
        close(fd);
        return false;
    }
    Json root;
    root["version"] = 1;
    root["configs"] = Json::array();
    for (const auto& config : configs) root["configs"].push_back(axi_to_json(config));
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    unlock_file(fd);
    close(fd);
    return ok;
}

bool AxiManager::create_axi(const std::string& session_id, const AxiConfig& config) {
    std::vector<AxiConfig> configs;
    load_session(session_id, configs);
    for (const auto& c : configs) {
        if (c.name == config.name) return false;
    }
    configs.push_back(config);
    return save_session(session_id, configs);
}

bool AxiManager::delete_axi(const std::string& session_id, const std::string& name) {
    std::vector<AxiConfig> configs;
    load_session(session_id, configs);
    std::vector<AxiConfig> out;
    bool found = false;
    for (const auto& c : configs) {
        if (c.name == name) {
            found = true;
            continue;
        }
        out.push_back(c);
    }
    return found && save_session(session_id, out);
}

bool AxiManager::get_axi(const std::string& session_id, const std::string& name, AxiConfig& config) {
    std::vector<AxiConfig> configs;
    load_session(session_id, configs);
    for (const auto& c : configs) {
        if (c.name == name) {
            config = c;
            return true;
        }
    }
    return false;
}

bool AxiManager::get_latest_axi(const std::string& session_id, std::string& name) {
    std::vector<AxiConfig> configs;
    load_session(session_id, configs);
    if (configs.empty()) return false;
    name = configs.back().name;
    return true;
}

std::vector<AxiConfig> AxiManager::list_all(const std::string& session_id) {
    std::vector<AxiConfig> configs;
    load_session(session_id, configs);
    return configs;
}

} // namespace xwave
