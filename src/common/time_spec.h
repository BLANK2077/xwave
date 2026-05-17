#pragma once

#include <string>

namespace xwave {

enum class TimeSpecKind {
    Absolute,
    Cursor
};

struct DurationSpec {
    double value = 0.0;
    std::string unit = "ns";
    bool cycle = false;
    std::string clock;
};

struct ParsedTimeSpec {
    TimeSpecKind kind = TimeSpecKind::Absolute;
    std::string source;
    std::string absolute_text;
    std::string cursor_name;
    bool use_active_cursor = false;
    bool has_offset = false;
    DurationSpec offset;
};

bool parse_duration_spec(const std::string& text, DurationSpec& out, std::string& error);
bool parse_time_spec_text(const std::string& text, ParsedTimeSpec& out, std::string& error);

} // namespace xwave
