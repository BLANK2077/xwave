#pragma once

#include <map>
#include <string>

namespace xwave {

struct EventField {
    std::string signal_alias;
    int left = 0;
    int right = 0;
};

struct EventConfig {
    std::string name;
    std::string clk;
    std::string rst_n;
    bool posedge = true;
    std::map<std::string, std::string> signals;
    std::map<std::string, EventField> fields;
};

} // namespace xwave
