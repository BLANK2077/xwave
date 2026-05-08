#pragma once

#include "apb_config.h"
#include <vector>
#include <string>

namespace xwave {

class ApbManager {
public:
    ApbManager();
    ~ApbManager();

    bool create_apb(int session_id, const ApbConfig& config);
    bool delete_apb(int session_id, const std::string& name);
    bool get_apb(int session_id, const std::string& name, ApbConfig& config);
    bool get_latest_apb(int session_id, std::string& name);
    std::vector<ApbConfig> list_all(int session_id);

private:
    char apbs_path_[256];

    bool load_all(std::vector<ApbConfig>& configs, std::vector<int>& session_ids);
    bool save_all(const std::vector<ApbConfig>& configs, const std::vector<int>& session_ids);
    static bool parse_line(const char* line, ApbConfig& config, int& session_id);
    static std::string config_to_line(int session_id, const ApbConfig& config);
};

} // namespace xwave
