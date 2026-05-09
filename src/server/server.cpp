#include "server.h"
#include "fsdb_value_reader.h"
#include "../protocol/protocol.h"
#include "../list/signal_list.h"
#include "../apb/apb_analyzer.h"
#include "../apb/apb_manager.h"
#include "../axi/axi_analyzer.h"
#include "../axi/axi_manager.h"
#include "../event/event_analyzer.h"
#include "../event/event_manager.h"
#include "../json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <cctype>
#include <cstdint>

// NPI headers
#include "npi.h"
#include "npi_fsdb.h"
#include "npi_L1.h"

namespace xwave {

using Json = nlohmann::ordered_json;

// Global for cleanup
static int g_session_id = 0;
static int g_srv_fd = -1;
static char g_sock_path[SOCK_PATH_LEN];
static npiFsdbFileHandle g_fsdb_file = nullptr;
static std::string g_fsdb_file_path;
static xwave::ApbAnalyzer g_apb_analyzer;
static xwave::AxiAnalyzer g_axi_analyzer;
static xwave::EventAnalyzer g_event_analyzer;

static void cleanup_and_exit(int sig) {
    if (g_srv_fd >= 0) {
        close(g_srv_fd);
    }
    if (strlen(g_sock_path) > 0) {
        unlink(g_sock_path);
    }
    if (g_fsdb_file) {
        npi_fsdb_close(g_fsdb_file);
        g_fsdb_file = nullptr;
    }
    npi_end();
    exit(0);
}

static void daemonize_io() {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
}

static bool send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static std::string json_response(const Json& j) {
    return j.dump() + "\n" + END_MARKER;
}

// Helper: read a signal list from the registry file by session_id and list_name
static bool read_list_from_registry(int session_id, const char* list_name, SignalList& out_list) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    char path[256];
    snprintf(path, sizeof(path), "%s/.xwave.lists", home);

    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    char line[4096];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        char buf_session_id[16];
        char buf_list_name[256];
        if (sscanf(line, "%15[^|]|%255[^|]", buf_session_id, buf_list_name) != 2) {
            continue;
        }
        if (atoi(buf_session_id) != session_id) continue;
        if (strcmp(buf_list_name, list_name) != 0) continue;

        out_list.name = list_name;
        out_list.signals.clear();

        char* p = strchr(line, '|');
        if (!p) continue;
        p = strchr(p + 1, '|'); // skip session_id and list_name
        while (p) {
            p++;
            char* end = strchr(p, '|');
            if (end) *end = '\0';
            if (strlen(p) > 0) {
                out_list.signals.push_back(p);
            }
            p = end;
        }
        found = true;
        break;
    }

    fclose(fp);
    return found;
}

static std::string format_time(npiFsdbTime t) {
    if (t % 1000000 == 0 && t >= 1000000) {
        return std::to_string(t / 1000000) + "us";
    } else if (t % 1000 == 0 && t >= 1000) {
        return std::to_string(t / 1000) + "ns";
    } else {
        return std::to_string(t) + "ps";
    }
}

static void handle_value(int client_fd, const char* signal_path, npiFsdbTime time, char fmt) {
    std::string value;
    if (read_sig_value_at(g_fsdb_file, signal_path, time, fmt, value)) {
        char fmt_lower = std::tolower(static_cast<unsigned char>(fmt));
        std::string response = std::string("'") + fmt_lower + value + "\n" + END_MARKER;
        send_all(client_fd, response.c_str(), response.length());
    } else {
        std::string err = std::string(ERROR_PREFIX) + "Failed to read value for signal: " + signal_path + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_list_value(int client_fd, const char* list_name, npiFsdbTime time, char fmt, bool json) {
    SignalList list;
    if (!read_list_from_registry(g_session_id, list_name, list)) {
        std::string err = std::string(ERROR_PREFIX) + "List not found: " + list_name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    if (list.signals.empty()) {
        std::string err = std::string(ERROR_PREFIX) + "List is empty: " + list_name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    std::vector<std::string> values;
    if (!read_sig_vec_value_at(g_fsdb_file, list.signals, time, fmt, values)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to read list values\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    std::string response;
    if (json) {
        Json j = Json::object();
        for (size_t i = 0; i < list.signals.size(); ++i) {
            j[list.signals[i]] = values[i];
        }
        response = json_response(j);
    } else {
        char fmt_lower = std::tolower(static_cast<unsigned char>(fmt));
        for (size_t i = 0; i < list.signals.size(); ++i) {
            response += list.signals[i] + ":'" + fmt_lower + values[i] + "\n";
        }
        response += END_MARKER;
    }
    send_all(client_fd, response.c_str(), response.length());
}

// Helper: read an APB config from the registry file by session_id and name
static bool read_apb_from_registry(int session_id, const char* name, xwave::ApbConfig& out_config) {
    xwave::ApbManager am;
    return am.get_apb(session_id, name, out_config);
}

static bool read_axi_from_registry(int session_id, const char* name, xwave::AxiConfig& out_config) {
    xwave::AxiManager am;
    return am.get_axi(session_id, name, out_config);
}

static void handle_list_diff(int client_fd, const char* list_name, npiFsdbTime begin_time, npiFsdbTime end_time) {
    SignalList list;
    if (!read_list_from_registry(g_session_id, list_name, list)) {
        std::string err = std::string(ERROR_PREFIX) + "List not found: " + list_name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    if (list.signals.empty()) {
        std::string err = std::string(ERROR_PREFIX) + "List is empty: " + list_name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    npiFsdbTime diff_time;
    if (find_list_diff(g_fsdb_file, list.signals, begin_time, end_time, diff_time)) {
        std::string response = format_time(diff_time) + "\n" + END_MARKER;
        send_all(client_fd, response.c_str(), response.length());
    } else {
        std::string response = "(no diff found)\n" + std::string(END_MARKER);
        send_all(client_fd, response.c_str(), response.length());
    }
}

static std::string format_apb_txn(const xwave::ApbTransaction* txn) {
    if (!txn) return "";
    return "time=" + format_time(txn->time) + " addr='h" + txn->addr + " data='h" + txn->data;
}

static std::string format_apb_txn_with_type(const xwave::ApbTransaction* txn) {
    if (!txn) return "";
    return "time=" + format_time(txn->time) + " type=" + (txn->is_write ? "WR" : "RD")
           + " addr='h" + txn->addr + " data='h" + txn->data;
}

static std::string format_apb_count_json(size_t count) {
    Json j;
    j["count"] = count;
    return json_response(j);
}

static std::string format_apb_txn_json(const xwave::ApbTransaction* txn) {
    if (!txn) return std::string(END_MARKER);
    Json j;
    j["time"] = format_time(txn->time);
    j["addr"] = "'h" + txn->addr;
    j["data"] = "'h" + txn->data;
    return json_response(j);
}

static std::string format_apb_txn_json_with_type(const xwave::ApbTransaction* txn) {
    if (!txn) return std::string(END_MARKER);
    Json j;
    j["time"] = format_time(txn->time);
    j["type"] = txn->is_write ? "WR" : "RD";
    j["addr"] = "'h" + txn->addr;
    j["data"] = "'h" + txn->data;
    return json_response(j);
}

static void handle_apb_wr(int client_fd, const char* name, const char* addr_str,
                          int num, bool last_flag, bool json) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    const xwave::ApbTransaction* txn = nullptr;
    if (!addr_str && num < 0 && !last_flag) {
        size_t count = g_apb_analyzer.get_write_count(name);
        if (json) {
            std::string resp = format_apb_count_json(count);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = std::to_string(count) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
        return;
    }

    bool ok = false;
    if (addr_str) {
        uint64_t addr = strtoull(addr_str, nullptr, 0);
        if (num > 0) ok = g_apb_analyzer.get_write_by_addr_num(name, addr, (size_t)num, txn);
        else if (last_flag) ok = g_apb_analyzer.get_write_by_addr_last(name, addr, txn);
        else ok = g_apb_analyzer.get_write_by_addr(name, addr, txn);
    } else if (num > 0) {
        ok = g_apb_analyzer.get_write_by_num(name, (size_t)num, txn);
    } else if (last_flag) {
        ok = g_apb_analyzer.get_write_last(name, txn);
    }

    if (ok && txn) {
        if (json) {
            std::string resp = format_apb_txn_json(txn);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = format_apb_txn(txn) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
    } else {
        std::string err = std::string(ERROR_PREFIX) + "Not found\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_apb_rd(int client_fd, const char* name, const char* addr_str,
                          int num, bool last_flag, bool json) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    const xwave::ApbTransaction* txn = nullptr;
    if (!addr_str && num < 0 && !last_flag) {
        size_t count = g_apb_analyzer.get_read_count(name);
        if (json) {
            std::string resp = format_apb_count_json(count);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = std::to_string(count) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
        return;
    }

    bool ok = false;
    if (addr_str) {
        uint64_t addr = strtoull(addr_str, nullptr, 0);
        if (num > 0) ok = g_apb_analyzer.get_read_by_addr_num(name, addr, (size_t)num, txn);
        else if (last_flag) ok = g_apb_analyzer.get_read_by_addr_last(name, addr, txn);
        else ok = g_apb_analyzer.get_read_by_addr(name, addr, txn);
    } else if (num > 0) {
        ok = g_apb_analyzer.get_read_by_num(name, (size_t)num, txn);
    } else if (last_flag) {
        ok = g_apb_analyzer.get_read_last(name, txn);
    }

    if (ok && txn) {
        if (json) {
            std::string resp = format_apb_txn_json(txn);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = format_apb_txn(txn) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
    } else {
        std::string err = std::string(ERROR_PREFIX) + "Not found\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_apb_begin(int client_fd, const char* name, int filter, bool json) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    const xwave::ApbTransaction* txn = nullptr;
    if (g_apb_analyzer.cursor_begin(name, filter, txn) && txn) {
        if (json) {
            std::string resp = format_apb_txn_json_with_type(txn);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = format_apb_txn_with_type(txn) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
    } else {
        std::string err = std::string(ERROR_PREFIX) + "No transaction found\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_apb_next(int client_fd, const char* name, int filter, bool json) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    const xwave::ApbTransaction* txn = nullptr;
    if (g_apb_analyzer.cursor_next(name, filter, txn) && txn) {
        if (json) {
            std::string resp = format_apb_txn_json_with_type(txn);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = format_apb_txn_with_type(txn) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
    } else {
        std::string err = std::string(ERROR_PREFIX) + "No more transactions\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_apb_prev(int client_fd, const char* name, int filter, bool json) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    const xwave::ApbTransaction* txn = nullptr;
    if (g_apb_analyzer.cursor_prev(name, filter, txn) && txn) {
        if (json) {
            std::string resp = format_apb_txn_json_with_type(txn);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = format_apb_txn_with_type(txn) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
    } else {
        std::string err = std::string(ERROR_PREFIX) + "Already at beginning\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_apb_last(int client_fd, const char* name, int filter, bool json) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    const xwave::ApbTransaction* txn = nullptr;
    if (g_apb_analyzer.cursor_last(name, filter, txn) && txn) {
        if (json) {
            std::string resp = format_apb_txn_json_with_type(txn);
            send_all(client_fd, resp.c_str(), resp.length());
        } else {
            std::string resp = format_apb_txn_with_type(txn) + "\n" + END_MARKER;
            send_all(client_fd, resp.c_str(), resp.length());
        }
    } else {
        std::string err = std::string(ERROR_PREFIX) + "No transaction found\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static bool ensure_axi_analyzed(int client_fd, const char* name) {
    xwave::AxiConfig config;
    if (!read_axi_from_registry(g_session_id, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "AXI config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return false;
    }
    if (!g_axi_analyzer.analyze(name, g_fsdb_file, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to analyze AXI: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return false;
    }
    return true;
}

static Json json_array_hex(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (size_t i = 0; i < values.size(); ++i) {
        out.push_back("'h" + values[i]);
    }
    return out;
}

static std::string format_axi_txn(const xwave::AxiTransaction* txn) {
    if (!txn) return "";
    std::string out = "addr_time=" + format_time(txn->addr_time)
        + " type=" + (txn->is_write ? "WR" : "RD")
        + " id='h" + txn->id
        + " addr='h" + txn->addr
        + " len='h" + txn->len
        + " beats=" + std::to_string(txn->data.size())
        + " first_data_time=" + format_time(txn->first_data_time)
        + " last_data_time=" + format_time(txn->last_data_time)
        + " resp_time=" + format_time(txn->resp_time)
        + " resp='h" + txn->resp;
    if (!txn->data.empty()) out += " data0='h" + txn->data.front();
    return out;
}

static std::string format_axi_txn_json(const xwave::AxiTransaction* txn) {
    if (!txn) return std::string(END_MARKER);
    Json j;
    j["addr_time"] = format_time(txn->addr_time);
    j["type"] = txn->is_write ? "WR" : "RD";
    j["id"] = "'h" + txn->id;
    j["addr"] = "'h" + txn->addr;
    j["len"] = "'h" + txn->len;
    j["size"] = "'h" + txn->size;
    j["burst"] = "'h" + txn->burst;
    j["beats"] = txn->data.size();
    j["first_data_time"] = format_time(txn->first_data_time);
    j["last_data_time"] = format_time(txn->last_data_time);
    j["resp_time"] = format_time(txn->resp_time);
    j["resp"] = "'h" + txn->resp;
    j["data"] = json_array_hex(txn->data);
    j["wstrb"] = json_array_hex(txn->wstrb);
    return json_response(j);
}

static std::string format_axi_count_json(size_t count) {
    Json j;
    j["count"] = count;
    return json_response(j);
}

static size_t count_axi_by_id(const char* name, bool is_write, const char* id_str) {
    if (!id_str) return is_write ? g_axi_analyzer.get_write_count(name) : g_axi_analyzer.get_read_count(name);
    size_t count = 0;
    const xwave::AxiTransaction* txn = nullptr;
    while (true) {
        bool ok = is_write
            ? g_axi_analyzer.get_write_by_num(name, id_str, count + 1, txn)
            : g_axi_analyzer.get_read_by_num(name, id_str, count + 1, txn);
        if (!ok) break;
        ++count;
    }
    return count;
}

static void send_axi_txn_or_error(int client_fd, bool ok, const xwave::AxiTransaction* txn, bool json) {
    if (ok && txn) {
        std::string resp = json ? format_axi_txn_json(txn) : (format_axi_txn(txn) + "\n" + END_MARKER);
        send_all(client_fd, resp.c_str(), resp.length());
    } else {
        std::string err = std::string(ERROR_PREFIX) + "Not found\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_axi_rw(int client_fd, const char* name, bool is_write, const char* addr_str,
                          const char* id_str, int num, bool last_flag, bool json) {
    if (!ensure_axi_analyzed(client_fd, name)) return;
    const xwave::AxiTransaction* txn = nullptr;
    if (!addr_str && num < 0 && !last_flag) {
        size_t count = count_axi_by_id(name, is_write, id_str);
        std::string resp = json ? format_axi_count_json(count) : (std::to_string(count) + "\n" + END_MARKER);
        send_all(client_fd, resp.c_str(), resp.length());
        return;
    }

    bool ok = false;
    if (addr_str) {
        uint64_t addr = strtoull(addr_str, nullptr, 0);
        if (is_write) {
            if (num > 0) ok = g_axi_analyzer.get_write_by_addr_num(name, addr, id_str, (size_t)num, txn);
            else if (last_flag) ok = g_axi_analyzer.get_write_by_addr_last(name, addr, id_str, txn);
            else ok = g_axi_analyzer.get_write_by_addr(name, addr, id_str, txn);
        } else {
            if (num > 0) ok = g_axi_analyzer.get_read_by_addr_num(name, addr, id_str, (size_t)num, txn);
            else if (last_flag) ok = g_axi_analyzer.get_read_by_addr_last(name, addr, id_str, txn);
            else ok = g_axi_analyzer.get_read_by_addr(name, addr, id_str, txn);
        }
    } else if (num > 0) {
        ok = is_write
            ? g_axi_analyzer.get_write_by_num(name, id_str, (size_t)num, txn)
            : g_axi_analyzer.get_read_by_num(name, id_str, (size_t)num, txn);
    } else if (last_flag) {
        ok = is_write
            ? g_axi_analyzer.get_write_last(name, id_str, txn)
            : g_axi_analyzer.get_read_last(name, id_str, txn);
    }
    send_axi_txn_or_error(client_fd, ok, txn, json);
}

static void handle_axi_cursor(int client_fd, const char* name, int cmd_type, int filter, bool json) {
    if (!ensure_axi_analyzed(client_fd, name)) return;
    const xwave::AxiTransaction* txn = nullptr;
    bool ok = false;
    if (cmd_type == 1) ok = g_axi_analyzer.cursor_begin(name, filter, txn);
    else if (cmd_type == 2) ok = g_axi_analyzer.cursor_next(name, filter, txn);
    else if (cmd_type == 3) ok = g_axi_analyzer.cursor_prev(name, filter, txn);
    else ok = g_axi_analyzer.cursor_last(name, filter, txn);

    if (ok && txn) {
        std::string resp = json ? format_axi_txn_json(txn) : (format_axi_txn(txn) + "\n" + END_MARKER);
        send_all(client_fd, resp.c_str(), resp.length());
    } else {
        std::string err = std::string(ERROR_PREFIX) + "No transaction found\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static std::string format_axi_stat(const char* label, const xwave::AxiStatResult& stat, bool json) {
    if (json) {
        Json j;
        j[label] = {
            {"max", stat.max},
            {"min", stat.min},
            {"avg", stat.avg},
            {"samples", stat.samples}
        };
        return json_response(j);
    }
    return std::string(label) + " max=" + std::to_string(stat.max)
        + " min=" + std::to_string(stat.min)
        + " avg=" + std::to_string(stat.avg)
        + " samples=" + std::to_string(stat.samples) + "\n" + END_MARKER;
}

static void handle_axi_stat(int client_fd, const char* name, bool latency, int filter,
                            const char* id_str, bool json) {
    if (!ensure_axi_analyzed(client_fd, name)) return;
    xwave::AxiStatResult stat;
    bool ok = latency
        ? g_axi_analyzer.get_latency_stats(name, filter, id_str, stat)
        : g_axi_analyzer.get_outstanding_stats(name, filter, id_str, stat);
    if (!ok) {
        std::string err = std::string(ERROR_PREFIX) + "No data\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    std::string resp = format_axi_stat(latency ? "latency" : "outstanding", stat, json);
    send_all(client_fd, resp.c_str(), resp.length());
}

static Json event_record_to_json(const xwave::EventRecord& rec) {
    Json j;
    j["time"] = format_time(rec.time);
    j["time_ps"] = rec.time;
    j["signals"] = rec.signals;
    j["fields"] = rec.fields;
    return j;
}

static std::string format_event_records_text(const std::vector<xwave::EventRecord>& records) {
    if (records.empty()) return std::string("(no event found)\n") + END_MARKER;
    std::string out;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        out += "idx=" + std::to_string(i) + " time=" + format_time(rec.time);
        for (const auto& kv : rec.signals) out += " " + kv.first + "=" + kv.second;
        for (const auto& kv : rec.fields) out += " " + kv.first + "=" + kv.second;
        out += "\n";
    }
    out += END_MARKER;
    return out;
}

static std::string format_event_records_json(const std::vector<xwave::EventRecord>& records) {
    Json j = Json::array();
    for (const auto& rec : records) j.push_back(event_record_to_json(rec));
    return json_response(j);
}

static void handle_event_query(int client_fd,
                               const char* name,
                               npiFsdbTime begin_time,
                               npiFsdbTime end_time,
                               int limit,
                               bool use_json,
                               const char* expr) {
    xwave::EventManager em;
    xwave::EventConfig config;
    if (!em.get_event(g_session_id, g_fsdb_file_path, name, config)) {
        std::string err = std::string(ERROR_PREFIX) + "Event config not found: " + name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    xwave::EventQuery query;
    query.expr = expr ? expr : "";
    query.begin = begin_time;
    query.end = end_time;
    query.limit = limit;
    std::vector<xwave::EventRecord> records;
    std::string error;
    if (!g_event_analyzer.analyze(g_fsdb_file, config, query, records, error)) {
        std::string err = std::string(ERROR_PREFIX) + error + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    std::string resp = use_json ? format_event_records_json(records) : format_event_records_text(records);
    send_all(client_fd, resp.c_str(), resp.length());
}

static bool handle_client(int client_fd, bool& should_quit) {
    should_quit = false;

    // Read command line
    char line[4096] = {};
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(line) - 1) {
        ssize_t n = read(client_fd, line + total, 1);
        if (n <= 0) return false;
        if (line[total] == '\n') break;
        total++;
    }
    line[total] = '\0';

    // Trim whitespace
    char* cmd = line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r' || cmd[len-1] == ' ')) {
        cmd[len-1] = '\0';
        len--;
    }

    // Handle QUIT
    if (strcmp(cmd, CMD_QUIT) == 0) {
        send_all(client_fd, END_MARKER, strlen(END_MARKER));
        should_quit = true;
        return true;
    }

    // Handle PING
    if (strcmp(cmd, CMD_PING) == 0) {
        const char* pong = "PONG\n" END_MARKER;
        send_all(client_fd, pong, strlen(pong));
        return true;
    }

    // Handle VALUE <signal> <time> <fmt>
    if (strncmp(cmd, CMD_VALUE, strlen(CMD_VALUE)) == 0) {
        char signal_path[1024];
        char time_str[64];
        char fmt;
        if (sscanf(cmd + strlen(CMD_VALUE), " %1023s %63s %c", signal_path, time_str, &fmt) == 3) {
            npiFsdbTime t = strtoull(time_str, nullptr, 10);
            handle_value(client_fd, signal_path, t, fmt);
        } else {
            const char* err = ERROR_PREFIX "Usage: VALUE <signal> <time> <fmt>\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle LIST_VALUE <list_name> <time> <fmt> [json]
    if (strncmp(cmd, CMD_LIST_VALUE, strlen(CMD_LIST_VALUE)) == 0) {
        char list_name[256];
        char time_str[64];
        char fmt[16];
        if (sscanf(cmd + strlen(CMD_LIST_VALUE), " %255s %63s %15s", list_name, time_str, fmt) >= 3) {
            npiFsdbTime t = strtoull(time_str, nullptr, 10);
            bool json = (strstr(cmd, "json") != nullptr);
            handle_list_value(client_fd, list_name, t, fmt[0], json);
        } else {
            const char* err = ERROR_PREFIX "Usage: LIST_VALUE <list> <time> <fmt> [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle LIST_DIFF <list_name> <begin_time> <end_time>
    if (strncmp(cmd, CMD_LIST_DIFF, strlen(CMD_LIST_DIFF)) == 0) {
        char list_name[256];
        char begin_str[64];
        char end_str[64];
        if (sscanf(cmd + strlen(CMD_LIST_DIFF), " %255s %63s %63s", list_name, begin_str, end_str) == 3) {
            npiFsdbTime begin = strtoull(begin_str, nullptr, 10);
            npiFsdbTime end = strtoull(end_str, nullptr, 10);
            handle_list_diff(client_fd, list_name, begin, end);
        } else {
            const char* err = ERROR_PREFIX "Usage: LIST_DIFF <list> <begin> <end>\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle APB_WR <name> [addr <addr>] [num <x>] [last] [json]
    if (strncmp(cmd, CMD_APB_WR, strlen(CMD_APB_WR)) == 0) {
        char name[256] = {};
        char addr_arg[64] = {};
        char num_arg[64] = {};
        bool has_addr = false, has_num = false, has_last = false, use_json = false;
        const char* p = cmd + strlen(CMD_APB_WR);
        if (sscanf(p, " %255s", name) >= 1) {
            const char* rest = strstr(p, name) + strlen(name);
            if (strstr(rest, "addr")) {
                if (sscanf(strstr(rest, "addr") + 4, " %63s", addr_arg) == 1) has_addr = true;
            }
            if (strstr(rest, "num")) {
                if (sscanf(strstr(rest, "num") + 3, " %63s", num_arg) == 1) has_num = true;
            }
            if (strstr(rest, "last")) has_last = true;
            if (strstr(rest, "json")) use_json = true;
            handle_apb_wr(client_fd, name, has_addr ? addr_arg : nullptr,
                          has_num ? atoi(num_arg) : -1, has_last, use_json);
        } else {
            const char* err = ERROR_PREFIX "Usage: APB_WR <name> [addr <a>] [num <x>] [last] [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle APB_RD <name> [addr <addr>] [num <x>] [last] [json]
    if (strncmp(cmd, CMD_APB_RD, strlen(CMD_APB_RD)) == 0) {
        char name[256] = {};
        char addr_arg[64] = {};
        char num_arg[64] = {};
        bool has_addr = false, has_num = false, has_last = false, use_json = false;
        const char* p = cmd + strlen(CMD_APB_RD);
        if (sscanf(p, " %255s", name) >= 1) {
            const char* rest = strstr(p, name) + strlen(name);
            if (strstr(rest, "addr")) {
                if (sscanf(strstr(rest, "addr") + 4, " %63s", addr_arg) == 1) has_addr = true;
            }
            if (strstr(rest, "num")) {
                if (sscanf(strstr(rest, "num") + 3, " %63s", num_arg) == 1) has_num = true;
            }
            if (strstr(rest, "last")) has_last = true;
            if (strstr(rest, "json")) use_json = true;
            handle_apb_rd(client_fd, name, has_addr ? addr_arg : nullptr,
                          has_num ? atoi(num_arg) : -1, has_last, use_json);
        } else {
            const char* err = ERROR_PREFIX "Usage: APB_RD <name> [addr <a>] [num <x>] [last] [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle APB_BEGIN|NEXT|PREV|LAST <name> [all|wr|rd] [json]
    if (strncmp(cmd, CMD_APB_BEGIN, strlen(CMD_APB_BEGIN)) == 0 ||
        strncmp(cmd, CMD_APB_NEXT, strlen(CMD_APB_NEXT)) == 0 ||
        strncmp(cmd, CMD_APB_PREV, strlen(CMD_APB_PREV)) == 0 ||
        strncmp(cmd, CMD_APB_LAST, strlen(CMD_APB_LAST)) == 0) {
        size_t base_len = 0;
        int cmd_type = 0;
        if (strncmp(cmd, CMD_APB_BEGIN, strlen(CMD_APB_BEGIN)) == 0) { base_len = strlen(CMD_APB_BEGIN); cmd_type = 1; }
        else if (strncmp(cmd, CMD_APB_NEXT, strlen(CMD_APB_NEXT)) == 0) { base_len = strlen(CMD_APB_NEXT); cmd_type = 2; }
        else if (strncmp(cmd, CMD_APB_PREV, strlen(CMD_APB_PREV)) == 0) { base_len = strlen(CMD_APB_PREV); cmd_type = 3; }
        else { base_len = strlen(CMD_APB_LAST); cmd_type = 4; }

        char name[256] = {};
        char filter_str[16] = {};
        bool use_json = false;
        const char* p = cmd + base_len;
        if (sscanf(p, " %255s %15s", name, filter_str) >= 1) {
            const char* rest = strstr(p, name);
            if (rest) rest += strlen(name);
            if (strstr(cmd, "json")) use_json = true;

            int filter = 0; // all
            if (strcmp(filter_str, "wr") == 0) filter = 1;
            else if (strcmp(filter_str, "rd") == 0) filter = 2;

            if (cmd_type == 1) handle_apb_begin(client_fd, name, filter, use_json);
            else if (cmd_type == 2) handle_apb_next(client_fd, name, filter, use_json);
            else if (cmd_type == 3) handle_apb_prev(client_fd, name, filter, use_json);
            else handle_apb_last(client_fd, name, filter, use_json);
        } else {
            const char* err = ERROR_PREFIX "Usage: APB_{BEGIN|NEXT|PREV|LAST} <name> [all|wr|rd] [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle AXI_WR|AXI_RD <name> [addr <addr>] [id <id>] [num <x>] [last] [json]
    if (strncmp(cmd, CMD_AXI_WR, strlen(CMD_AXI_WR)) == 0 ||
        strncmp(cmd, CMD_AXI_RD, strlen(CMD_AXI_RD)) == 0) {
        bool is_write = strncmp(cmd, CMD_AXI_WR, strlen(CMD_AXI_WR)) == 0;
        size_t base_len = is_write ? strlen(CMD_AXI_WR) : strlen(CMD_AXI_RD);
        char name[256] = {};
        char addr_arg[64] = {};
        char id_arg[64] = {};
        char num_arg[64] = {};
        bool has_addr = false, has_id = false, has_num = false, has_last = false, use_json = false;
        const char* p = cmd + base_len;
        if (sscanf(p, " %255s", name) >= 1) {
            const char* rest = strstr(p, name) + strlen(name);
            const char* addr_p = strstr(rest, "addr");
            if (addr_p && sscanf(addr_p + 4, " %63s", addr_arg) == 1) has_addr = true;
            const char* id_p = strstr(rest, "id");
            if (id_p && sscanf(id_p + 2, " %63s", id_arg) == 1) has_id = true;
            const char* num_p = strstr(rest, "num");
            if (num_p && sscanf(num_p + 3, " %63s", num_arg) == 1) has_num = true;
            if (strstr(rest, "last")) has_last = true;
            if (strstr(rest, "json")) use_json = true;
            handle_axi_rw(client_fd, name, is_write, has_addr ? addr_arg : nullptr,
                          has_id ? id_arg : nullptr, has_num ? atoi(num_arg) : -1,
                          has_last, use_json);
        } else {
            const char* err = ERROR_PREFIX "Usage: AXI_WR|AXI_RD <name> [addr <a>] [id <id>] [num <x>] [last] [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle AXI_BEGIN|NEXT|PREV|LAST <name> [all|wr|rd] [json]
    if (strncmp(cmd, CMD_AXI_BEGIN, strlen(CMD_AXI_BEGIN)) == 0 ||
        strncmp(cmd, CMD_AXI_NEXT, strlen(CMD_AXI_NEXT)) == 0 ||
        strncmp(cmd, CMD_AXI_PREV, strlen(CMD_AXI_PREV)) == 0 ||
        strncmp(cmd, CMD_AXI_LAST, strlen(CMD_AXI_LAST)) == 0) {
        size_t base_len = 0;
        int cmd_type = 0;
        if (strncmp(cmd, CMD_AXI_BEGIN, strlen(CMD_AXI_BEGIN)) == 0) { base_len = strlen(CMD_AXI_BEGIN); cmd_type = 1; }
        else if (strncmp(cmd, CMD_AXI_NEXT, strlen(CMD_AXI_NEXT)) == 0) { base_len = strlen(CMD_AXI_NEXT); cmd_type = 2; }
        else if (strncmp(cmd, CMD_AXI_PREV, strlen(CMD_AXI_PREV)) == 0) { base_len = strlen(CMD_AXI_PREV); cmd_type = 3; }
        else { base_len = strlen(CMD_AXI_LAST); cmd_type = 4; }

        char name[256] = {};
        char filter_str[16] = {};
        bool use_json = false;
        const char* p = cmd + base_len;
        if (sscanf(p, " %255s %15s", name, filter_str) >= 1) {
            if (strstr(cmd, "json")) use_json = true;
            int filter = 0;
            if (strcmp(filter_str, "wr") == 0) filter = 1;
            else if (strcmp(filter_str, "rd") == 0) filter = 2;
            handle_axi_cursor(client_fd, name, cmd_type, filter, use_json);
        } else {
            const char* err = ERROR_PREFIX "Usage: AXI_{BEGIN|NEXT|PREV|LAST} <name> [all|wr|rd] [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle AXI_LATENCY|AXI_OSD <name> [all|wr|rd] [id <id>] [json]
    if (strncmp(cmd, CMD_AXI_LATENCY, strlen(CMD_AXI_LATENCY)) == 0 ||
        strncmp(cmd, CMD_AXI_OSD, strlen(CMD_AXI_OSD)) == 0) {
        bool latency = strncmp(cmd, CMD_AXI_LATENCY, strlen(CMD_AXI_LATENCY)) == 0;
        size_t base_len = latency ? strlen(CMD_AXI_LATENCY) : strlen(CMD_AXI_OSD);
        char name[256] = {};
        char filter_str[16] = {};
        char id_arg[64] = {};
        bool has_id = false;
        bool use_json = false;
        const char* p = cmd + base_len;
        if (sscanf(p, " %255s %15s", name, filter_str) >= 1) {
            int filter = 0;
            if (strcmp(filter_str, "wr") == 0) filter = 1;
            else if (strcmp(filter_str, "rd") == 0) filter = 2;
            const char* rest = strstr(p, name) + strlen(name);
            const char* id_p = strstr(rest, "id");
            if (id_p && sscanf(id_p + 2, " %63s", id_arg) == 1) has_id = true;
            if (strstr(rest, "json")) use_json = true;
            handle_axi_stat(client_fd, name, latency, filter, has_id ? id_arg : nullptr, use_json);
        } else {
            const char* err = ERROR_PREFIX "Usage: AXI_LATENCY|AXI_OSD <name> [all|wr|rd] [id <id>] [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle EVENT_FIND|EVENT_EXPORT <name> <begin> <end> <limit> <json|text> expr <expr>
    if (strncmp(cmd, CMD_EVENT_FIND, strlen(CMD_EVENT_FIND)) == 0 ||
        strncmp(cmd, CMD_EVENT_EXPORT, strlen(CMD_EVENT_EXPORT)) == 0) {
        bool find = strncmp(cmd, CMD_EVENT_FIND, strlen(CMD_EVENT_FIND)) == 0;
        size_t base_len = find ? strlen(CMD_EVENT_FIND) : strlen(CMD_EVENT_EXPORT);
        char name[256] = {};
        unsigned long long begin = 0;
        unsigned long long end = 0;
        int limit = -1;
        char mode[16] = {};
        const char* p = cmd + base_len;
        int matched = sscanf(p, " %255s %llu %llu %d %15s", name, &begin, &end, &limit, mode);
        const char* expr_p = strstr(p, " expr ");
        if (matched >= 5 && expr_p) {
            expr_p += strlen(" expr ");
            bool use_json = strcmp(mode, "json") == 0;
            if (find) limit = 1;
            handle_event_query(client_fd,
                               name,
                               static_cast<npiFsdbTime>(begin),
                               static_cast<npiFsdbTime>(end),
                               limit,
                               use_json,
                               expr_p);
        } else {
            const char* err = ERROR_PREFIX "Usage: EVENT_FIND|EVENT_EXPORT <name> <begin> <end> <limit> <json|text> expr <expr>\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Unknown command
    const char* err = ERROR_PREFIX "Unknown command\n" END_MARKER;
    send_all(client_fd, err, strlen(err));
    return true;
}

int server_main(int argc, char** argv) {
    // argv: [exe, session_id, fsdb_file]
    if (argc < 3) {
        fprintf(stderr, "Server mode requires session_id and fsdb_file arguments\n");
        return 1;
    }

    int arg_idx = 1;

    // Parse session ID
    g_session_id = atoi(argv[arg_idx]);
    if (g_session_id <= 0) {
        fprintf(stderr, "Invalid session ID: %s\n", argv[arg_idx]);
        return 1;
    }
    arg_idx++;

    // Parse FSDB file
    const char* fsdb_file = argv[arg_idx];
    g_fsdb_file_path = fsdb_file;

    // Redirect stdout to capture NPI init messages, but keep a copy
    int stdout_copy = dup(STDOUT_FILENO);

    // Initialize NPI
    int npi_argc = 1;
    char** npi_argv = argv;
    int result = npi_init(npi_argc, npi_argv);
    if (result == 0) {
        dprintf(stdout_copy, "[Session %d] ERROR: npi_init failed\n", g_session_id);
        close(stdout_copy);
        return 1;
    }

    g_fsdb_file = npi_fsdb_open(fsdb_file);
    if (!g_fsdb_file) {
        dprintf(stdout_copy, "[Session %d] ERROR: npi_fsdb_open failed: %s\n", g_session_id, fsdb_file);
        npi_end();
        close(stdout_copy);
        return 1;
    }

    npiFsdbTime minTime, maxTime;
    npi_fsdb_min_time(g_fsdb_file, &minTime);
    npi_fsdb_max_time(g_fsdb_file, &maxTime);

    dprintf(stdout_copy, "[Session %d] Ready (FSDB: %llu ~ %llu)\n", g_session_id, minTime, maxTime);
    fflush(stdout);
    close(stdout_copy);

    // Now daemonize I/O
    daemonize_io();

    // Set up signal handlers
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);

    // Create socket
    g_srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv_fd < 0) {
        npi_fsdb_close(g_fsdb_file);
        npi_end();
        return 1;
    }

    get_sock_path(g_sock_path, g_session_id);
    unlink(g_sock_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(g_srv_fd);
        npi_fsdb_close(g_fsdb_file);
        npi_end();
        return 1;
    }
    chmod(g_sock_path, 0600);

    if (listen(g_srv_fd, 8) < 0) {
        close(g_srv_fd);
        unlink(g_sock_path);
        npi_fsdb_close(g_fsdb_file);
        npi_end();
        return 1;
    }

    // Accept loop
    while (true) {
        int client_fd = accept(g_srv_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        bool quit = false;
        handle_client(client_fd, quit);
        close(client_fd);

        if (quit) break;
    }

    // Cleanup
    close(g_srv_fd);
    unlink(g_sock_path);
    if (g_fsdb_file) {
        npi_fsdb_close(g_fsdb_file);
        g_fsdb_file = nullptr;
    }
    npi_end();

    return 0;
}

} // namespace xwave
