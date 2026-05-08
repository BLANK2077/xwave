#pragma once

#include <string>

namespace xwave {

struct AxiConfig {
    std::string name;
    // Write Address
    std::string awaddr, awid, awlen, awsize, awburst;
    std::string awvalid, awready;
    // Write Data
    std::string wdata, wstrb, wlast, wvalid, wready;
    // Write Response
    std::string bid, bresp, bvalid, bready;
    // Read Address
    std::string araddr, arid, arlen, arsize, arburst;
    std::string arvalid, arready;
    // Read Data
    std::string rid, rdata, rresp, rlast, rvalid, rready;
    // Common
    std::string clk, rst_n;
    bool posedge = true;
};

} // namespace xwave
