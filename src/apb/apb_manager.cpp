#include "apb_manager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

namespace xwave {

ApbManager::ApbManager() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(apbs_path_, sizeof(apbs_path_), "%s/.xwave.apb", home);
}

ApbManager::~ApbManager() {
}

static bool lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

static bool unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

bool ApbManager::parse_line(const char* line, ApbConfig& config, int& session_id) {
    char buf[11][1024];
    int n = sscanf(line, "%d|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|]|%1023[^|\n]",
                   &session_id,
                   buf[0], buf[1], buf[2], buf[3], buf[4],
                   buf[5], buf[6], buf[7], buf[8], buf[9]);
    if (n != 11) return false;
    config.name    = buf[0];
    config.paddr   = buf[1];
    config.pwdata  = buf[2];
    config.prdata  = buf[3];
    config.pwrite  = buf[4];
    config.penable = buf[5];
    config.psel    = buf[6];
    config.clk     = buf[7];
    config.rst_n   = buf[8];
    config.posedge = (strcmp(buf[9], "posedge") == 0);
    return true;
}

std::string ApbManager::config_to_line(int session_id, const ApbConfig& config) {
    return std::to_string(session_id) + "|" + config.name + "|" + config.paddr + "|" +
           config.pwdata + "|" + config.prdata + "|" + config.pwrite + "|" +
           config.penable + "|" + config.psel + "|" + config.clk + "|" +
           config.rst_n + "|" + (config.posedge ? "posedge" : "negedge") + "\n";
}

bool ApbManager::load_all(std::vector<ApbConfig>& configs, std::vector<int>& session_ids) {
    configs.clear();
    session_ids.clear();

    int fd = open(apbs_path_, O_RDONLY | O_CREAT, 0600);
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

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        ApbConfig config;
        int sid;
        if (parse_line(line, config, sid)) {
            configs.push_back(config);
            session_ids.push_back(sid);
        }
    }

    fclose(fp);
    return true;
}

bool ApbManager::save_all(const std::vector<ApbConfig>& configs, const std::vector<int>& session_ids) {
    int fd = open(apbs_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
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

bool ApbManager::create_apb(int session_id, const ApbConfig& config) {
    std::vector<ApbConfig> configs;
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

bool ApbManager::delete_apb(int session_id, const std::string& name) {
    std::vector<ApbConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    std::vector<ApbConfig> new_configs;
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

bool ApbManager::get_apb(int session_id, const std::string& name, ApbConfig& config) {
    std::vector<ApbConfig> configs;
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

bool ApbManager::get_latest_apb(int session_id, std::string& name) {
    std::vector<ApbConfig> configs;
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

std::vector<ApbConfig> ApbManager::list_all(int session_id) {
    std::vector<ApbConfig> configs;
    std::vector<int> session_ids;
    load_all(configs, session_ids);

    std::vector<ApbConfig> result;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (session_ids[i] == session_id) {
            result.push_back(configs[i]);
        }
    }
    return result;
}

} // namespace xwave
