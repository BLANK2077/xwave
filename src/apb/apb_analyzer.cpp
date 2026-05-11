#include "apb_analyzer.h"
#include "../server/fsdb_value_reader.h"
#include "npi_fsdb.h"
#include "npi_L1.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>

namespace xwave {

bool ApbAnalyzer::parse_hex_value(const std::string& hex_str, uint64_t& out) {
    if (hex_str.empty()) return false;
    const char* start = hex_str.c_str();
    char* end = nullptr;
    out = strtoull(start, &end, 16);
    return end != start;
}

const ApbResult* ApbAnalyzer::get_result(const std::string& name) const {
    auto it = results_.find(name);
    if (it != results_.end()) return &it->second;
    return nullptr;
}

ApbResult* ApbAnalyzer::get_result_mut(const std::string& name) {
    auto it = results_.find(name);
    if (it != results_.end()) return &it->second;
    return nullptr;
}

ApbCursor* ApbAnalyzer::get_cursor_mut(const std::string& name) {
    auto it = cursors_.find(name);
    if (it != cursors_.end()) return &it->second;
    return nullptr;
}

bool ApbAnalyzer::analyze(const std::string& name, npiFsdbFileHandle file, const ApbConfig& config) {
    if (get_result(name) != nullptr) {
        return true; // already cached
    }

    npiFsdbSigHandle clk_sig = npi_fsdb_sig_by_name(file, config.clk.c_str(), NULL);
    if (!clk_sig) return false;

    npiFsdbVctHandle vct = npi_fsdb_create_vct(clk_sig);
    if (!vct) return false;

    ApbResult result;

    std::string prev_clk_val;
    if (npi_fsdb_goto_first(vct)) {
        do {
            npiFsdbTime t;
            npi_fsdb_vct_time(vct, &t);

            npiFsdbValue clk_val;
            clk_val.format = npiFsdbBinStrVal;
            std::string curr_clk_val;
            if (npi_fsdb_vct_value(vct, &clk_val) && clk_val.value.str) {
                curr_clk_val = clk_val.value.str;
            }

            bool is_target_edge = false;
            if (config.posedge && prev_clk_val == "0" && curr_clk_val == "1") {
                is_target_edge = true;
            } else if (!config.posedge && prev_clk_val == "1" && curr_clk_val == "0") {
                is_target_edge = true;
            }
            prev_clk_val = curr_clk_val;
            if (!is_target_edge) continue;

            std::vector<std::string> signals = {
                config.rst_n, config.psel, config.penable,
                config.pwrite, config.paddr, config.pwdata, config.prdata
            };
            std::vector<std::string> values;
            if (!read_sig_vec_value_at(file, signals, t, 'H', values)) {
                continue;
            }
            if (values.size() < 7) continue;

            const std::string& rst_n_val = values[0];
            const std::string& psel_val = values[1];
            const std::string& penable_val = values[2];
            const std::string& pwrite_val = values[3];
            const std::string& paddr_val = values[4];
            const std::string& pwdata_val = values[5];
            const std::string& prdata_val = values[6];

            // Check rst_n == 1
            if (rst_n_val.empty() || rst_n_val == "0" || rst_n_val == "X" || rst_n_val == "Z") {
                continue;
            }
            // Check psel == 1
            if (psel_val.empty() || psel_val == "0" || psel_val == "X" || psel_val == "Z") {
                continue;
            }
            // Check penable == 1
            if (penable_val.empty() || penable_val == "0" || penable_val == "X" || penable_val == "Z") {
                continue;
            }

            bool is_write = !(pwrite_val.empty() || pwrite_val == "0" || pwrite_val == "X" || pwrite_val == "Z");

            ApbTransaction txn;
            txn.time = t;
            txn.addr = paddr_val;
            txn.data = is_write ? pwdata_val : prdata_val;
            txn.is_write = is_write;

            result.all.push_back(txn);
            if (is_write) {
                result.writes.push_back(txn);
            } else {
                result.reads.push_back(txn);
            }
        } while (npi_fsdb_goto_next(vct));
    }

    npi_fsdb_release_vct(vct);

    // Sort by time just in case (though VCT should naturally be in order)
    auto cmp = [](const ApbTransaction& a, const ApbTransaction& b) { return a.time < b.time; };
    std::sort(result.all.begin(), result.all.end(), cmp);
    std::sort(result.writes.begin(), result.writes.end(), cmp);
    std::sort(result.reads.begin(), result.reads.end(), cmp);

    results_[name] = std::move(result);
    cursors_[name] = ApbCursor();
    return true;
}

size_t ApbAnalyzer::get_write_count(const std::string& name) const {
    const ApbResult* r = get_result(name);
    return r ? r->writes.size() : 0;
}

size_t ApbAnalyzer::get_read_count(const std::string& name) const {
    const ApbResult* r = get_result(name);
    return r ? r->reads.size() : 0;
}

bool ApbAnalyzer::get_write_by_addr(const std::string& name, uint64_t addr, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r) return false;
    for (const auto& txn : r->writes) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr) {
            out = &txn;
            return true;
        }
    }
    return false;
}

bool ApbAnalyzer::get_write_by_addr_num(const std::string& name, uint64_t addr, size_t num, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r) return false;
    if (num == 0) return false;
    size_t count = 0;
    for (const auto& txn : r->writes) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr) {
            if (++count == num) {
                out = &txn;
                return true;
            }
        }
    }
    return false;
}

bool ApbAnalyzer::get_write_by_addr_last(const std::string& name, uint64_t addr, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r) return false;
    const ApbTransaction* found = nullptr;
    for (const auto& txn : r->writes) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr) {
            found = &txn;
        }
    }
    if (found) {
        out = found;
        return true;
    }
    return false;
}

bool ApbAnalyzer::get_write_by_num(const std::string& name, size_t num, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r || num == 0 || num > r->writes.size()) return false;
    out = &r->writes[num - 1];
    return true;
}

bool ApbAnalyzer::get_write_last(const std::string& name, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r || r->writes.empty()) return false;
    out = &r->writes.back();
    return true;
}

bool ApbAnalyzer::get_read_by_addr(const std::string& name, uint64_t addr, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r) return false;
    for (const auto& txn : r->reads) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr) {
            out = &txn;
            return true;
        }
    }
    return false;
}

bool ApbAnalyzer::get_read_by_addr_num(const std::string& name, uint64_t addr, size_t num, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r) return false;
    if (num == 0) return false;
    size_t count = 0;
    for (const auto& txn : r->reads) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr) {
            if (++count == num) {
                out = &txn;
                return true;
            }
        }
    }
    return false;
}

bool ApbAnalyzer::get_read_by_addr_last(const std::string& name, uint64_t addr, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r) return false;
    const ApbTransaction* found = nullptr;
    for (const auto& txn : r->reads) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr) {
            found = &txn;
        }
    }
    if (found) {
        out = found;
        return true;
    }
    return false;
}

bool ApbAnalyzer::get_read_by_num(const std::string& name, size_t num, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r || num == 0 || num > r->reads.size()) return false;
    out = &r->reads[num - 1];
    return true;
}

bool ApbAnalyzer::get_read_last(const std::string& name, const ApbTransaction*& out) const {
    const ApbResult* r = get_result(name);
    if (!r || r->reads.empty()) return false;
    out = &r->reads.back();
    return true;
}

// Cursor operations
bool ApbAnalyzer::cursor_begin(const std::string& name, int filter, const ApbTransaction*& out) {
    ApbResult* r = get_result_mut(name);
    ApbCursor* c = get_cursor_mut(name);
    if (!r || !c) return false;

    if (filter == 1) {
        c->wr_idx = 0;
        if (c->wr_idx < r->writes.size()) {
            out = &r->writes[c->wr_idx];
            return true;
        }
    } else if (filter == 2) {
        c->rd_idx = 0;
        if (c->rd_idx < r->reads.size()) {
            out = &r->reads[c->rd_idx];
            return true;
        }
    } else {
        c->all_idx = 0;
        if (c->all_idx < r->all.size()) {
            out = &r->all[c->all_idx];
            return true;
        }
    }
    return false;
}

bool ApbAnalyzer::cursor_next(const std::string& name, int filter, const ApbTransaction*& out) {
    ApbResult* r = get_result_mut(name);
    ApbCursor* c = get_cursor_mut(name);
    if (!r || !c) return false;

    if (filter == 1) {
        if (c->wr_idx + 1 < r->writes.size()) {
            out = &r->writes[++c->wr_idx];
            return true;
        }
    } else if (filter == 2) {
        if (c->rd_idx + 1 < r->reads.size()) {
            out = &r->reads[++c->rd_idx];
            return true;
        }
    } else {
        if (c->all_idx + 1 < r->all.size()) {
            out = &r->all[++c->all_idx];
            return true;
        }
    }
    return false;
}

bool ApbAnalyzer::cursor_prev(const std::string& name, int filter, const ApbTransaction*& out) {
    ApbResult* r = get_result_mut(name);
    ApbCursor* c = get_cursor_mut(name);
    if (!r || !c) return false;

    if (filter == 1) {
        if (c->wr_idx > 0) {
            out = &r->writes[--c->wr_idx];
            return true;
        }
    } else if (filter == 2) {
        if (c->rd_idx > 0) {
            out = &r->reads[--c->rd_idx];
            return true;
        }
    } else {
        if (c->all_idx > 0) {
            out = &r->all[--c->all_idx];
            return true;
        }
    }
    return false;
}

bool ApbAnalyzer::cursor_last(const std::string& name, int filter, const ApbTransaction*& out) {
    ApbResult* r = get_result_mut(name);
    ApbCursor* c = get_cursor_mut(name);
    if (!r || !c) return false;

    if (filter == 1) {
        if (!r->writes.empty()) {
            c->wr_idx = r->writes.size() - 1;
            out = &r->writes[c->wr_idx];
            return true;
        }
    } else if (filter == 2) {
        if (!r->reads.empty()) {
            c->rd_idx = r->reads.size() - 1;
            out = &r->reads[c->rd_idx];
            return true;
        }
    } else {
        if (!r->all.empty()) {
            c->all_idx = r->all.size() - 1;
            out = &r->all[c->all_idx];
            return true;
        }
    }
    return false;
}

bool ApbAnalyzer::get_transactions_in_range(const std::string& name,
                                            npiFsdbTime begin,
                                            npiFsdbTime end,
                                            std::vector<ApbContextTransaction>& out) const {
    out.clear();
    const ApbResult* r = get_result(name);
    if (!r) return false;

    for (const auto& txn : r->all) {
        if (txn.time >= begin && txn.time <= end) {
            ApbContextTransaction item;
            item.txn = &txn;
            out.push_back(item);
        }
    }
    return true;
}

} // namespace xwave
