#pragma once

#include "axi_config.h"
#include <vector>
#include <string>

namespace xwave {

class AxiManager {
public:
    AxiManager();
    ~AxiManager();

    bool create_axi(const std::string& session_id, const AxiConfig& config);
    bool create_axi(int session_id, const AxiConfig& config) { return create_axi(std::to_string(session_id), config); }
    bool delete_axi(const std::string& session_id, const std::string& name);
    bool get_axi(const std::string& session_id, const std::string& name, AxiConfig& config);
    bool get_axi(int session_id, const std::string& name, AxiConfig& config) { return get_axi(std::to_string(session_id), name, config); }
    bool get_latest_axi(const std::string& session_id, std::string& name);
    bool get_latest_axi(int session_id, std::string& name) { return get_latest_axi(std::to_string(session_id), name); }
    std::vector<AxiConfig> list_all(const std::string& session_id);

private:
    bool load_session(const std::string& session_id, std::vector<AxiConfig>& configs);
    bool save_session(const std::string& session_id, const std::vector<AxiConfig>& configs);
    bool migrate_legacy(const std::string& session_id, std::vector<AxiConfig>& configs);
    static bool parse_legacy_line(const char* line, AxiConfig& config, int& session_id);
};

} // namespace xwave
