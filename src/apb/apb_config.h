#pragma once

#include <string>

namespace xwave {

struct ApbConfig {
    std::string name;
    std::string paddr;
    std::string pwdata;
    std::string prdata;
    std::string pwrite;
    std::string penable;
    std::string psel;
    std::string clk;
    std::string rst_n;
    bool posedge = true;
};

} // namespace xwave
