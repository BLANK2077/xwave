#pragma once

namespace xwave {

// Command implementations

// xwave open - create a new session
int cmd_open(int argc, char** argv);

// xwave session list - list all sessions
int cmd_session_list();

// xwave session kill <id> - kill a session
int cmd_session_kill(const char* id_str);

// xwave session doctor -s <sid> [-json] - diagnose a session
int cmd_session_doctor(int argc, char** argv);

// xwave session gc - clean stale and idle sessions
int cmd_session_gc();

// Print help message
void print_help(const char* prog);

} // namespace xwave
