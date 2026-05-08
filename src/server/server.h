#pragma once

namespace xwave {

// Server main function - called when --server flag is passed
// argv: [exe, --server, session_id, fsdb_file]
int server_main(int argc, char** argv);

} // namespace xwave
