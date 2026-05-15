#include "axi_analyzer.h"
#include "../server/fsdb_value_reader.h"
#include "../server/fsdb_scan_utils.h"
#include "npi_fsdb.h"
#include "npi_L1.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <list>
#include <deque>
#include <map>
#include <limits>
#include <set>

namespace xwave {

bool AxiAnalyzer::parse_hex_value(const std::string& hex_str, uint64_t& out) {
    if (hex_str.empty()) return false;
    const char* start = hex_str.c_str();
    char* end = nullptr;
    out = strtoull(start, &end, 16);
    return end != start;
}

bool AxiAnalyzer::id_matches(const std::string& txn_id, const char* id_str) {
    if (!id_str) return true;
    uint64_t txn_id_val = 0;
    if (!parse_hex_value(txn_id, txn_id_val)) return false;
    char* end = nullptr;
    uint64_t id_val = strtoull(id_str, &end, 0);
    if (end == id_str) return false;
    return txn_id_val == id_val;
}

const AxiResult* AxiAnalyzer::get_result(const std::string& name) const {
    auto it = results_.find(name);
    if (it != results_.end()) return &it->second;
    return nullptr;
}

AxiResult* AxiAnalyzer::get_result_mut(const std::string& name) {
    auto it = results_.find(name);
    if (it != results_.end()) return &it->second;
    return nullptr;
}

AxiCursor* AxiAnalyzer::get_cursor_mut(const std::string& name) {
    auto it = cursors_.find(name);
    if (it != cursors_.end()) return &it->second;
    return nullptr;
}

struct SigIdx {
    int rst_n = -1;
    int awaddr = -1, awid = -1, awlen = -1, awsize = -1, awburst = -1, awvalid = -1, awready = -1;
    int wdata = -1, wstrb = -1, wlast = -1, wvalid = -1, wready = -1;
    int bid = -1, bresp = -1, bvalid = -1, bready = -1;
    int araddr = -1, arid = -1, arlen = -1, arsize = -1, arburst = -1, arvalid = -1, arready = -1;
    int rid = -1, rdata = -1, rresp = -1, rlast = -1, rvalid = -1, rready = -1;
};

static void add_sig(const std::string& path, int& idx, std::vector<std::string>& signals) {
    if (!path.empty()) {
        idx = (int)signals.size();
        signals.push_back(path);
    } else {
        idx = -1;
    }
}

static bool is_active(const std::string& v) {
    return !v.empty() && v != "0" && v != "X" && v != "Z";
}

static void inc_osd(int& total, std::map<std::string, int>& by_id, const std::string& id) {
    ++total;
    ++by_id[id];
}

static void dec_osd(int& total, std::map<std::string, int>& by_id, const std::string& id) {
    if (total > 0) --total;
    auto it = by_id.find(id);
    if (it != by_id.end()) {
        if (it->second > 0) --it->second;
        if (it->second == 0) by_id.erase(it);
    }
}

struct WBeat {
    npiFsdbTime time;
    std::string data;
    std::string strb;
    bool last = false;
};

struct PendingWrite {
    AxiTransaction txn;
    bool data_complete = false;
};

bool AxiAnalyzer::analyze(const std::string& name, npiFsdbFileHandle file, const AxiConfig& config) {
    if (get_result(name) != nullptr) {
        return true; // already cached
    }

    npiFsdbSigHandle clk_sig = npi_fsdb_sig_by_name(file, config.clk.c_str(), NULL);
    if (!clk_sig) return false;

    npiFsdbVctHandle vct = npi_fsdb_create_vct(clk_sig);
    if (!vct) return false;

    // Build signal vector and index map
    std::vector<std::string> signals;
    signals.reserve(30);
    SigIdx idx;
    add_sig(config.rst_n,   idx.rst_n,   signals);
    add_sig(config.awaddr,  idx.awaddr,  signals);
    add_sig(config.awid,    idx.awid,    signals);
    add_sig(config.awlen,   idx.awlen,   signals);
    add_sig(config.awsize,  idx.awsize,  signals);
    add_sig(config.awburst, idx.awburst, signals);
    add_sig(config.awvalid, idx.awvalid, signals);
    add_sig(config.awready, idx.awready, signals);
    add_sig(config.wdata,   idx.wdata,   signals);
    add_sig(config.wstrb,   idx.wstrb,   signals);
    add_sig(config.wlast,   idx.wlast,   signals);
    add_sig(config.wvalid,  idx.wvalid,  signals);
    add_sig(config.wready,  idx.wready,  signals);
    add_sig(config.bid,     idx.bid,     signals);
    add_sig(config.bresp,   idx.bresp,   signals);
    add_sig(config.bvalid,  idx.bvalid,  signals);
    add_sig(config.bready,  idx.bready,  signals);
    add_sig(config.araddr,  idx.araddr,  signals);
    add_sig(config.arid,    idx.arid,    signals);
    add_sig(config.arlen,   idx.arlen,   signals);
    add_sig(config.arsize,  idx.arsize,  signals);
    add_sig(config.arburst, idx.arburst, signals);
    add_sig(config.arvalid, idx.arvalid, signals);
    add_sig(config.arready, idx.arready, signals);
    add_sig(config.rid,     idx.rid,     signals);
    add_sig(config.rdata,   idx.rdata,   signals);
    add_sig(config.rresp,   idx.rresp,   signals);
    add_sig(config.rlast,   idx.rlast,   signals);
    add_sig(config.rvalid,  idx.rvalid,  signals);
    add_sig(config.rready,  idx.rready,  signals);

    fsdbSigVec_t sig_handles;
    for (const auto& sig_name : signals) {
        npiFsdbSigHandle sig = npi_fsdb_sig_by_name(file, sig_name.c_str(), NULL);
        if (!sig) {
            npi_fsdb_release_vct(vct);
            return false;
        }
        sig_handles.push_back(sig);
    }

    AxiResult result;
    std::deque<WBeat> w_beat_buffer;
    std::list<PendingWrite> pending_writes;
    std::map<std::string, std::deque<AxiTransaction>> pending_reads;
    int read_outstanding = 0;
    int write_outstanding = 0;
    std::map<std::string, int> read_outstanding_by_id;
    std::map<std::string, int> write_outstanding_by_id;

    auto process_edge = [&](npiFsdbTime t, const std::vector<std::string>& values) {
            if (values.size() != signals.size()) return;
            // Reset check
            bool reset_active = false;
            if (idx.rst_n >= 0) {
                const std::string& rst = values[idx.rst_n];
                if (rst.empty() || rst == "0" || rst == "X" || rst == "Z") {
                    reset_active = true;
                }
            }
            if (reset_active) {
                w_beat_buffer.clear();
                pending_writes.clear();
                pending_reads.clear();
                read_outstanding = 0;
                write_outstanding = 0;
                read_outstanding_by_id.clear();
                write_outstanding_by_id.clear();
                return;
            }

            // Detect handshakes
            bool aw_handshake = false;
            if (idx.awvalid >= 0 && idx.awready >= 0) {
                aw_handshake = is_active(values[idx.awvalid]) && is_active(values[idx.awready]);
            }
            bool w_handshake = false;
            if (idx.wvalid >= 0 && idx.wready >= 0) {
                w_handshake = is_active(values[idx.wvalid]) && is_active(values[idx.wready]);
            }
            bool b_handshake = false;
            if (idx.bvalid >= 0 && idx.bready >= 0) {
                b_handshake = is_active(values[idx.bvalid]) && is_active(values[idx.bready]);
            }
            bool ar_handshake = false;
            if (idx.arvalid >= 0 && idx.arready >= 0) {
                ar_handshake = is_active(values[idx.arvalid]) && is_active(values[idx.arready]);
            }
            bool r_handshake = false;
            if (idx.rvalid >= 0 && idx.rready >= 0) {
                r_handshake = is_active(values[idx.rvalid]) && is_active(values[idx.rready]);
            }

            // W handling: push beat into buffer
            if (w_handshake) {
                WBeat beat;
                beat.time = t;
                beat.data = (idx.wdata >= 0) ? values[idx.wdata] : "";
                beat.strb = (idx.wstrb >= 0) ? values[idx.wstrb] : "";
                if (idx.wlast >= 0) {
                    beat.last = is_active(values[idx.wlast]);
                } else {
                    beat.last = true; // AXI4-Lite default
                }
                w_beat_buffer.push_back(std::move(beat));
            }

            // AW handling: create pending write and drain buffer
            if (aw_handshake) {
                PendingWrite pw;
                pw.txn.addr_time = t;
                pw.txn.addr = (idx.awaddr >= 0) ? values[idx.awaddr] : "";
                pw.txn.id = (idx.awid >= 0) ? values[idx.awid] : "0";
                pw.txn.len = (idx.awlen >= 0) ? values[idx.awlen] : "0";
                pw.txn.size = (idx.awsize >= 0) ? values[idx.awsize] : "";
                pw.txn.burst = (idx.awburst >= 0) ? values[idx.awburst] : "";
                pw.txn.is_write = true;
                inc_osd(write_outstanding, write_outstanding_by_id, pw.txn.id);
                pending_writes.push_back(std::move(pw));

                while (!w_beat_buffer.empty()) {
                    PendingWrite* target = nullptr;
                    for (auto& p : pending_writes) {
                        if (!p.data_complete) {
                            target = &p;
                            break;
                        }
                    }
                    if (!target) break;
                    WBeat beat = std::move(w_beat_buffer.front());
                    w_beat_buffer.pop_front();
                    if (target->txn.data.empty()) {
                        target->txn.first_data_time = beat.time;
                    }
                    target->txn.data.push_back(std::move(beat.data));
                    target->txn.wstrb.push_back(std::move(beat.strb));
                    target->txn.last_data_time = beat.time;
                    if (beat.last) {
                        target->data_complete = true;
                    }
                }
            }

            // B handling: match to first pending write with same ID and data complete
            if (b_handshake) {
                std::string b_id_val = (idx.bid >= 0) ? values[idx.bid] : "0";
                for (auto it = pending_writes.begin(); it != pending_writes.end(); ++it) {
                    if (!it->data_complete) continue;
                    if (idx.bid >= 0 && it->txn.id != b_id_val) continue;
                    it->txn.resp_time = t;
                    it->txn.resp = (idx.bresp >= 0) ? values[idx.bresp] : "";
                    dec_osd(write_outstanding, write_outstanding_by_id, it->txn.id);
                    result.writes.push_back(std::move(it->txn));
                    pending_writes.erase(it);
                    break;
                }
            }

            // AR handling
            if (ar_handshake) {
                AxiTransaction txn;
                txn.addr_time = t;
                txn.addr = (idx.araddr >= 0) ? values[idx.araddr] : "";
                txn.id = (idx.arid >= 0) ? values[idx.arid] : "0";
                txn.len = (idx.arlen >= 0) ? values[idx.arlen] : "0";
                txn.size = (idx.arsize >= 0) ? values[idx.arsize] : "";
                txn.burst = (idx.arburst >= 0) ? values[idx.arburst] : "";
                txn.is_write = false;
                inc_osd(read_outstanding, read_outstanding_by_id, txn.id);
                pending_reads[txn.id].push_back(std::move(txn));
            }

            // R handling
            if (r_handshake) {
                std::string r_id_val = (idx.rid >= 0) ? values[idx.rid] : "0";
                auto it_fifo = pending_reads.find(r_id_val);
                if (it_fifo != pending_reads.end() && !it_fifo->second.empty()) {
                    AxiTransaction& txn = it_fifo->second.front();
                    if (txn.data.empty()) {
                        txn.first_data_time = t;
                    }
                    txn.data.push_back((idx.rdata >= 0) ? values[idx.rdata] : "");
                    txn.resp = (idx.rresp >= 0) ? values[idx.rresp] : "";
                    txn.last_data_time = t;
                    bool last = false;
                    if (idx.rlast >= 0) {
                        last = is_active(values[idx.rlast]);
                    } else {
                        last = true; // AXI4-Lite default
                    }
                    if (last) {
                        txn.resp_time = t;
                        dec_osd(read_outstanding, read_outstanding_by_id, txn.id);
                        result.reads.push_back(std::move(txn));
                        it_fifo->second.pop_front();
                        if (it_fifo->second.empty()) {
                            pending_reads.erase(it_fifo);
                        }
                    }
                }
            }

            AxiOutstandingSample sample;
            sample.time = t;
            sample.read = read_outstanding;
            sample.write = write_outstanding;
            sample.read_by_id = read_outstanding_by_id;
            sample.write_by_id = write_outstanding_by_id;
            result.outstanding_samples.push_back(std::move(sample));
    };

    npiFsdbTime min_time = 0;
    npiFsdbTime max_time = 0;
    npi_fsdb_min_time(file, &min_time);
    npi_fsdb_max_time(file, &max_time);

    fsdbSigVec_t all_handles;
    all_handles.push_back(clk_sig);
    for (auto sig : sig_handles) all_handles.push_back(sig);

    fsdbValVec_t init_values;
    std::vector<std::string> values(signals.size());
    std::string prev_clk_val;
    if (npi_fsdb_sig_hdl_vec_value_at(all_handles, min_time, init_values, npiFsdbHexStrVal) &&
        init_values.size() == all_handles.size()) {
        prev_clk_val = init_values[0];
        for (size_t i = 0; i < signals.size(); ++i) values[i] = init_values[i + 1];
    }

    TimeBasedVcIterGuard guard;
    npiFsdbTimeBasedVcIter& iter = guard.iter();
    iter.add(clk_sig);
    for (auto sig : sig_handles) iter.add(sig);
    guard.start(min_time, max_time);

    bool have_group = false;
    bool clk_changed = false;
    std::string old_clk_val;
    std::string new_clk_val;
    npiFsdbTime group_time = 0;
    npiFsdbTime curr_time = 0;
    npiFsdbSigHandle changed_sig = nullptr;

    auto finish_group = [&]() {
        if (!have_group || !clk_changed) return;
        bool is_target_edge = false;
        if (config.posedge && old_clk_val == "0" && new_clk_val == "1") {
            is_target_edge = true;
        } else if (!config.posedge && old_clk_val == "1" && new_clk_val == "0") {
            is_target_edge = true;
        }
        if (is_target_edge) process_edge(group_time, values);
    };

    while (iter.iter_next(curr_time, changed_sig) > 0) {
        if (!have_group) {
            have_group = true;
            group_time = curr_time;
        } else if (curr_time != group_time) {
            finish_group();
            group_time = curr_time;
            clk_changed = false;
        }

        npiFsdbValue val;
        val.format = npiFsdbHexStrVal;
        std::string val_str;
        if (iter.get_value(val) && val.value.str) {
            val_str = val.value.str;
        }

        if (changed_sig == clk_sig) {
            old_clk_val = prev_clk_val;
            new_clk_val = val_str;
            prev_clk_val = val_str;
            clk_changed = true;
        } else {
            for (size_t i = 0; i < sig_handles.size(); ++i) {
                if (sig_handles[i] == changed_sig) {
                    values[i] = val_str;
                    break;
                }
            }
        }
    }
    finish_group();
    npi_fsdb_release_vct(vct);

    // Discard incomplete pending transactions
    pending_writes.clear();
    pending_reads.clear();
    w_beat_buffer.clear();

    // Build all vector and sort by addr_time
    result.all.reserve(result.writes.size() + result.reads.size());
    for (const auto& w : result.writes) result.all.push_back(w);
    for (const auto& r : result.reads) result.all.push_back(r);
    auto cmp = [](const AxiTransaction& a, const AxiTransaction& b) { return a.addr_time < b.addr_time; };
    std::sort(result.all.begin(), result.all.end(), cmp);
    std::sort(result.writes.begin(), result.writes.end(), cmp);
    std::sort(result.reads.begin(), result.reads.end(), cmp);
    result.all_by_resp_time.resize(result.all.size());
    for (size_t i = 0; i < result.all.size(); ++i) result.all_by_resp_time[i] = i;
    std::sort(result.all_by_resp_time.begin(), result.all_by_resp_time.end(),
        [&](size_t lhs, size_t rhs) {
            return result.all[lhs].resp_time < result.all[rhs].resp_time;
        });

    results_[name] = std::move(result);
    cursors_[name] = AxiCursor();
    return true;
}

size_t AxiAnalyzer::get_write_count(const std::string& name) const {
    const AxiResult* r = get_result(name);
    return r ? r->writes.size() : 0;
}

size_t AxiAnalyzer::get_read_count(const std::string& name) const {
    const AxiResult* r = get_result(name);
    return r ? r->reads.size() : 0;
}

bool AxiAnalyzer::get_write_by_addr(const std::string& name, uint64_t addr, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
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

bool AxiAnalyzer::get_write_by_addr_num(const std::string& name, uint64_t addr, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
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

bool AxiAnalyzer::get_write_by_addr_last(const std::string& name, uint64_t addr, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const AxiTransaction* found = nullptr;
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

bool AxiAnalyzer::get_write_by_num(const std::string& name, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || num == 0 || num > r->writes.size()) return false;
    out = &r->writes[num - 1];
    return true;
}

bool AxiAnalyzer::get_write_last(const std::string& name, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || r->writes.empty()) return false;
    out = &r->writes.back();
    return true;
}

bool AxiAnalyzer::get_write_by_addr(const std::string& name, uint64_t addr, const char* id_str, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    for (const auto& txn : r->writes) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr && id_matches(txn.id, id_str)) {
            out = &txn;
            return true;
        }
    }
    return false;
}

bool AxiAnalyzer::get_write_by_addr_num(const std::string& name, uint64_t addr, const char* id_str, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || num == 0) return false;
    size_t count = 0;
    for (const auto& txn : r->writes) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr && id_matches(txn.id, id_str)) {
            if (++count == num) {
                out = &txn;
                return true;
            }
        }
    }
    return false;
}

bool AxiAnalyzer::get_write_by_addr_last(const std::string& name, uint64_t addr, const char* id_str, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const AxiTransaction* found = nullptr;
    for (const auto& txn : r->writes) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr && id_matches(txn.id, id_str)) {
            found = &txn;
        }
    }
    if (!found) return false;
    out = found;
    return true;
}

bool AxiAnalyzer::get_write_by_num(const std::string& name, const char* id_str, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || num == 0) return false;
    size_t count = 0;
    for (const auto& txn : r->writes) {
        if (id_matches(txn.id, id_str) && ++count == num) {
            out = &txn;
            return true;
        }
    }
    return false;
}

bool AxiAnalyzer::get_write_last(const std::string& name, const char* id_str, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const AxiTransaction* found = nullptr;
    for (const auto& txn : r->writes) {
        if (id_matches(txn.id, id_str)) found = &txn;
    }
    if (!found) return false;
    out = found;
    return true;
}

bool AxiAnalyzer::get_read_by_addr(const std::string& name, uint64_t addr, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
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

bool AxiAnalyzer::get_read_by_addr_num(const std::string& name, uint64_t addr, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
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

bool AxiAnalyzer::get_read_by_addr_last(const std::string& name, uint64_t addr, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const AxiTransaction* found = nullptr;
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

bool AxiAnalyzer::get_read_by_num(const std::string& name, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || num == 0 || num > r->reads.size()) return false;
    out = &r->reads[num - 1];
    return true;
}

bool AxiAnalyzer::get_read_last(const std::string& name, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || r->reads.empty()) return false;
    out = &r->reads.back();
    return true;
}

bool AxiAnalyzer::get_read_by_addr(const std::string& name, uint64_t addr, const char* id_str, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    for (const auto& txn : r->reads) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr && id_matches(txn.id, id_str)) {
            out = &txn;
            return true;
        }
    }
    return false;
}

bool AxiAnalyzer::get_read_by_addr_num(const std::string& name, uint64_t addr, const char* id_str, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || num == 0) return false;
    size_t count = 0;
    for (const auto& txn : r->reads) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr && id_matches(txn.id, id_str)) {
            if (++count == num) {
                out = &txn;
                return true;
            }
        }
    }
    return false;
}

bool AxiAnalyzer::get_read_by_addr_last(const std::string& name, uint64_t addr, const char* id_str, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const AxiTransaction* found = nullptr;
    for (const auto& txn : r->reads) {
        uint64_t txn_addr = 0;
        if (parse_hex_value(txn.addr, txn_addr) && txn_addr == addr && id_matches(txn.id, id_str)) {
            found = &txn;
        }
    }
    if (!found) return false;
    out = found;
    return true;
}

bool AxiAnalyzer::get_read_by_num(const std::string& name, const char* id_str, size_t num, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r || num == 0) return false;
    size_t count = 0;
    for (const auto& txn : r->reads) {
        if (id_matches(txn.id, id_str) && ++count == num) {
            out = &txn;
            return true;
        }
    }
    return false;
}

bool AxiAnalyzer::get_read_last(const std::string& name, const char* id_str, const AxiTransaction*& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const AxiTransaction* found = nullptr;
    for (const auto& txn : r->reads) {
        if (id_matches(txn.id, id_str)) found = &txn;
    }
    if (!found) return false;
    out = found;
    return true;
}

bool AxiAnalyzer::cursor_begin(const std::string& name, int filter, const AxiTransaction*& out) {
    AxiResult* r = get_result_mut(name);
    AxiCursor* c = get_cursor_mut(name);
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

bool AxiAnalyzer::cursor_next(const std::string& name, int filter, const AxiTransaction*& out) {
    AxiResult* r = get_result_mut(name);
    AxiCursor* c = get_cursor_mut(name);
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

bool AxiAnalyzer::cursor_prev(const std::string& name, int filter, const AxiTransaction*& out) {
    AxiResult* r = get_result_mut(name);
    AxiCursor* c = get_cursor_mut(name);
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

bool AxiAnalyzer::cursor_last(const std::string& name, int filter, const AxiTransaction*& out) {
    AxiResult* r = get_result_mut(name);
    AxiCursor* c = get_cursor_mut(name);
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

bool AxiAnalyzer::get_latency_stats(const std::string& name, bool is_write,
                                    const AxiTransaction*& max_txn,
                                    const AxiTransaction*& min_txn,
                                    double& avg_latency) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;
    const auto& vec = is_write ? r->writes : r->reads;
    if (vec.empty()) return false;

    const AxiTransaction* max_t = &vec[0];
    const AxiTransaction* min_t = &vec[0];
    double total = 0.0;

    for (const auto& txn : vec) {
        double lat = static_cast<double>(txn.resp_time) - static_cast<double>(txn.addr_time);
        total += lat;
        double max_lat = static_cast<double>(max_t->resp_time) - static_cast<double>(max_t->addr_time);
        double min_lat = static_cast<double>(min_t->resp_time) - static_cast<double>(min_t->addr_time);
        if (lat > max_lat) max_t = &txn;
        if (lat < min_lat) min_t = &txn;
    }

    max_txn = max_t;
    min_txn = min_t;
    avg_latency = total / vec.size();
    return true;
}

bool AxiAnalyzer::get_latency_stats(const std::string& name, int filter, const char* id_str,
                                    AxiStatResult& out) const {
    const AxiResult* r = get_result(name);
    if (!r) return false;

    bool include_wr = (filter == 0 || filter == 1);
    bool include_rd = (filter == 0 || filter == 2);
    double total = 0.0;
    bool seen = false;

    auto visit = [&](const AxiTransaction& txn) {
        if (!id_matches(txn.id, id_str)) return;
        double lat = static_cast<double>(txn.resp_time) - static_cast<double>(txn.addr_time);
        if (!seen) {
            out.max = lat;
            out.min = lat;
            out.max_txn = &txn;
            out.min_txn = &txn;
            seen = true;
        } else {
            if (lat > out.max) {
                out.max = lat;
                out.max_txn = &txn;
            }
            if (lat < out.min) {
                out.min = lat;
                out.min_txn = &txn;
            }
        }
        total += lat;
        ++out.samples;
    };

    if (include_wr) {
        for (const auto& txn : r->writes) visit(txn);
    }
    if (include_rd) {
        for (const auto& txn : r->reads) visit(txn);
    }

    if (!seen || out.samples == 0) return false;
    out.avg = total / static_cast<double>(out.samples);
    return true;
}

bool AxiAnalyzer::get_outstanding_stats(const std::string& name, int filter, const char* id_str,
                                        AxiStatResult& out) const {
    const AxiResult* r = get_result(name);
    if (!r || r->outstanding_samples.empty()) return false;

    bool seen = false;
    double total = 0.0;

    for (const auto& sample : r->outstanding_samples) {
        int value = 0;
        if (id_str) {
            uint64_t want = 0;
            char* end = nullptr;
            want = strtoull(id_str, &end, 0);
            if (end == id_str) return false;

            auto add_matching = [&](const std::map<std::string, int>& m) {
                for (const auto& kv : m) {
                    uint64_t id_val = 0;
                    if (parse_hex_value(kv.first, id_val) && id_val == want) {
                        value += kv.second;
                    }
                }
            };
            if (filter == 0 || filter == 1) add_matching(sample.write_by_id);
            if (filter == 0 || filter == 2) add_matching(sample.read_by_id);
        } else {
            if (filter == 0 || filter == 1) value += sample.write;
            if (filter == 0 || filter == 2) value += sample.read;
        }

        if (!seen) {
            out.max = value;
            out.min = value;
            seen = true;
        } else {
            if (value > out.max) out.max = value;
            if (value < out.min) out.min = value;
        }
        total += value;
        ++out.samples;
    }

    if (!seen || out.samples == 0) return false;
    out.avg = total / static_cast<double>(out.samples);
    return true;
}

bool AxiAnalyzer::get_transactions_in_range(const std::string& name,
                                            npiFsdbTime begin,
                                            npiFsdbTime end,
                                            std::vector<AxiContextTransaction>& out,
                                            int max_results) const {
    out.clear();
    const AxiResult* r = get_result(name);
    if (!r) return false;

    std::set<const AxiTransaction*> emitted;
    auto emit = [&](const AxiTransaction& txn, npiFsdbTime match_time) {
        if (max_results >= 0 && static_cast<int>(out.size()) >= max_results) return;
        if (!emitted.insert(&txn).second) return;
        AxiContextTransaction item;
        item.txn = &txn;
        item.match_time = match_time;
        out.push_back(item);
    };

    auto addr_it = std::lower_bound(r->all.begin(), r->all.end(), begin,
        [](const AxiTransaction& txn, npiFsdbTime t) {
            return txn.addr_time < t;
        });
    for (; addr_it != r->all.end() && addr_it->addr_time <= end; ++addr_it) {
        if (max_results >= 0 && static_cast<int>(out.size()) >= max_results) break;
        emit(*addr_it, addr_it->addr_time);
    }

    auto resp_it = std::lower_bound(r->all_by_resp_time.begin(), r->all_by_resp_time.end(), begin,
        [&](size_t idx, npiFsdbTime t) {
            return r->all[idx].resp_time < t;
        });
    for (; resp_it != r->all_by_resp_time.end(); ++resp_it) {
        if (max_results >= 0 && static_cast<int>(out.size()) >= max_results) break;
        const AxiTransaction& txn = r->all[*resp_it];
        if (txn.resp_time > end) break;
        emit(txn, txn.resp_time);
    }

    std::sort(out.begin(), out.end(), [](const AxiContextTransaction& lhs, const AxiContextTransaction& rhs) {
        return lhs.match_time < rhs.match_time;
    });
    return true;
}

bool AxiAnalyzer::get_outstanding_samples_in_range(const std::string& name,
                                                   npiFsdbTime begin,
                                                   npiFsdbTime end,
                                                   std::vector<AxiOutstandingSample>& out,
                                                   int max_results) const {
    out.clear();
    const AxiResult* r = get_result(name);
    if (!r) return false;
    auto it = std::lower_bound(r->outstanding_samples.begin(), r->outstanding_samples.end(), begin,
        [](const AxiOutstandingSample& sample, npiFsdbTime t) {
            return sample.time < t;
        });
    for (; it != r->outstanding_samples.end() && it->time <= end; ++it) {
        if (max_results >= 0 && static_cast<int>(out.size()) >= max_results) break;
        out.push_back(*it);
    }
    return true;
}

} // namespace xwave
