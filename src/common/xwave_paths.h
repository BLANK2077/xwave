#pragma once

#include <string>

namespace xwave {

std::string xwave_home_dir();
std::string xwave_sessions_dir();
std::string xwave_session_dir(int session_id);
std::string xwave_registry_path();
std::string xwave_registry_lock_path();
std::string xwave_session_json_path(int session_id);
std::string xwave_socket_path(int session_id);
std::string xwave_debug_log_path(int session_id);
std::string xwave_lists_path(int session_id);
std::string xwave_apb_path(int session_id);
std::string xwave_axi_path(int session_id);
std::string xwave_events_path(int session_id);

std::string xwave_legacy_registry_path();
std::string xwave_legacy_lists_path();
std::string xwave_legacy_apb_path();
std::string xwave_legacy_axi_path();
std::string xwave_legacy_events_path();

bool xwave_ensure_home();
bool xwave_ensure_session_dir(int session_id);
bool xwave_remove_session_dir(int session_id);
bool xwave_legacy_registry_has_session(int session_id);

} // namespace xwave
