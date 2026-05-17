#include "time_spec.h"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <strings.h>

namespace xwave {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool valid_unit(const std::string& unit) {
    return unit == "us" || unit == "ns" || unit == "ps" || unit == "fs";
}

bool is_cursor_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

} // namespace

bool parse_duration_spec(const std::string& text, DurationSpec& out, std::string& error) {
    std::string s = trim(text);
    if (s.empty()) {
        error = "Invalid duration: empty";
        return false;
    }
    char* end = nullptr;
    errno = 0;
    double value = strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || !std::isfinite(value)) {
        error = "Invalid duration '" + text + "'";
        return false;
    }
    std::string unit = trim(end ? end : "");
    if (unit.empty()) unit = "ns";
    if (unit.find("cycle(") == 0 && unit.size() > 7 && unit[unit.size() - 1] == ')') {
        out.value = value;
        out.unit = "cycle";
        out.cycle = true;
        out.clock = unit.substr(6, unit.size() - 7);
        return true;
    }
    if (!valid_unit(unit)) {
        error = "Invalid duration '" + text + "': unsupported unit, expected us/ns/ps/fs";
        return false;
    }
    out.value = value;
    out.unit = unit;
    out.cycle = false;
    out.clock.clear();
    return true;
}

bool parse_time_spec_text(const std::string& text, ParsedTimeSpec& out, std::string& error) {
    std::string s = trim(text);
    if (s.empty()) {
        error = "Invalid TimeSpec: empty";
        return false;
    }
    out = ParsedTimeSpec();
    out.source = s;
    if (s[0] != '@') {
        out.kind = TimeSpecKind::Absolute;
        out.absolute_text = s;
        return true;
    }

    out.kind = TimeSpecKind::Cursor;
    size_t pos = 1;
    if (pos >= s.size() || s[pos] == '+' || s[pos] == '-') {
        out.use_active_cursor = true;
    } else {
        size_t start = pos;
        while (pos < s.size()) {
            if (s[pos] == '+') break;
            if (s[pos] == '-' && pos + 1 < s.size() &&
                (std::isdigit(static_cast<unsigned char>(s[pos + 1])) || s[pos + 1] == '.')) {
                break;
            }
            if (!is_cursor_name_char(s[pos])) break;
            ++pos;
        }
        out.cursor_name = s.substr(start, pos - start);
        if (out.cursor_name.empty()) {
            error = "Invalid TimeSpec '" + text + "': cursor name is empty";
            return false;
        }
    }

    if (pos < s.size()) {
        char sign = s[pos];
        if (sign != '+' && sign != '-') {
            error = "Invalid TimeSpec '" + text + "': expected + or - before offset";
            return false;
        }
        DurationSpec dur;
        if (!parse_duration_spec(s.substr(pos + 1), dur, error)) return false;
        if (sign == '-') dur.value = -dur.value;
        out.has_offset = true;
        out.offset = dur;
    }
    return true;
}

} // namespace xwave
