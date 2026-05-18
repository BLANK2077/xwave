#pragma once

#include "apb_config.h"
#include <vector>
#include <string>

namespace xwave {

class ApbManager {
public:
    ApbManager();
    ~ApbManager();

    bool create_apb(const std::string& session_id, const ApbConfig& config);
    bool create_apb(int session_id, const ApbConfig& config) { return create_apb(std::to_string(session_id), config); }
    bool delete_apb(const std::string& session_id, const std::string& name);
    bool get_apb(const std::string& session_id, const std::string& name, ApbConfig& config);
    bool get_apb(int session_id, const std::string& name, ApbConfig& config) { return get_apb(std::to_string(session_id), name, config); }
    bool get_latest_apb(const std::string& session_id, std::string& name);
    bool get_latest_apb(int session_id, std::string& name) { return get_latest_apb(std::to_string(session_id), name); }
    std::vector<ApbConfig> list_all(const std::string& session_id);

private:
    bool load_session(const std::string& session_id, std::vector<ApbConfig>& configs);
    bool save_session(const std::string& session_id, const std::vector<ApbConfig>& configs);
    bool migrate_legacy(const std::string& session_id, std::vector<ApbConfig>& configs);
    static bool parse_legacy_line(const char* line, ApbConfig& config, int& session_id);
};

} // namespace xwave
