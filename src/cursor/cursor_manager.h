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
    bool set_cursor(const std::string& session_id, const Cursor& cursor, bool make_active = true);
    bool set_cursor(int session_id, const Cursor& cursor, bool make_active = true) { return set_cursor(std::to_string(session_id), cursor, make_active); }
    bool get_cursor(const std::string& session_id, const std::string& name, Cursor& cursor) const;
    bool get_cursor(int session_id, const std::string& name, Cursor& cursor) const { return get_cursor(std::to_string(session_id), name, cursor); }
    bool delete_cursor(const std::string& session_id, const std::string& name);
    bool delete_cursor(int session_id, const std::string& name) { return delete_cursor(std::to_string(session_id), name); }
    bool use_cursor(const std::string& session_id, const std::string& name);
    bool use_cursor(int session_id, const std::string& name) { return use_cursor(std::to_string(session_id), name); }
    bool get_active_cursor(const std::string& session_id, std::string& name) const;
    bool get_active_cursor(int session_id, std::string& name) const { return get_active_cursor(std::to_string(session_id), name); }
    std::vector<Cursor> list_cursors(const std::string& session_id) const;
    std::vector<Cursor> list_cursors(int session_id) const { return list_cursors(std::to_string(session_id)); }
};

} // namespace xwave
