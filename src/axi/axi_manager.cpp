#include "axi_manager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

namespace xwave {

AxiManager::AxiManager() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(axis_path_, sizeof(axis_path_), "%s/.xwave.axi", home);
}

AxiManager::~AxiManager() {
}

static bool lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

static bool unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

bool AxiManager::parse_line(const char* line, AxiConfig& config, int& session_id) {
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

std::string AxiManager::config_to_line(int session_id, const AxiConfig& config) {
    return std::to_string(session_id) + "|" + config.name + "|" +
           config.awaddr + "|" + config.awid + "|" + config.awlen + "|" +
           config.awsize + "|" + config.awburst + "|" + config.awvalid + "|" +
           config.awready + "|" + config.wdata + "|" + config.wstrb + "|" +
           config.wlast + "|" + config.wvalid + "|" + config.wready + "|" +
           config.bid + "|" + config.bresp + "|" + config.bvalid + "|" +
           config.bready + "|" + config.araddr + "|" + config.arid + "|" +
           config.arlen + "|" + config.arsize + "|" + config.arburst + "|" +
           config.arvalid + "|" + config.arready + "|" + config.rid + "|" +
           config.rdata + "|" + config.rresp + "|" + config.rlast + "|" +
           config.rvalid + "|" + config.rready + "|" + config.clk + "|" +
           config.rst_n + "|" + (config.posedge ? "posedge" : "negedge") + "\n";
}

bool AxiManager::load_all(std::vector<AxiConfig>& configs, std::vector<int>& session_ids) {
    configs.clear();
    session_ids.clear();

    int fd = open(axis_path_, O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return false;
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

    char line[65536];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        AxiConfig config;
        int sid;
        if (parse_line(line, config, sid)) {
            configs.push_back(config);
            session_ids.push_back(sid);
        }
    }

    fclose(fp);
    return true;
}

bool AxiManager::save_all(const std::vector<AxiConfig>& configs, const std::vector<int>& session_ids) {
    int fd = open(axis_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    for (size_t i = 0; i < configs.size(); ++i) {
        std::string line = config_to_line(session_ids[i], configs[i]);
        write(fd, line.c_str(), line.length());
    }

    unlock_file(fd);
    close(fd);
    return true;
}

bool AxiManager::create_axi(int session_id, const AxiConfig& config) {
    std::vector<AxiConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    for (size_t i = 0; i < configs.size(); ++i) {
        if (session_ids[i] == session_id && configs[i].name == config.name) {
            return false; // already exists
        }
    }

    configs.push_back(config);
    session_ids.push_back(session_id);
    return save_all(configs, session_ids);
}

bool AxiManager::delete_axi(int session_id, const std::string& name) {
    std::vector<AxiConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    std::vector<AxiConfig> new_configs;
    std::vector<int> new_session_ids;
    bool found = false;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (session_ids[i] == session_id && configs[i].name == name) {
            found = true;
            continue;
        }
        new_configs.push_back(configs[i]);
        new_session_ids.push_back(session_ids[i]);
    }
    if (!found) return false;
    return save_all(new_configs, new_session_ids);
}

bool AxiManager::get_axi(int session_id, const std::string& name, AxiConfig& config) {
    std::vector<AxiConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    for (size_t i = 0; i < configs.size(); ++i) {
        if (session_ids[i] == session_id && configs[i].name == name) {
            config = configs[i];
            return true;
        }
    }
    return false;
}

bool AxiManager::get_latest_axi(int session_id, std::string& name) {
    std::vector<AxiConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    for (int i = (int)configs.size() - 1; i >= 0; --i) {
        if (session_ids[i] == session_id) {
            name = configs[i].name;
            return true;
        }
    }
    return false;
}

std::vector<AxiConfig> AxiManager::list_all(int session_id) {
    std::vector<AxiConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    std::vector<AxiConfig> result;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (session_ids[i] == session_id) {
            result.push_back(configs[i]);
        }
    }
    return result;
}

} // namespace xwave
