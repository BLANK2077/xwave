#include "event_expr.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>

namespace xwave {

static std::string strip_value_prefix(const std::string& value) {
    if (value.size() >= 2 && value[0] == '\'' &&
        (value[1] == 'b' || value[1] == 'B' || value[1] == 'h' || value[1] == 'H' ||
         value[1] == 'd' || value[1] == 'D')) {
        return value.substr(2);
    }
    return value;
}

std::string expr_bits_only(const std::string& value) {
    std::string raw = strip_value_prefix(value);
    std::string out;
    for (char c : raw) {
        if (c == '0' || c == '1' || c == 'x' || c == 'X' || c == 'z' || c == 'Z') {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

bool expr_value_has_unknown(const std::string& value) {
    for (char c : value) {
        if (c == 'x' || c == 'X' || c == 'z' || c == 'Z') return true;
    }
    return false;
}

ExprTri expr_truth_value(const std::string& value) {
    std::string bits = expr_bits_only(value);
    if (bits.empty()) {
        std::string raw = strip_value_prefix(value);
        if (raw == "1") return ExprTri::True;
        if (raw == "0") return ExprTri::False;
        return ExprTri::Unknown;
    }
    if (expr_value_has_unknown(bits)) return ExprTri::Unknown;
    for (char c : bits) {
        if (c == '1') return ExprTri::True;
    }
    return ExprTri::False;
}

const char* expr_tri_text(ExprTri value) {
    if (value == ExprTri::True) return "true";
    if (value == ExprTri::False) return "false";
    return "unknown";
}

ExprTri expr_tri_not(ExprTri value) {
    if (value == ExprTri::True) return ExprTri::False;
    if (value == ExprTri::False) return ExprTri::True;
    return ExprTri::Unknown;
}

ExprTri expr_tri_and(ExprTri lhs, ExprTri rhs) {
    if (lhs == ExprTri::False || rhs == ExprTri::False) return ExprTri::False;
    if (lhs == ExprTri::True && rhs == ExprTri::True) return ExprTri::True;
    return ExprTri::Unknown;
}

ExprTri expr_tri_or(ExprTri lhs, ExprTri rhs) {
    if (lhs == ExprTri::True || rhs == ExprTri::True) return ExprTri::True;
    if (lhs == ExprTri::False && rhs == ExprTri::False) return ExprTri::False;
    return ExprTri::Unknown;
}

static std::string hex_to_bits(const std::string& hex) {
    std::string out;
    for (char c : hex) {
        if (c == '_' || c == ' ') continue;
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') v = 10 + c - 'A';
        if (v < 0) return "";
        for (int bit = 3; bit >= 0; --bit) out.push_back((v & (1 << bit)) ? '1' : '0');
    }
    size_t first_one = out.find('1');
    if (first_one == std::string::npos) return "0";
    return out.substr(first_one);
}

static std::string dec_to_bits(const std::string& dec) {
    std::string clean;
    for (char c : dec) {
        if (c != '_') clean.push_back(c);
    }
    char* end = nullptr;
    unsigned long long v = strtoull(clean.c_str(), &end, 10);
    if (!end || *end != '\0') return "";
    if (v == 0) return "0";
    std::string out;
    while (v) {
        out.push_back((v & 1ULL) ? '1' : '0');
        v >>= 1;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

static std::string literal_to_bits(const std::string& literal) {
    std::string s = literal;
    size_t tick = s.find('\'');
    if (tick != std::string::npos && tick + 1 < s.size()) {
        char base = static_cast<char>(std::tolower(static_cast<unsigned char>(s[tick + 1])));
        std::string body = s.substr(tick + 2);
        if (base == 'b') return expr_bits_only(body);
        if (base == 'h') return hex_to_bits(body);
        if (base == 'd') return dec_to_bits(body);
    }
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return hex_to_bits(s.substr(2));
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) return expr_bits_only(s.substr(2));
    return dec_to_bits(s);
}

static bool is_decimal_literal_text(const std::string& value) {
    bool saw_digit = false;
    for (char c : value) {
        if (c == '_') continue;
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        saw_digit = true;
    }
    return saw_digit;
}

std::string expr_normalize_for_compare(const std::string& value, size_t min_width) {
    std::string bits;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X' || value[1] == 'b' || value[1] == 'B')) {
        bits = literal_to_bits(value);
    } else if (value.find('\'') != std::string::npos) {
        bits = literal_to_bits(value);
        if (bits.empty()) bits = expr_bits_only(value);
    } else if (is_decimal_literal_text(value)) {
        bits = literal_to_bits(value);
    } else {
        bits = expr_bits_only(value);
        if (bits.empty()) bits = literal_to_bits(value);
    }
    if (bits.empty()) return value;
    size_t first = bits.find_first_not_of('0');
    if (first == std::string::npos) bits = "0";
    else bits = bits.substr(first);
    while (bits.size() < min_width) bits.insert(bits.begin(), '0');
    return bits;
}

class ExprParser {
public:
    ExprParser(const std::string& expr, const std::map<std::string, std::string>& values)
        : expr_(expr), values_(values) {}

    bool eval(ExprTri& result, std::string& error) {
        pos_ = 0;
        error.clear();
        result = parse_or(error);
        skip_ws();
        if (error.empty() && pos_ != expr_.size()) {
            error = "Unexpected token near: " + expr_.substr(pos_);
        }
        return error.empty();
    }

private:
    ExprTri parse_or(std::string& error) {
        ExprTri lhs = parse_and(error);
        while (error.empty()) {
            skip_ws();
            if (!consume("||")) break;
            ExprTri rhs = parse_and(error);
            lhs = expr_tri_or(lhs, rhs);
        }
        return lhs;
    }

    ExprTri parse_and(std::string& error) {
        ExprTri lhs = parse_unary(error);
        while (error.empty()) {
            skip_ws();
            if (!consume("&&")) break;
            ExprTri rhs = parse_unary(error);
            lhs = expr_tri_and(lhs, rhs);
        }
        return lhs;
    }

    ExprTri parse_unary(std::string& error) {
        skip_ws();
        if (consume("!")) return expr_tri_not(parse_unary(error));
        return parse_primary(error);
    }

    ExprTri parse_primary(std::string& error) {
        skip_ws();
        if (consume("(")) {
            ExprTri v = parse_or(error);
            if (error.empty() && !consume(")")) error = "Missing ')'";
            return v;
        }

        std::string lhs = parse_atom(error);
        if (!error.empty()) return ExprTri::False;
        skip_ws();
        if (consume("==") || consume("=")) {
            std::string rhs = parse_atom(error);
            if (!error.empty()) return ExprTri::False;
            return compare(lhs, rhs);
        }
        if (consume("!=")) {
            std::string rhs = parse_atom(error);
            if (!error.empty()) return ExprTri::False;
            ExprTri eq = compare(lhs, rhs);
            if (eq == ExprTri::Unknown) return ExprTri::Unknown;
            return eq == ExprTri::True ? ExprTri::False : ExprTri::True;
        }
        return expr_truth_value(lhs);
    }

    ExprTri compare(const std::string& lhs, const std::string& rhs) {
        size_t width = std::max(expr_bits_only(lhs).size(), expr_bits_only(rhs).size());
        std::string lhs_norm = expr_normalize_for_compare(lhs, width);
        std::string rhs_norm = expr_normalize_for_compare(rhs, width);
        if (expr_value_has_unknown(lhs_norm) || expr_value_has_unknown(rhs_norm)) return ExprTri::Unknown;
        return lhs_norm == rhs_norm ? ExprTri::True : ExprTri::False;
    }

    std::string parse_atom(std::string& error) {
        skip_ws();
        if (pos_ >= expr_.size()) {
            error = "Unexpected end of expression";
            return "";
        }
        if (expr_[pos_] == '\'' || std::isdigit(static_cast<unsigned char>(expr_[pos_]))) {
            return parse_literal();
        }
        if (is_ident_start(expr_[pos_])) {
            std::string name = parse_identifier();
            auto it = values_.find(name);
            if (it == values_.end()) {
                error = "Unknown alias in expression: " + name;
                return "";
            }
            return it->second;
        }
        error = "Unexpected token near: " + expr_.substr(pos_);
        return "";
    }

    std::string parse_literal() {
        size_t start = pos_;
        if (expr_[pos_] == '\'') {
            pos_++;
            if (pos_ < expr_.size()) pos_++;
            while (pos_ < expr_.size() && is_literal_char(expr_[pos_])) pos_++;
            return expr_.substr(start, pos_ - start);
        }
        while (pos_ < expr_.size() && is_literal_char(expr_[pos_])) pos_++;
        return expr_.substr(start, pos_ - start);
    }

    std::string parse_identifier() {
        size_t start = pos_;
        pos_++;
        while (pos_ < expr_.size() && is_ident_char(expr_[pos_])) pos_++;
        return expr_.substr(start, pos_ - start);
    }

    bool consume(const char* token) {
        skip_ws();
        size_t n = strlen(token);
        if (expr_.compare(pos_, n, token) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }

    void skip_ws() {
        while (pos_ < expr_.size() && std::isspace(static_cast<unsigned char>(expr_[pos_]))) pos_++;
    }

    static bool is_ident_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    static bool is_ident_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    }

    static bool is_literal_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'' || c == 'x' || c == 'X';
    }

    std::string expr_;
    const std::map<std::string, std::string>& values_;
    size_t pos_ = 0;
};

bool eval_event_expression(const std::string& expr,
                           const std::map<std::string, std::string>& values,
                           ExprTri& result,
                           std::string& error) {
    ExprParser parser(expr, values);
    return parser.eval(result, error);
}

} // namespace xwave
