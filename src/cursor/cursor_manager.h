#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xwave {

struct Cursor {
    std::string name;
    uint64_t time = 0;
    std::string time_text;
    std::string note;
    std::string origin;
    std::string clock;
    long created_at = 0;
    long updated_at = 0;
};

class CursorManager {
public:
    bool set_cursor(int session_id, const Cursor& cursor, bool make_active = true);
    bool get_cursor(int session_id, const std::string& name, Cursor& cursor) const;
    bool delete_cursor(int session_id, const std::string& name);
    bool use_cursor(int session_id, const std::string& name);
    bool get_active_cursor(int session_id, std::string& name) const;
    std::vector<Cursor> list_cursors(int session_id) const;
};

} // namespace xwave
