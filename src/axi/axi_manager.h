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
    bool load_session(int session_id, std::vector<AxiConfig>& configs);
    bool save_session(int session_id, const std::vector<AxiConfig>& configs);
    bool migrate_legacy(int session_id, std::vector<AxiConfig>& configs);
    static bool parse_legacy_line(const char* line, AxiConfig& config, int& session_id);
};

} // namespace xwave
