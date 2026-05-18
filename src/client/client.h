#pragma once

#include <string>

namespace xwave {

// Client communication functions

// Connect to a session's server, returns fd or -1 on error
int session_connect(const std::string& session_id);

// Send command and print response
bool send_command_and_print(const std::string& session_id, const char* cmd);

// Send command and capture response payload without END marker
bool send_command_capture(const std::string& session_id, const char* cmd, std::string& payload);

// Check if session is responsive (sends PING)
bool session_ping(const std::string& session_id);

} // namespace xtrace
