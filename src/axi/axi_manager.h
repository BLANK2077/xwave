#pragma once

#include "axi_config.h"
#include <vector>
#include <string>

namespace xwave {

class AxiManager {
public:
    AxiManager();
    ~AxiManager();

    bool create_axi(int session_id, const AxiConfig& config);
    bool delete_axi(int session_id, const std::string& name);
    bool get_axi(int session_id, const std::string& name, AxiConfig& config);
    bool get_latest_axi(int session_id, std::string& name);
    std::vector<AxiConfig> list_all(int session_id);

private:
    char axis_path_[256];

    bool load_all(std::vector<AxiConfig>& configs, std::vector<int>& session_ids);
    bool save_all(const std::vector<AxiConfig>& configs, const std::vector<int>& session_ids);
    static bool parse_line(const char* line, AxiConfig& config, int& session_id);
    static std::string config_to_line(int session_id, const AxiConfig& config);
};

} // namespace xwave
