#pragma once

#include <map>
#include <string>

namespace xwave {

enum class ExprTri {
    False,
    True,
    Unknown
};

const char* expr_tri_text(ExprTri value);
ExprTri expr_tri_not(ExprTri value);
ExprTri expr_tri_and(ExprTri lhs, ExprTri rhs);
ExprTri expr_tri_or(ExprTri lhs, ExprTri rhs);

bool expr_value_has_unknown(const std::string& value);
ExprTri expr_truth_value(const std::string& value);
std::string expr_bits_only(const std::string& value);
std::string expr_normalize_for_compare(const std::string& value, size_t min_width);

bool eval_event_expression(const std::string& expr,
                           const std::map<std::string, std::string>& values,
                           ExprTri& result,
                           std::string& error);

} // namespace xwave
