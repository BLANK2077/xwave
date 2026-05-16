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
    bool load_session(int session_id, std::vector<ApbConfig>& configs);
    bool save_session(int session_id, const std::vector<ApbConfig>& configs);
    bool migrate_legacy(int session_id, std::vector<ApbConfig>& configs);
    static bool parse_legacy_line(const char* line, ApbConfig& config, int& session_id);
};

} // namespace xwave
