#pragma once

#include <string>

namespace xwave {

// Client communication functions

// Connect to a session's server, returns fd or -1 on error
int session_connect(int session_id);

// Send command and print response
bool send_command_and_print(int session_id, const char* cmd);

// Send command and capture response payload without END marker
bool send_command_capture(int session_id, const char* cmd, std::string& payload);

// Check if session is responsive (sends PING)
bool session_ping(int session_id);

} // namespace xtrace
