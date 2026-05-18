#include "server.h"
#include "fsdb_value_reader.h"
#include "fsdb_scan_utils.h"
#include "../protocol/protocol.h"
#include "../list/list_manager.h"
#include "../list/signal_list.h"
#include "../apb/apb_analyzer.h"
#include "../apb/apb_manager.h"
#include "../axi/axi_analyzer.h"
#include "../axi/axi_manager.h"
#include "../event/event_analyzer.h"
#include "../event/event_expr.h"
#include "../event/event_manager.h"
#include "../cursor/cursor_manager.h"
#include "../common/time_spec.h"
#include "../session/session_registry.h"
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
#include <ctime>
#include <cerrno>
#include <cmath>
#include <strings.h>
#include <algorithm>
#include <map>
#include <sstream>
#include <functional>
#include <cstdarg>

// NPI headers
#include "npi.h"
#include "npi_fsdb.h"
#include "npi_L1.h"

namespace xwave {

using Json = nlohmann::ordered_json;

// Global for cleanup
static std::string g_session_id;
static int g_srv_fd = -1;
static char g_sock_path[SOCK_PATH_LEN];
static npiFsdbFileHandle g_fsdb_file = nullptr;
static std::string g_fsdb_file_path;
static long g_fsdb_mtime = 0;
static long long g_fsdb_size = 0;
static unsigned long long g_fsdb_dev = 0;
static unsigned long long g_fsdb_inode = 0;
static xwave::ApbAnalyzer g_apb_analyzer;
static xwave::AxiAnalyzer g_axi_analyzer;
static xwave::EventAnalyzer g_event_analyzer;
static FILE* g_debug_log = nullptr;

static bool server_debug_enabled() {
    const char* env = getenv("XWAVE_DEBUG");
    return env && env[0] != '\0' && strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 && strcasecmp(env, "off") != 0;
}

static void server_debug_open_log() {
    if (!server_debug_enabled() || g_session_id.empty()) return;
    char log_path[SOCK_PATH_LEN];
    get_debug_log_path(log_path, g_session_id);
    g_debug_log = fopen(log_path, "a");
    if (g_debug_log) {
        time_t now = time(nullptr);
        fprintf(g_debug_log, "=== xwave server debug session=%s time=%ld ===\n",
                g_session_id.c_str(), static_cast<long>(now));
        fflush(g_debug_log);
    }
}

static void server_debug_log(const char* fmt, ...) {
    if (!g_debug_log) return;
    fprintf(g_debug_log, "[server] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_debug_log, fmt, ap);
    va_end(ap);
    fprintf(g_debug_log, "\n");
    fflush(g_debug_log);
}

static void cleanup_and_exit(int sig) {
    server_debug_log("cleanup_and_exit: signal=%d", sig);
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
    if (!g_session_id.empty()) {
        SessionRegistry registry;
        registry.remove(g_session_id);
    }
    npi_end();
    if (g_debug_log) {
        server_debug_log("cleanup_and_exit: done");
        fclose(g_debug_log);
        g_debug_log = nullptr;
    }
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

static bool contains_xz_value(const std::string& value) {
    for (char c : value) {
        if (c == 'x' || c == 'X' || c == 'z' || c == 'Z') return true;
    }
    return false;
}

static std::string with_value_prefix(const std::string& value, char prefix) {
    if (value.size() >= 2 && value[0] == '\'') return value;
    char p = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix)));
    return std::string("'") + p + value;
}

static Json wave_value_json(const std::string& raw, char prefix = 'b') {
    Json v;
    std::string text = with_value_prefix(raw, prefix);
    v["text"] = text;
    v["known"] = !contains_xz_value(text);
    if (!v["known"].get<bool>()) v["unknown_reason"] = "contains_xz";
    if (text.size() >= 2 && text[0] == '\'' && (text[1] == 'b' || text[1] == 'B')) v["bits"] = text.substr(2);
    if (text.size() >= 2 && text[0] == '\'' && (text[1] == 'h' || text[1] == 'H')) v["hex"] = "0x" + text.substr(2);
    if (v["known"].get<bool>()) {
        std::string bits = xwave::expr_bits_only(text);
        unsigned long long u = 0;
        for (char c : bits) {
            u = (u << 1) | (c == '1' ? 1ULL : 0ULL);
        }
        v["unsigned"] = u;
    }
    return v;
}

static bool stat_fsdb(long& mtime,
                      long long& size,
                      unsigned long long& dev,
                      unsigned long long& inode) {
    struct stat st;
    if (stat(g_fsdb_file_path.c_str(), &st) != 0) return false;
    mtime = static_cast<long>(st.st_mtime);
    size = static_cast<long long>(st.st_size);
    dev = static_cast<unsigned long long>(st.st_dev);
    inode = static_cast<unsigned long long>(st.st_ino);
    return true;
}

static bool fsdb_changed() {
    long mtime = 0;
    long long size = 0;
    unsigned long long dev = 0;
    unsigned long long inode = 0;
    if (!stat_fsdb(mtime, size, dev, inode)) return true;
    return mtime != g_fsdb_mtime || size != g_fsdb_size ||
           dev != g_fsdb_dev || inode != g_fsdb_inode;
}

static void send_error(int client_fd, const std::string& message) {
    std::string err = std::string(ERROR_PREFIX) + message + "\n" + END_MARKER;
    send_all(client_fd, err.c_str(), err.length());
}

static std::string fsdb_time_scale() {
    const char* scale = g_fsdb_file ? npi_fsdb_time_scale_unit(g_fsdb_file) : nullptr;
    return scale ? scale : "unknown";
}

static bool parse_user_time(const char* text,
                            bool allow_max,
                            npiFsdbTime& out_time,
                            std::string& error) {
    if (!text || text[0] == '\0') {
        error = "Invalid time: empty";
        return false;
    }
    if (allow_max && (strcasecmp(text, "max") == 0 || strcasecmp(text, "inf") == 0)) {
        out_time = 0xFFFFFFFFFFFFFFFFULL;
        return true;
    }
    ParsedTimeSpec spec;
    if (!parse_time_spec_text(text, spec, error)) return false;

    auto convert_abs = [&](const std::string& source, npiFsdbTime& out) -> bool {
        if (source.empty() || source[0] == '-') {
            error = std::string("Invalid time '") + source + "': negative time is not allowed";
            return false;
        }
        char* end = nullptr;
        errno = 0;
        double value = strtod(source.c_str(), &end);
        if (errno != 0 || end == source.c_str() || !std::isfinite(value) || value < 0) {
            error = std::string("Invalid time '") + source + "'";
            return false;
        }
        while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        const char* unit = "ns";
        if (*end != '\0') {
            if (strcasecmp(end, "us") == 0) unit = "us";
            else if (strcasecmp(end, "ns") == 0) unit = "ns";
            else if (strcasecmp(end, "ps") == 0) unit = "ps";
            else if (strcasecmp(end, "fs") == 0) unit = "fs";
            else {
                error = std::string("Invalid time '") + source
                      + "': unsupported unit, expected us/ns/ps/fs (FSDB scale "
                      + fsdb_time_scale() + ")";
                return false;
            }
        }
        npiFsdbTime converted = 0;
        if (!g_fsdb_file || !npi_fsdb_convert_time_in(g_fsdb_file, value, unit, converted)) {
            error = std::string("Failed to convert time '") + source
                  + "' for FSDB scale " + fsdb_time_scale();
            return false;
        }
        out = converted;
        return true;
    };

    if (spec.kind == TimeSpecKind::Absolute) {
        return convert_abs(spec.absolute_text, out_time);
    }

    std::string cursor_name = spec.cursor_name;
    CursorManager cm;
    if (spec.use_active_cursor && !cm.get_active_cursor(g_session_id, cursor_name)) {
        error = "CURSOR_NOT_FOUND: active cursor is not set";
        return false;
    }
    Cursor cursor;
    if (!cm.get_cursor(g_session_id, cursor_name, cursor)) {
        error = "CURSOR_NOT_FOUND: Cursor '" + cursor_name + "' does not exist";
        return false;
    }
    uint64_t resolved = cursor.time;
    if (spec.has_offset) {
        if (spec.offset.cycle) {
            error = "CLOCK_OFFSET_UNSUPPORTED: cycle offsets are not implemented in this build";
            return false;
        }
        npiFsdbTime delta = 0;
        if (!g_fsdb_file ||
            !npi_fsdb_convert_time_in(g_fsdb_file,
                                      std::fabs(spec.offset.value),
                                      spec.offset.unit.c_str(),
                                      delta)) {
            error = "Failed to convert TimeSpec offset '" + spec.source + "'";
            return false;
        }
        if (spec.offset.value < 0) {
            if (resolved < delta) {
                error = "TIME_OUT_OF_RANGE: resolved time is before waveform start";
                return false;
            }
            resolved -= delta;
        } else {
            resolved += delta;
        }
    }
    out_time = static_cast<npiFsdbTime>(resolved);
    return true;
}

static bool read_list_from_storage(const std::string& session_id, const char* list_name, SignalList& out_list) {
    ListManager lm;
    return lm.get_list(session_id, list_name, out_list);
}

static std::string format_time(npiFsdbTime t) {
    auto format_number = [](double value) {
        char buf[64];
        double rounded = std::round(value);
        if (std::fabs(value - rounded) < 1e-9) {
            snprintf(buf, sizeof(buf), "%.0f", rounded);
        } else {
            snprintf(buf, sizeof(buf), "%.6g", value);
        }
        return std::string(buf);
    };

    if (g_fsdb_file) {
        double us = 0.0;
        if (npi_fsdb_convert_time_out(g_fsdb_file, t, "us", us) && us >= 1.0 &&
            std::fabs(us - std::round(us)) < 1e-9) {
            return format_number(us) + "us";
        }
        double ns = 0.0;
        if (npi_fsdb_convert_time_out(g_fsdb_file, t, "ns", ns) && ns >= 1.0 &&
            std::fabs(ns - std::round(ns)) < 1e-9) {
            return format_number(ns) + "ns";
        }
        double ps = 0.0;
        if (npi_fsdb_convert_time_out(g_fsdb_file, t, "ps", ps)) {
            return format_number(ps) + "ps";
        }
    }

    if (t % 1000000 == 0 && t >= 1000000) {
        return std::to_string(t / 1000000) + "us";
    }
    if (t % 1000 == 0 && t >= 1000) {
        return std::to_string(t / 1000) + "ns";
    }
    return std::to_string(t) + "ps";
}

static bool json_time_range(const Json& args,
                            npiFsdbTime& begin,
                            npiFsdbTime& end,
                            std::string& error) {
    begin = 0;
    end = 0xFFFFFFFFFFFFFFFFULL;
    Json tr = args.value("time_range", Json::object());
    std::string begin_s = tr.value("begin", args.value("begin", std::string("0ns")));
    std::string end_s = tr.value("end", args.value("end", std::string("max")));
    return parse_user_time(begin_s.c_str(), false, begin, error) &&
           parse_user_time(end_s.c_str(), true, end, error);
}

static npiFsdbValType json_value_format(const Json& args) {
    std::string fmt = args.value("format", std::string("binary"));
    if (fmt == "hex" || fmt == "h") return npiFsdbHexStrVal;
    if (fmt == "decimal" || fmt == "dec" || fmt == "d") return npiFsdbDecStrVal;
    return npiFsdbBinStrVal;
}

static std::string compact_expr_ws(const std::string& expr) {
    std::string out;
    for (char c : expr) {
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

static char json_value_prefix(npiFsdbValType fmt) {
    if (fmt == npiFsdbHexStrVal) return 'h';
    if (fmt == npiFsdbDecStrVal) return 'd';
    return 'b';
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
    if (!read_list_from_storage(g_session_id, list_name, list)) {
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
    std::vector<bool> found;
    bool all_found = read_sig_vec_value_at_with_status(g_fsdb_file, list.signals, time, fmt, values, found);

    std::string response;
    if (json) {
        Json j = Json::object();
        Json missing = Json::array();
        Json value_obj = Json::object();
        for (size_t i = 0; i < list.signals.size(); ++i) {
            value_obj[list.signals[i]] = values[i];
            if (!found[i]) missing.push_back(list.signals[i]);
        }
        if (all_found) {
            response = json_response(value_obj);
        } else {
            j["error"] = "List contains missing signals";
            j["values"] = value_obj;
            j["missing"] = missing;
            response = std::string(ERROR_PREFIX) + j.dump() + "\n" + END_MARKER;
        }
    } else {
        char fmt_lower = std::tolower(static_cast<unsigned char>(fmt));
        for (size_t i = 0; i < list.signals.size(); ++i) {
            if (found[i]) response += list.signals[i] + ":'" + fmt_lower + values[i] + "\n";
            else response += list.signals[i] + ":NOT_FOUND\n";
        }
        if (!all_found) response = std::string(ERROR_PREFIX) + "List contains missing signals\n" + response;
        response += END_MARKER;
    }
    send_all(client_fd, response.c_str(), response.length());
}

static void handle_signal_check(int client_fd, const char* signal_path) {
    if (npi_fsdb_sig_by_name(g_fsdb_file, signal_path, NULL)) {
        std::string resp = std::string("OK\n") + END_MARKER;
        send_all(client_fd, resp.c_str(), resp.length());
    } else {
        std::string err = std::string(ERROR_PREFIX) + "Signal not found: " + signal_path + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
    }
}

static void handle_list_validate(int client_fd, const char* list_name, bool json) {
    SignalList list;
    if (!read_list_from_storage(g_session_id, list_name, list)) {
        std::string err = std::string(ERROR_PREFIX) + "List not found: " + list_name + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    bool all_found = true;
    std::string text;
    Json out = Json::array();
    for (const auto& signal : list.signals) {
        bool found = npi_fsdb_sig_by_name(g_fsdb_file, signal.c_str(), NULL) != nullptr;
        if (!found) all_found = false;
        if (json) {
            Json item;
            item["signal"] = signal;
            item["status"] = found ? "ok" : "not_found";
            out.push_back(item);
        } else {
            text += signal + ": " + (found ? "OK" : "NOT_FOUND") + "\n";
        }
    }

    std::string resp;
    if (json) {
        resp = all_found ? json_response(out) : std::string(ERROR_PREFIX) + out.dump() + "\n" + END_MARKER;
    } else {
        if (!all_found) resp = std::string(ERROR_PREFIX) + "List contains missing signals\n";
        resp += text;
        resp += END_MARKER;
    }
    send_all(client_fd, resp.c_str(), resp.length());
}

static void handle_scope(int client_fd, const char* scope_path, bool recursive, bool json) {
    FILE* fp = tmpfile();
    if (!fp) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to create temporary scope output\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    int ok = npi_fsdb_hier_tree_dump_sig(g_fsdb_file, fp, scope_path, recursive ? 1 : 0);
    fflush(fp);
    rewind(fp);

    std::vector<std::string> lines;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len > 0) lines.push_back(line);
    }
    fclose(fp);

    if (!ok) {
        std::string err = std::string(ERROR_PREFIX) + "Failed to list scope: " + scope_path + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }

    std::string resp;
    if (json) {
        Json arr = Json::array();
        for (const auto& l : lines) arr.push_back(l);
        resp = json_response(arr);
    } else {
        for (const auto& l : lines) resp += l + "\n";
        if (lines.empty()) resp += "(no signals found)\n";
        resp += END_MARKER;
    }
    send_all(client_fd, resp.c_str(), resp.length());
}

// Helper: read an APB config from the registry file by session_id and name
static bool read_apb_from_registry(const std::string& session_id, const char* name, xwave::ApbConfig& out_config) {
    xwave::ApbManager am;
    return am.get_apb(session_id, name, out_config);
}

static bool read_axi_from_registry(const std::string& session_id, const char* name, xwave::AxiConfig& out_config) {
    xwave::AxiManager am;
    return am.get_axi(session_id, name, out_config);
}

static void handle_list_diff(int client_fd, const char* list_name, npiFsdbTime begin_time, npiFsdbTime end_time) {
    SignalList list;
    if (!read_list_from_storage(g_session_id, list_name, list)) {
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

static Json apb_txn_to_json(const xwave::ApbTransaction* txn, bool include_type) {
    Json j;
    if (!txn) return j;
    j["time"] = format_time(txn->time);
    if (include_type) j["type"] = txn->is_write ? "WR" : "RD";
    j["addr"] = "'h" + txn->addr;
    j["data"] = "'h" + txn->data;
    return j;
}

static std::string format_apb_txn_json(const xwave::ApbTransaction* txn) {
    if (!txn) return std::string(END_MARKER);
    Json j = apb_txn_to_json(txn, false);
    return json_response(j);
}

static std::string format_apb_txn_json_with_type(const xwave::ApbTransaction* txn) {
    if (!txn) return std::string(END_MARKER);
    Json j = apb_txn_to_json(txn, true);
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

static Json axi_txn_to_json(const xwave::AxiTransaction* txn) {
    Json j;
    if (!txn) return j;
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
    return j;
}

static std::string format_axi_txn_json(const xwave::AxiTransaction* txn) {
    if (!txn) return std::string(END_MARKER);
    Json j = axi_txn_to_json(txn);
    return json_response(j);
}

static bool read_signal_changes(const std::string& signal,
                                npiFsdbTime begin,
                                npiFsdbTime end,
                                npiFsdbValType fmt,
                                fsdbTimeValPairVec_t& changes,
                                std::string& error,
                                int max_changes = -1,
                                bool* truncated = nullptr) {
    changes.clear();
    if (truncated) *truncated = false;
    npiFsdbSigHandle sig = npi_fsdb_sig_by_name(g_fsdb_file, signal.c_str(), NULL);
    if (!sig) {
        error = "Signal not found: " + signal;
        return false;
    }
    TimeBasedVcIterGuard guard;
    npiFsdbTimeBasedVcIter& iter = guard.iter();
    iter.add(sig);
    guard.start(begin, end);
    npiFsdbTime t = 0;
    npiFsdbSigHandle changed_sig = nullptr;
    while (iter.iter_next(t, changed_sig) > 0) {
        if (max_changes >= 0 && static_cast<int>(changes.size()) >= max_changes) {
            if (truncated) *truncated = true;
            break;
        }
        npiFsdbValue val;
        val.format = fmt;
        std::string value;
        if (!iter.get_value(val) || !val.value.str) continue;
        value = val.value.str;
        changes.push_back(std::make_pair(t, value));
    }
    return true;
}

static Json changes_to_json(const fsdbTimeValPairVec_t& changes,
                            char prefix,
                            int limit,
                            bool& truncated) {
    Json arr = Json::array();
    truncated = false;
    for (size_t i = 0; i < changes.size(); ++i) {
        if (limit >= 0 && static_cast<int>(arr.size()) >= limit) {
            truncated = true;
            break;
        }
        Json item;
        item["time"] = format_time(changes[i].first);
        item["time_ps"] = changes[i].first;
        item["value"] = wave_value_json(changes[i].second, prefix);
        arr.push_back(item);
    }
    return arr;
}

static bool build_signal_alias_handles(const Json& signals,
                                       std::vector<std::string>& aliases,
                                       std::vector<std::string>& paths,
                                       fsdbSigVec_t& handles,
                                       std::string& error) {
    if (!signals.is_object()) {
        error = "signals must be an object";
        return false;
    }
    std::map<std::string, std::string> seen;
    for (auto it = signals.begin(); it != signals.end(); ++it) {
        if (!it.value().is_string()) {
            error = "signal path must be string for alias: " + it.key();
            return false;
        }
        std::string alias = it.key();
        std::string path = it.value().get<std::string>();
        auto prev = seen.find(alias);
        if (prev != seen.end() && prev->second != path) {
            error = "alias maps to different signals: " + alias;
            return false;
        }
        if (prev != seen.end()) continue;
        npiFsdbSigHandle sig = npi_fsdb_sig_by_name(g_fsdb_file, path.c_str(), NULL);
        if (!sig) {
            error = "Signal not found: " + path;
            return false;
        }
        seen[alias] = path;
        aliases.push_back(alias);
        paths.push_back(path);
        handles.push_back(sig);
    }
    return true;
}

static Json ai_signal_changes(const Json& args, std::string& error) {
    std::string signal = args.value("signal", std::string());
    if (signal.empty()) {
        error = "signal.changes requires args.signal";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    int limit = args.value("limit", args.value("max_events", 1000));
    npiFsdbValType fmt = json_value_format(args);
    fsdbTimeValPairVec_t changes;
    bool truncated = false;
    int read_limit = limit >= 0 ? limit + 1 : -1;
    if (!read_signal_changes(signal, begin, end, fmt, changes, error, read_limit, &truncated)) return Json();
    Json data;
    data["signal"] = signal;
    data["begin"] = format_time(begin);
    data["end"] = format_time(end);
    data["changes"] = changes_to_json(changes, json_value_prefix(fmt), limit, truncated);
    data["transition_count"] = changes.size();
    data["truncated"] = truncated;
    if (!changes.empty()) {
        data["initial_value"] = wave_value_json(changes.front().second, json_value_prefix(fmt));
        data["final_value"] = wave_value_json(changes.back().second, json_value_prefix(fmt));
        data["first_change"] = format_time(changes.front().first);
        data["last_change"] = format_time(changes.back().first);
    }
    return data;
}

static Json ai_signal_stability(const Json& args, std::string& error) {
    std::string signal = args.value("signal", std::string());
    if (signal.empty()) {
        error = "signal.stability requires args.signal";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    npiFsdbValType fmt = json_value_format(args);
    bool truncated = false;
    npiFsdbSigHandle sig = npi_fsdb_sig_by_name(g_fsdb_file, signal.c_str(), NULL);
    if (!sig) {
        error = "Signal not found: " + signal;
        return Json();
    }
    Json arr = Json::array();
    bool stable = true;
    std::string first;
    TimeBasedVcIterGuard guard;
    npiFsdbTimeBasedVcIter& iter = guard.iter();
    iter.add(sig);
    guard.start(begin, end);
    npiFsdbTime change_time = 0;
    npiFsdbSigHandle changed_sig = nullptr;
    while (iter.iter_next(change_time, changed_sig) > 0) {
        npiFsdbValue val;
        val.format = fmt;
        if (!iter.get_value(val) || !val.value.str) continue;
        std::string text = value_with_prefix(val.value.str, json_value_prefix(fmt));
        Json item;
        item["time"] = format_time(change_time);
        item["time_ps"] = change_time;
        item["value"] = wave_value_json(val.value.str, json_value_prefix(fmt));
        arr.push_back(item);
        if (first.empty()) {
            first = text;
        } else if (text != first) {
            stable = false;
            break;
        }
    }

    Json data;
    data["signal"] = signal;
    data["begin"] = format_time(begin);
    data["end"] = format_time(end);
    data["changes"] = arr;
    data["transition_count"] = arr.size();
    data["truncated"] = truncated;
    if (!arr.empty()) {
        data["initial_value"] = arr[0]["value"];
        data["final_value"] = arr[arr.size() - 1]["value"];
        data["first_change"] = arr[0]["time"];
        data["last_change"] = arr[arr.size() - 1]["time"];
    }
    if (!stable) {
        for (const auto& item : arr) {
            if (item["value"]["text"].get<std::string>() != first) {
                data["first_change_time"] = item["time"];
                break;
            }
        }
    }
    data["stable"] = stable;
    if (stable && !arr.empty()) data["value"] = arr[0]["value"];
    return data;
}

static bool sample_on_clock(const std::string& clock_path,
                            bool posedge,
                            const std::vector<std::string>& aliases,
                            const fsdbSigVec_t& signal_handles,
                            npiFsdbTime begin,
                            npiFsdbTime end,
                            int max_samples,
                            std::function<bool(npiFsdbTime, const std::map<std::string, std::string>&)> cb,
                            std::string& error,
                            int& sample_count,
                            bool& truncated) {
    sample_count = 0;
    truncated = false;
    npiFsdbSigHandle clk = npi_fsdb_sig_by_name(g_fsdb_file, clock_path.c_str(), NULL);
    if (!clk) {
        error = "Clock signal not found: " + clock_path;
        return false;
    }
    fsdbSigVec_t all_handles;
    all_handles.push_back(clk);
    for (auto h : signal_handles) all_handles.push_back(h);
    fsdbValVec_t init_values;
    npiFsdbTime init_time = begin > 0 ? begin - 1 : begin;
    if (!npi_fsdb_sig_hdl_vec_value_at(all_handles, init_time, init_values, npiFsdbBinStrVal) ||
        init_values.size() != all_handles.size()) {
        error = "Failed to read initial sampled values";
        return false;
    }
    std::string prev_clk = with_value_prefix(init_values[0], 'b');
    std::vector<std::string> values(signal_handles.size(), "'b0");
    for (size_t i = 0; i < signal_handles.size(); ++i) values[i] = with_value_prefix(init_values[i + 1], 'b');

    TimeBasedVcIterGuard guard;
    npiFsdbTimeBasedVcIter& iter = guard.iter();
    iter.add(clk);
    for (auto h : signal_handles) iter.add(h);
    guard.start(begin, end);

    bool have_group = false;
    bool edge = false;
    npiFsdbTime group_time = 0;
    auto finish_group = [&]() -> bool {
        if (!have_group || !edge) return true;
        std::map<std::string, std::string> value_map;
        for (size_t i = 0; i < aliases.size(); ++i) value_map[aliases[i]] = values[i];
        ++sample_count;
        if (max_samples >= 0 && sample_count > max_samples) {
            truncated = true;
            return false;
        }
        return cb(group_time, value_map);
    };

    npiFsdbTime curr_time = 0;
    npiFsdbSigHandle changed_sig = nullptr;
    bool keep = true;
    while (keep && iter.iter_next(curr_time, changed_sig) > 0) {
        if (!have_group) {
            have_group = true;
            group_time = curr_time;
        } else if (curr_time != group_time) {
            keep = finish_group();
            if (!keep) break;
            group_time = curr_time;
            edge = false;
        }
        npiFsdbValue val;
        val.format = npiFsdbBinStrVal;
        if (!iter.get_value(val) || !val.value.str) continue;
        std::string v = with_value_prefix(val.value.str, 'b');
        if (changed_sig == clk) {
            ExprTri old_clk = xwave::expr_truth_value(prev_clk);
            ExprTri new_clk = xwave::expr_truth_value(v);
            edge = posedge ? (old_clk == ExprTri::False && new_clk == ExprTri::True)
                           : (old_clk == ExprTri::True && new_clk == ExprTri::False);
            prev_clk = v;
        } else {
            for (size_t i = 0; i < signal_handles.size(); ++i) {
                if (signal_handles[i] == changed_sig) {
                    values[i] = v;
                    break;
                }
            }
        }
    }
    if (keep) finish_group();
    return error.empty();
}

static Json ai_expr_eval_at(const Json& args, std::string& error) {
    std::string time_s = args.value("at", args.value("time", std::string()));
    std::string expr = compact_expr_ws(args.value("expr", std::string()));
    if (time_s.empty() || expr.empty() || !args.contains("signals")) {
        error = "expr.eval_at requires args.time/args.at, args.expr and args.signals";
        return Json();
    }
    npiFsdbTime t = 0;
    if (!parse_user_time(time_s.c_str(), false, t, error)) return Json();
    std::vector<std::string> aliases, paths;
    fsdbSigVec_t handles;
    if (!build_signal_alias_handles(args["signals"], aliases, paths, handles, error)) return Json();
    fsdbValVec_t values;
    if (!npi_fsdb_sig_hdl_vec_value_at(handles, t, values, npiFsdbBinStrVal) || values.size() != handles.size()) {
        error = "Failed to read expression operands";
        return Json();
    }
    std::map<std::string, std::string> value_map;
    Json operands = Json::array();
    for (size_t i = 0; i < aliases.size(); ++i) {
        std::string v = with_value_prefix(values[i], 'b');
        value_map[aliases[i]] = v;
        operands.push_back({{"alias", aliases[i]}, {"signal", paths[i]}, {"value", wave_value_json(v, 'b')}});
    }
    ExprTri result = ExprTri::Unknown;
    if (!xwave::eval_event_expression(expr, value_map, result, error)) return Json();
    Json data;
    data["expr"] = expr;
    data["time"] = format_time(t);
    data["time_ps"] = t;
    data["status"] = xwave::expr_tri_text(result);
    data["known"] = result != ExprTri::Unknown;
    data["expr_value"] = result == ExprTri::True ? Json(true) : result == ExprTri::False ? Json(false) : Json(nullptr);
    data["operands"] = operands;
    return data;
}

static Json ai_window_verify(const Json& args, std::string& error) {
    std::string clock = args.value("clock", std::string());
    if (clock.empty() || !args.contains("conditions") || !args["conditions"].is_array()) {
        error = "window.verify requires args.clock and args.conditions[]";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    bool posedge = args.value("sampling", std::string("posedge")) != "negedge";
    int max_samples = args.value("max_samples", 1000000);

    Json signal_union = Json::object();
    for (const auto& cond : args["conditions"]) {
        if (!cond.contains("expr") || !cond.contains("signals")) {
            error = "each condition requires expr and signals";
            return Json();
        }
        for (auto it = cond["signals"].begin(); it != cond["signals"].end(); ++it) {
            if (signal_union.contains(it.key()) && signal_union[it.key()] != it.value()) {
                error = "duplicate alias maps to different signals: " + it.key();
                return Json();
            }
            signal_union[it.key()] = it.value();
        }
    }
    std::vector<std::string> aliases, paths;
    fsdbSigVec_t handles;
    if (!build_signal_alias_handles(signal_union, aliases, paths, handles, error)) return Json();

    struct CondState { std::string expr; std::string mode; int pass = 0; int fail = 0; int unknown = 0; };
    std::vector<CondState> states;
    for (const auto& cond : args["conditions"]) {
        CondState st;
        st.expr = compact_expr_ws(cond.value("expr", std::string()));
        st.mode = cond.value("mode", std::string("always"));
        states.push_back(st);
    }

    int samples = 0;
    bool truncated = false;
    bool ok = sample_on_clock(clock, posedge, aliases, handles, begin, end, max_samples,
        [&](npiFsdbTime, const std::map<std::string, std::string>& values) -> bool {
            bool has_eventually = false;
            bool all_eventually_seen = true;
            for (auto& st : states) {
                ExprTri r = ExprTri::Unknown;
                std::string eval_error;
                if (!xwave::eval_event_expression(st.expr, values, r, eval_error)) {
                    error = eval_error;
                    return false;
                }
                bool pass = false;
                if (st.mode == "eventually") pass = (r == ExprTri::True);
                else if (st.mode == "never") pass = (r == ExprTri::False);
                else pass = (r == ExprTri::True);
                if (r == ExprTri::Unknown) st.unknown++;
                else if (pass) st.pass++;
                else st.fail++;
                if (st.mode == "eventually") {
                    has_eventually = true;
                    if (st.pass == 0) all_eventually_seen = false;
                } else if (r == ExprTri::Unknown || !pass) {
                    return false;
                }
            }
            return !(has_eventually && all_eventually_seen);
        }, error, samples, truncated);
    if (!ok) return Json();

    Json conds = Json::array();
    bool all_passed = true;
    int failed_samples = 0, unknown_samples = 0;
    for (const auto& st : states) {
        bool passed = false;
        if (st.mode == "eventually") passed = st.pass > 0;
        else passed = st.fail == 0 && st.unknown == 0;
        if (!passed) all_passed = false;
        failed_samples += st.fail;
        unknown_samples += st.unknown;
        conds.push_back({{"expr", st.expr}, {"mode", st.mode}, {"passed", passed},
                         {"pass_samples", st.pass}, {"failed_samples", st.fail}, {"unknown_samples", st.unknown}});
    }
    Json data;
    data["all_passed"] = all_passed;
    data["sample_count"] = samples;
    data["failed_samples"] = failed_samples;
    data["unknown_samples"] = unknown_samples;
    data["truncated"] = truncated;
    data["conditions"] = conds;
    return data;
}

static Json ai_signal_trend(const Json& args, std::string& error) {
    std::string signal = args.value("signal", std::string());
    std::string clock = args.value("clock", std::string());
    if (signal.empty() || clock.empty()) {
        error = "signal.trend requires args.signal and args.clock";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    Json signals = {{"sig", signal}};
    std::vector<std::string> aliases, paths;
    fsdbSigVec_t handles;
    if (!build_signal_alias_handles(signals, aliases, paths, handles, error)) return Json();
    bool posedge = args.value("sampling", std::string("posedge")) != "negedge";
    int max_samples = args.value("max_samples", 1000000);
    bool have = false, stable = true, inc = true, dec = true;
    unsigned long long first = 0, last = 0, minv = 0, maxv = 0, prev = 0;
    int unknown = 0, samples = 0;
    bool truncated = false;
    if (!sample_on_clock(clock, posedge, aliases, handles, begin, end, max_samples,
        [&](npiFsdbTime, const std::map<std::string, std::string>& values) -> bool {
            auto it = values.find("sig");
            if (it == values.end() || contains_xz_value(it->second)) {
                unknown++;
                return true;
            }
            std::string bits = xwave::expr_bits_only(it->second);
            unsigned long long v = 0;
            for (char c : bits) v = (v << 1) | (c == '1' ? 1ULL : 0ULL);
            if (!have) {
                first = last = minv = maxv = prev = v;
                have = true;
            } else {
                if (v != prev) stable = false;
                if (v < prev) inc = false;
                if (v > prev) dec = false;
                if (v < minv) minv = v;
                if (v > maxv) maxv = v;
                prev = v;
                last = v;
            }
            return true;
        }, error, samples, truncated)) return Json();
    Json data;
    data["signal"] = signal;
    data["sample_count"] = samples;
    data["unknown_count"] = unknown;
    data["stable"] = stable;
    data["truncated"] = truncated;
    if (have) {
        data["initial_value"] = first;
        data["final_value"] = last;
        data["min_value"] = minv;
        data["max_value"] = maxv;
        data["monotonic"] = stable ? "stable" : inc ? "increasing" : dec ? "decreasing" : "none";
    }
    return data;
}

static Json ai_handshake_inspect(const Json& args, std::string& error) {
    std::string clock = args.value("clock", std::string());
    std::string valid = args.value("valid", std::string());
    std::string ready = args.value("ready", std::string());
    if (clock.empty() || valid.empty() || ready.empty()) {
        error = "handshake.inspect requires args.clock, args.valid and args.ready";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    Json signals = {{"valid", valid}, {"ready", ready}};
    if (args.contains("data") && args["data"].is_array()) {
        int idx = 0;
        for (const auto& d : args["data"]) if (d.is_string()) signals["data" + std::to_string(idx++)] = d.get<std::string>();
    }
    std::vector<std::string> aliases, paths;
    fsdbSigVec_t handles;
    if (!build_signal_alias_handles(signals, aliases, paths, handles, error)) return Json();
    bool posedge = args.value("sampling", std::string("posedge")) != "negedge";
    Json rules = args.value("rules", Json::object());
    int max_wait = rules.value("max_wait_cycles", 100);
    bool check_data = rules.value("check_data_stable_when_stalled", false);
    int samples = 0, transfers = 0, stall_cycles = 0, max_stall = 0, ready_only = 0, data_violations = 0;
    bool in_stall = false, truncated = false;
    npiFsdbTime stall_begin = 0;
    std::map<std::string, std::string> stall_data;
    Json findings = Json::array();
    if (!sample_on_clock(clock, posedge, aliases, handles, begin, end, args.value("max_samples", 1000000),
        [&](npiFsdbTime t, const std::map<std::string, std::string>& values) -> bool {
            ExprTri v = xwave::expr_truth_value(values.at("valid"));
            ExprTri r = xwave::expr_truth_value(values.at("ready"));
            bool transfer = v == ExprTri::True && r == ExprTri::True;
            bool stall = v == ExprTri::True && r == ExprTri::False;
            if (transfer) transfers++;
            if (r == ExprTri::True && v == ExprTri::False) ready_only++;
            if (stall) {
                stall_cycles++;
                if (!in_stall) {
                    in_stall = true;
                    stall_begin = t;
                    stall_data = values;
                } else if (check_data) {
                    for (const auto& kv : values) {
                        if (kv.first.find("data") == 0 && stall_data[kv.first] != kv.second) data_violations++;
                    }
                }
            } else if (in_stall) {
                int cycles = stall_cycles;
                if (cycles > max_stall) max_stall = cycles;
                if (cycles > max_wait) {
                    findings.push_back({{"type", "long_stall"}, {"severity", "warning"},
                                        {"begin", format_time(stall_begin)}, {"end", format_time(t)}, {"cycles", cycles}});
                }
                in_stall = false;
                stall_cycles = 0;
            }
            return true;
        }, error, samples, truncated)) return Json();
    if (in_stall && stall_cycles > max_stall) max_stall = stall_cycles;
    Json data;
    data["sample_count"] = samples;
    data["transfer_count"] = transfers;
    data["max_stall_cycles"] = max_stall;
    data["ready_without_valid_cycles"] = ready_only;
    data["data_stability_violations"] = data_violations;
    data["truncated"] = truncated;
    data["findings"] = findings;
    return data;
}

static Json ai_inspect_signal(const Json& args, std::string& error) {
    Json data = ai_signal_changes(args, error);
    if (!error.empty()) return Json();
    npiFsdbTime glitch_threshold = 0;
    std::string threshold = args.value("glitch_threshold", std::string("1ns"));
    parse_user_time(threshold.c_str(), false, glitch_threshold, error);
    if (!error.empty()) return Json();
    Json arr = data["changes"];
    Json period;
    double total_period = 0.0;
    int period_count = 0;
    int glitch_count = 0;
    for (size_t i = 1; i < arr.size(); ++i) {
        npiFsdbTime t0 = arr[i - 1]["time_ps"].get<npiFsdbTime>();
        npiFsdbTime t1 = arr[i]["time_ps"].get<npiFsdbTime>();
        npiFsdbTime width = t1 >= t0 ? t1 - t0 : 0;
        if (width > 0) {
            total_period += static_cast<double>(width);
            period_count++;
            if (width < glitch_threshold) glitch_count++;
        }
    }
    data["edge_count"] = arr.size();
    data["glitch"] = {{"count", glitch_count}, {"threshold", format_time(glitch_threshold)}};
    if (period_count > 0) {
        period["avg_ps"] = total_period / period_count;
        period["samples"] = period_count;
        data["period"] = period;
    }
    return data;
}

static Json ai_detect_anomaly(const Json& args, std::string& error) {
    if (!args.contains("signals") || !args["signals"].is_array()) {
        error = "detect_anomaly requires args.signals[]";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    Json checks = args.value("checks", Json::array());
    npiFsdbTime glitch_width = 0, stuck_duration = 0;
    bool check_glitch = false, check_stuck = false, check_unknown = false;
    for (const auto& c : checks) {
        std::string type = c.value("type", std::string());
        if (type == "glitch") {
            check_glitch = true;
            std::string v = c.value("min_pulse_width", std::string("1ns"));
            if (!parse_user_time(v.c_str(), false, glitch_width, error)) return Json();
        } else if (type == "stuck") {
            check_stuck = true;
            std::string v = c.value("min_duration", std::string("1us"));
            if (!parse_user_time(v.c_str(), false, stuck_duration, error)) return Json();
        } else if (type == "unknown_xz") {
            check_unknown = true;
        }
    }
    if (checks.empty()) {
        check_unknown = true;
        check_stuck = true;
        parse_user_time("1us", false, stuck_duration, error);
        if (!error.empty()) return Json();
    }
    int max_findings = args.value("max_findings", 50);
    Json findings = Json::array();
    for (const auto& s : args["signals"]) {
        if (max_findings >= 0 && static_cast<int>(findings.size()) >= max_findings) break;
        if (!s.is_string()) continue;
        std::string signal = s.get<std::string>();
        fsdbTimeValPairVec_t changes;
        if (!read_signal_changes(signal, begin, end, npiFsdbBinStrVal, changes, error)) return Json();
        for (size_t i = 0; i < changes.size(); ++i) {
            if (max_findings >= 0 && static_cast<int>(findings.size()) >= max_findings) break;
            if (check_unknown && contains_xz_value(changes[i].second)) {
                findings.push_back({{"type", "unknown_xz"}, {"signal", signal}, {"severity", "warning"},
                                    {"time", format_time(changes[i].first)}, {"value", wave_value_json(changes[i].second, 'b')}});
            }
            if (check_glitch && i + 1 < changes.size()) {
                npiFsdbTime width = changes[i + 1].first >= changes[i].first ? changes[i + 1].first - changes[i].first : 0;
                if (width > 0 && width < glitch_width) {
                    findings.push_back({{"type", "glitch"}, {"signal", signal}, {"severity", "info"},
                                        {"time", format_time(changes[i].first)}, {"pulse_width", format_time(width)}});
                }
            }
            if (check_stuck && i + 1 < changes.size()) {
                npiFsdbTime width = changes[i + 1].first >= changes[i].first ? changes[i + 1].first - changes[i].first : 0;
                if (width >= stuck_duration) {
                    findings.push_back({{"type", "stuck"}, {"signal", signal}, {"severity", "warning"},
                                        {"begin", format_time(changes[i].first)}, {"end", format_time(changes[i + 1].first)},
                                        {"duration", format_time(width)}, {"value", wave_value_json(changes[i].second, 'b')}});
                }
            }
        }
    }
    Json data;
    data["finding_count"] = findings.size();
    data["findings"] = findings;
    data["truncated"] = max_findings >= 0 && static_cast<int>(findings.size()) >= max_findings;
    return data;
}

static int direction_filter(const Json& args) {
    std::string direction = args.value("direction", std::string("all"));
    if (direction == "wr" || direction == "write") return 1;
    if (direction == "rd" || direction == "read") return 2;
    return 0;
}

static bool ensure_apb_analyzed_for_ai(const std::string& name, std::string& error) {
    xwave::ApbConfig config;
    if (!read_apb_from_registry(g_session_id, name.c_str(), config)) {
        error = "APB config not found: " + name;
        return false;
    }
    if (!g_apb_analyzer.analyze(name, g_fsdb_file, config)) {
        error = "Failed to analyze APB: " + name;
        return false;
    }
    return true;
}

static bool ensure_axi_analyzed_for_ai(const std::string& name, std::string& error) {
    xwave::AxiConfig config;
    if (!read_axi_from_registry(g_session_id, name.c_str(), config)) {
        error = "AXI config not found: " + name;
        return false;
    }
    if (!g_axi_analyzer.analyze(name, g_fsdb_file, config)) {
        error = "Failed to analyze AXI: " + name;
        return false;
    }
    return true;
}

static Json ai_apb_transfer_window(const Json& args, std::string& error) {
    std::string name = args.value("name", std::string());
    if (name.empty()) {
        error = "apb.transfer_window requires args.name";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    if (!ensure_apb_analyzed_for_ai(name, error)) return Json();
    std::vector<xwave::ApbContextTransaction> txns;
    int filter = direction_filter(args);
    int limit = args.value("max_rows", args.value("limit", 1000));
    int fetch_limit = (filter == 0 && limit >= 0) ? limit + 1 : -1;
    if (!g_apb_analyzer.get_transactions_in_range(name, begin, end, txns, fetch_limit)) {
        error = "APB config not analyzed: " + name;
        return Json();
    }
    Json arr = Json::array();
    bool truncated = false;
    for (const auto& item : txns) {
        if (!item.txn) continue;
        if (filter == 1 && !item.txn->is_write) continue;
        if (filter == 2 && item.txn->is_write) continue;
        if (limit >= 0 && static_cast<int>(arr.size()) >= limit) {
            truncated = true;
            break;
        }
        Json txn = apb_txn_to_json(item.txn, true);
        txn["time_ps"] = item.txn->time;
        arr.push_back(txn);
    }
    return Json{{"name", name}, {"begin", format_time(begin)}, {"end", format_time(end)},
                {"transaction_count", arr.size()}, {"truncated", truncated}, {"transactions", arr}};
}

static Json ai_axi_transactions_window(const Json& args, std::string& error) {
    std::string name = args.value("name", std::string());
    if (name.empty()) {
        error = "AXI action requires args.name";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    if (!ensure_axi_analyzed_for_ai(name, error)) return Json();
    std::vector<xwave::AxiContextTransaction> txns;
    int filter = direction_filter(args);
    int limit = args.value("max_rows", args.value("limit", 1000));
    int fetch_limit = (filter == 0 && limit >= 0) ? limit + 1 : -1;
    if (!g_axi_analyzer.get_transactions_in_range(name, begin, end, txns, fetch_limit)) {
        error = "AXI config not analyzed: " + name;
        return Json();
    }
    Json arr = Json::array();
    bool truncated = false;
    for (const auto& item : txns) {
        if (!item.txn) continue;
        if (filter == 1 && !item.txn->is_write) continue;
        if (filter == 2 && item.txn->is_write) continue;
        if (limit >= 0 && static_cast<int>(arr.size()) >= limit) {
            truncated = true;
            break;
        }
        Json txn = axi_txn_to_json(item.txn);
        txn["match_time"] = format_time(item.match_time);
        txn["match_time_ps"] = item.match_time;
        txn["latency_ps"] = item.txn->resp_time >= item.txn->addr_time ? item.txn->resp_time - item.txn->addr_time : 0;
        arr.push_back(txn);
    }
    return Json{{"name", name}, {"begin", format_time(begin)}, {"end", format_time(end)},
                {"transaction_count", arr.size()}, {"truncated", truncated}, {"transactions", arr}};
}

static Json ai_axi_latency_outlier(const Json& args, std::string& error) {
    Json data = ai_axi_transactions_window(args, error);
    if (!error.empty()) return Json();
    Json txns = data["transactions"];
    std::vector<Json> vec;
    for (const auto& t : txns) vec.push_back(t);
    std::sort(vec.begin(), vec.end(), [](const Json& a, const Json& b) {
        return a.value("latency_ps", 0ULL) > b.value("latency_ps", 0ULL);
    });
    int top_n = args.value("top_n", 10);
    Json out = Json::array();
    for (size_t i = 0; i < vec.size() && static_cast<int>(i) < top_n; ++i) out.push_back(vec[i]);
    data["outliers"] = out;
    data.erase("transactions");
    data["outlier_count"] = out.size();
    return data;
}

static Json ai_axi_outstanding_timeline(const Json& args, std::string& error) {
    std::string name = args.value("name", std::string());
    if (name.empty()) {
        error = "axi.outstanding_timeline requires args.name";
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    if (!ensure_axi_analyzed_for_ai(name, error)) return Json();
    std::vector<xwave::AxiOutstandingSample> samples;
    int limit = args.value("max_rows", args.value("limit", 1000));
    int fetch_limit = limit >= 0 ? limit + 1 : -1;
    if (!g_axi_analyzer.get_outstanding_samples_in_range(name, begin, end, samples, fetch_limit)) {
        error = "AXI config not analyzed: " + name;
        return Json();
    }
    int filter = direction_filter(args);
    Json arr = Json::array();
    bool truncated = false;
    for (const auto& s : samples) {
        if (limit >= 0 && static_cast<int>(arr.size()) >= limit) {
            truncated = true;
            break;
        }
        Json item;
        item["time"] = format_time(s.time);
        item["time_ps"] = s.time;
        if (filter == 0 || filter == 2) item["read"] = s.read;
        if (filter == 0 || filter == 1) item["write"] = s.write;
        arr.push_back(item);
    }
    return Json{{"name", name}, {"sample_count", arr.size()}, {"truncated", truncated}, {"samples", arr}};
}

static Json ai_axi_channel_stall(const Json& args, std::string& error) {
    std::string name = args.value("name", std::string());
    std::string channel = args.value("channel", std::string("ar"));
    xwave::AxiConfig cfg;
    if (name.empty()) {
        error = "axi.channel_stall requires args.name";
        return Json();
    }
    if (!read_axi_from_registry(g_session_id, name.c_str(), cfg)) {
        error = "AXI config not found: " + name;
        return Json();
    }
    npiFsdbTime begin = 0, end = 0;
    if (!json_time_range(args, begin, end, error)) return Json();
    std::string valid, ready;
    if (channel == "aw") { valid = cfg.awvalid; ready = cfg.awready; }
    else if (channel == "w") { valid = cfg.wvalid; ready = cfg.wready; }
    else if (channel == "b") { valid = cfg.bvalid; ready = cfg.bready; }
    else if (channel == "r") { valid = cfg.rvalid; ready = cfg.rready; }
    else { valid = cfg.arvalid; ready = cfg.arready; channel = "ar"; }

    npiFsdbSigHandle clk_h = npi_fsdb_sig_by_name(g_fsdb_file, cfg.clk.c_str(), NULL);
    npiFsdbSigHandle valid_h = npi_fsdb_sig_by_name(g_fsdb_file, valid.c_str(), NULL);
    npiFsdbSigHandle ready_h = npi_fsdb_sig_by_name(g_fsdb_file, ready.c_str(), NULL);
    if (!clk_h || !valid_h || !ready_h) {
        error = "AXI channel signal not found for channel: " + channel;
        return Json();
    }

    Json rules = args.value("rules", Json::object());
    int max_wait = rules.value("max_wait_cycles", 100);
    int max_samples = args.value("max_samples", 1000000);
    int sample_count = 0, transfers = 0, ready_only = 0, max_stall = 0;
    bool truncated = false;
    Json findings = Json::array();

    fsdbSigVec_t state_handles;
    state_handles.push_back(valid_h);
    state_handles.push_back(ready_h);
    fsdbValVec_t init_values;
    if (!npi_fsdb_sig_hdl_vec_value_at(state_handles, begin, init_values, npiFsdbBinStrVal) ||
        init_values.size() != state_handles.size()) {
        error = "Failed to read AXI channel initial values";
        return Json();
    }
    std::string valid_value = with_value_prefix(init_values[0], 'b');
    std::string ready_value = with_value_prefix(init_values[1], 'b');

    auto visit_interval = [&](npiFsdbTime start, npiFsdbTime stop) -> bool {
        if (stop < start) return true;
        ExprTri v = xwave::expr_truth_value(valid_value);
        ExprTri r = xwave::expr_truth_value(ready_value);
        bool interesting = (v == ExprTri::True || r == ExprTri::True);
        if (!interesting) return true;
        ClockEdgeCursor edge_cursor(clk_h, cfg.posedge);
        if (!edge_cursor.valid()) {
            error = "Failed to create AXI channel clock cursor";
            return false;
        }
        npiFsdbTime edge_time = 0;
        if (!edge_cursor.first_at_or_after(start, edge_time)) return true;
        int stall_cycles = 0;
        npiFsdbTime stall_begin = 0;
        while (edge_time <= stop) {
            if (max_samples >= 0 && sample_count >= max_samples) {
                truncated = true;
                return false;
            }
            ++sample_count;
            if (v == ExprTri::True && r == ExprTri::True) {
                ++transfers;
                if (stall_cycles > 0) {
                    if (stall_cycles > max_stall) max_stall = stall_cycles;
                    if (stall_cycles > max_wait) {
                        findings.push_back({{"type", "long_stall"}, {"severity", "warning"},
                                            {"begin", format_time(stall_begin)}, {"end", format_time(edge_time)},
                                            {"cycles", stall_cycles}});
                    }
                    stall_cycles = 0;
                }
            } else if (v == ExprTri::True && r == ExprTri::False) {
                if (stall_cycles == 0) stall_begin = edge_time;
                ++stall_cycles;
            } else if (r == ExprTri::True && v == ExprTri::False) {
                ++ready_only;
            }
            npiFsdbTime next_edge = 0;
            if (!edge_cursor.next(next_edge)) break;
            if (next_edge == edge_time) break;
            edge_time = next_edge;
        }
        if (stall_cycles > 0) {
            if (stall_cycles > max_stall) max_stall = stall_cycles;
            if (stall_cycles > max_wait) {
                findings.push_back({{"type", "long_stall"}, {"severity", "warning"},
                                    {"begin", format_time(stall_begin)}, {"end", format_time(stop)},
                                    {"cycles", stall_cycles}});
            }
        }
        return true;
    };

    TimeBasedVcIterGuard guard;
    npiFsdbTimeBasedVcIter& iter = guard.iter();
    iter.add(valid_h);
    iter.add(ready_h);
    guard.start(begin, end);
    npiFsdbTime interval_begin = begin;
    npiFsdbTime curr_time = 0;
    npiFsdbSigHandle changed_sig = nullptr;
    bool keep = true;
    while (keep && iter.iter_next(curr_time, changed_sig) > 0) {
        if (curr_time > end) break;
        if (curr_time > interval_begin) {
            keep = visit_interval(interval_begin, curr_time - 1);
            if (!keep) break;
            interval_begin = curr_time;
        }
        npiFsdbValue val;
        val.format = npiFsdbBinStrVal;
        if (iter.get_value(val) && val.value.str) {
            if (changed_sig == valid_h) valid_value = with_value_prefix(val.value.str, 'b');
            else if (changed_sig == ready_h) ready_value = with_value_prefix(val.value.str, 'b');
        }
    }
    if (keep && interval_begin <= end) visit_interval(interval_begin, end);

    Json data;
    data["sample_count"] = sample_count;
    data["transfer_count"] = transfers;
    data["max_stall_cycles"] = max_stall;
    data["ready_without_valid_cycles"] = ready_only;
    data["data_stability_violations"] = 0;
    data["truncated"] = truncated;
    data["findings"] = findings;
    data["name"] = name;
    data["channel"] = channel;
    return data;
}

static Json cursor_to_json(const Cursor& c) {
    Json j;
    j["name"] = c.name;
    j["time"] = c.time;
    j["time_text"] = c.time_text.empty() ? format_time(c.time) : c.time_text;
    j["note"] = c.note;
    j["origin"] = c.origin;
    j["clock"] = c.clock;
    j["created_at"] = c.created_at;
    j["updated_at"] = c.updated_at;
    return j;
}

static Json resolved_time_json(const std::string& spec, npiFsdbTime time) {
    Json j;
    j["source"] = spec;
    j["time"] = format_time(time);
    j["time_value"] = time;
    return j;
}

static Json ai_cursor_action(const std::string& action, const Json& args, std::string& error) {
    CursorManager cm;
    if (action == "cursor.set") {
        std::string name = args.value("name", std::string());
        std::string spec = args.value("time", args.value("at", std::string()));
        if (name.empty() || spec.empty()) {
            error = "cursor.set requires args.name and args.time";
            return Json();
        }
        npiFsdbTime t = 0;
        if (!parse_user_time(spec.c_str(), false, t, error)) return Json();
        Cursor c;
        c.name = name;
        c.time = t;
        c.time_text = format_time(t);
        c.note = args.value("note", std::string());
        c.origin = args.value("origin", std::string("manual"));
        c.clock = args.value("clock", std::string());
        if (!cm.set_cursor(g_session_id, c, args.value("active", true))) {
            error = "failed to save cursor: " + name;
            return Json();
        }
        Cursor saved;
        cm.get_cursor(g_session_id, name, saved);
        Json data;
        data["cursor"] = cursor_to_json(saved);
        data["resolved_time"] = resolved_time_json(spec, t);
        data["status"] = "set";
        return data;
    }
    if (action == "cursor.get") {
        std::string name = args.value("name", std::string());
        if (name.empty()) {
            error = "cursor.get requires args.name";
            return Json();
        }
        Cursor c;
        if (!cm.get_cursor(g_session_id, name, c)) {
            error = "CURSOR_NOT_FOUND: Cursor '" + name + "' does not exist";
            return Json();
        }
        Json data;
        data["cursor"] = cursor_to_json(c);
        return data;
    }
    if (action == "cursor.list") {
        Json arr = Json::array();
        for (const auto& c : cm.list_cursors(g_session_id)) arr.push_back(cursor_to_json(c));
        std::string active;
        cm.get_active_cursor(g_session_id, active);
        Json data;
        data["cursors"] = arr;
        data["active_cursor"] = active;
        data["cursor_count"] = arr.size();
        return data;
    }
    if (action == "cursor.delete") {
        std::string name = args.value("name", std::string());
        if (name.empty()) {
            error = "cursor.delete requires args.name";
            return Json();
        }
        if (!cm.delete_cursor(g_session_id, name)) {
            error = "CURSOR_NOT_FOUND: Cursor '" + name + "' does not exist";
            return Json();
        }
        Json data;
        data["status"] = "deleted";
        data["name"] = name;
        return data;
    }
    if (action == "cursor.use") {
        std::string name = args.value("name", std::string());
        if (name.empty()) {
            error = "cursor.use requires args.name";
            return Json();
        }
        if (!cm.use_cursor(g_session_id, name)) {
            error = "CURSOR_NOT_FOUND: Cursor '" + name + "' does not exist";
            return Json();
        }
        Cursor c;
        cm.get_cursor(g_session_id, name, c);
        Json data;
        data["status"] = "active";
        data["active_cursor"] = name;
        data["cursor"] = cursor_to_json(c);
        return data;
    }
    error = "Unsupported cursor action: " + action;
    return Json();
}

static Json ai_dispatch_query(const Json& req, std::string& error) {
    std::string action = req.value("action", std::string());
    Json args = req.value("args", Json::object());
    Json limits = req.value("limits", Json::object());
    for (auto it = limits.begin(); it != limits.end(); ++it) {
        if (!args.contains(it.key())) args[it.key()] = it.value();
    }
    if (action == "expr.eval_at") return ai_expr_eval_at(args, error);
    if (action == "window.verify") return ai_window_verify(args, error);
    if (action == "signal.changes") return ai_signal_changes(args, error);
    if (action == "signal.stability") return ai_signal_stability(args, error);
    if (action == "signal.trend") return ai_signal_trend(args, error);
    if (action == "inspect_signal") return ai_inspect_signal(args, error);
    if (action == "detect_anomaly") return ai_detect_anomaly(args, error);
    if (action == "handshake.inspect") return ai_handshake_inspect(args, error);
    if (action == "apb.transfer_window") return ai_apb_transfer_window(args, error);
    if (action == "axi.request_response_pair") return ai_axi_transactions_window(args, error);
    if (action == "axi.latency_outlier") return ai_axi_latency_outlier(args, error);
    if (action == "axi.outstanding_timeline") return ai_axi_outstanding_timeline(args, error);
    if (action == "axi.channel_stall") return ai_axi_channel_stall(args, error);
    if (action.compare(0, 7, "cursor.") == 0) return ai_cursor_action(action, args, error);
    error = "Unsupported AI action in server: " + action;
    return Json();
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

static long long signed_delta(npiFsdbTime t, npiFsdbTime base) {
    if (t >= base) return static_cast<long long>(t - base);
    return -static_cast<long long>(base - t);
}

static const char* relation_to_event(npiFsdbTime t, npiFsdbTime event_time) {
    if (t < event_time) return "before_event";
    if (t > event_time) return "after_event";
    return "at_event";
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

static Json axi_context_to_json(const char* axi_name,
                                npiFsdbTime event_time,
                                npiFsdbTime window_ps,
                                const std::vector<xwave::AxiContextTransaction>& txns) {
    Json ctx;
    ctx["name"] = axi_name ? axi_name : "";
    ctx["window_ps"] = window_ps;
    ctx["transactions"] = Json::array();
    for (const auto& item : txns) {
        Json txn = axi_txn_to_json(item.txn);
        txn["match_time"] = format_time(item.match_time);
        txn["match_time_ps"] = item.match_time;
        txn["relation"] = relation_to_event(item.match_time, event_time);
        txn["delta_ps"] = signed_delta(item.match_time, event_time);
        ctx["transactions"].push_back(txn);
    }
    return ctx;
}

static Json apb_context_to_json(const char* apb_name,
                                npiFsdbTime event_time,
                                npiFsdbTime window_ps,
                                const std::vector<xwave::ApbContextTransaction>& txns) {
    Json ctx;
    ctx["name"] = apb_name ? apb_name : "";
    ctx["window_ps"] = window_ps;
    ctx["transactions"] = Json::array();
    for (const auto& item : txns) {
        Json txn = apb_txn_to_json(item.txn, true);
        npiFsdbTime t = item.txn ? item.txn->time : 0;
        txn["time_ps"] = t;
        txn["relation"] = relation_to_event(t, event_time);
        txn["delta_ps"] = signed_delta(t, event_time);
        ctx["transactions"].push_back(txn);
    }
    return ctx;
}

static std::string format_event_records_with_context_json(
        const std::vector<xwave::EventRecord>& records,
        const std::vector<std::vector<xwave::AxiContextTransaction>>& axi_contexts,
        const std::vector<std::vector<xwave::ApbContextTransaction>>& apb_contexts,
        const char* axi_name,
        const char* apb_name,
        npiFsdbTime window_ps) {
    Json j = Json::array();
    for (size_t i = 0; i < records.size(); ++i) {
        Json rec = event_record_to_json(records[i]);
        Json context;
        if (axi_name) context["axi"] = axi_context_to_json(axi_name, records[i].time, window_ps, axi_contexts[i]);
        if (apb_name) context["apb"] = apb_context_to_json(apb_name, records[i].time, window_ps, apb_contexts[i]);
        rec["context"] = context;
        j.push_back(rec);
    }
    return json_response(j);
}

static std::string format_event_records_with_context_text(
        const std::vector<xwave::EventRecord>& records,
        const std::vector<std::vector<xwave::AxiContextTransaction>>& axi_contexts,
        const std::vector<std::vector<xwave::ApbContextTransaction>>& apb_contexts,
        const char* axi_name,
        const char* apb_name,
        npiFsdbTime window_ps) {
    if (records.empty()) return std::string("(no event found)\n") + END_MARKER;
    std::string out;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        out += "idx=" + std::to_string(i) + " time=" + format_time(rec.time);
        for (const auto& kv : rec.signals) out += " " + kv.first + "=" + kv.second;
        for (const auto& kv : rec.fields) out += " " + kv.first + "=" + kv.second;
        out += "\n";
        if (axi_name) {
            out += "  axi_context name=" + std::string(axi_name)
                + " window=" + format_time(window_ps);
            if (axi_contexts[i].empty()) {
                out += ": none\n";
            } else {
                out += "\n";
                for (const auto& item : axi_contexts[i]) {
                    out += "    " + format_axi_txn(item.txn)
                        + " match_time=" + format_time(item.match_time)
                        + " relation=" + relation_to_event(item.match_time, rec.time)
                        + " delta_ps=" + std::to_string(signed_delta(item.match_time, rec.time))
                        + "\n";
                }
            }
        }
        if (apb_name) {
            out += "  apb_context name=" + std::string(apb_name)
                + " window=" + format_time(window_ps);
            if (apb_contexts[i].empty()) {
                out += ": none\n";
            } else {
                out += "\n";
                for (const auto& item : apb_contexts[i]) {
                    npiFsdbTime t = item.txn ? item.txn->time : 0;
                    out += "    " + format_apb_txn_with_type(item.txn)
                        + " relation=" + relation_to_event(t, rec.time)
                        + " delta_ps=" + std::to_string(signed_delta(t, rec.time))
                        + "\n";
                }
            }
        }
    }
    out += END_MARKER;
    return out;
}

static void handle_event_query(int client_fd,
                               const char* name,
                               npiFsdbTime begin_time,
                               npiFsdbTime end_time,
                               int limit,
                               bool use_json,
                               bool fast_find,
                               const char* expr,
                               const char* axi_context_name = nullptr,
                               const char* apb_context_name = nullptr,
                               npiFsdbTime context_window = 0) {
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
    query.fast_find = fast_find;
    std::vector<xwave::EventRecord> records;
    std::string error;
    if (!g_event_analyzer.analyze(g_fsdb_file, config, query, records, error)) {
        std::string err = std::string(ERROR_PREFIX) + error + "\n" + END_MARKER;
        send_all(client_fd, err.c_str(), err.length());
        return;
    }
    bool use_axi_context = axi_context_name && strcmp(axi_context_name, "-") != 0;
    bool use_apb_context = apb_context_name && strcmp(apb_context_name, "-") != 0;

    std::string resp;
    if (use_axi_context || use_apb_context) {
        std::vector<std::vector<xwave::AxiContextTransaction>> axi_contexts(records.size());
        std::vector<std::vector<xwave::ApbContextTransaction>> apb_contexts(records.size());

        if (use_axi_context) {
            xwave::AxiConfig axi_config;
            if (!read_axi_from_registry(g_session_id, axi_context_name, axi_config)) {
                std::string err = std::string(ERROR_PREFIX) + "AXI config not found: " + axi_context_name + "\n" + END_MARKER;
                send_all(client_fd, err.c_str(), err.length());
                return;
            }
            if (!g_axi_analyzer.analyze(axi_context_name, g_fsdb_file, axi_config)) {
                std::string err = std::string(ERROR_PREFIX) + "Failed to analyze AXI: " + axi_context_name + "\n" + END_MARKER;
                send_all(client_fd, err.c_str(), err.length());
                return;
            }
            for (size_t i = 0; i < records.size(); ++i) {
                npiFsdbTime ctx_begin = records[i].time > context_window ? records[i].time - context_window : 0;
                npiFsdbTime ctx_end = records[i].time + context_window;
                if (ctx_end < records[i].time) ctx_end = 0xFFFFFFFFFFFFFFFFULL;
                if (!g_axi_analyzer.get_transactions_in_range(axi_context_name, ctx_begin, ctx_end, axi_contexts[i])) {
                    std::string err = std::string(ERROR_PREFIX) + "AXI config not analyzed: " + axi_context_name + "\n" + END_MARKER;
                    send_all(client_fd, err.c_str(), err.length());
                    return;
                }
            }
        }
        if (use_apb_context) {
            xwave::ApbConfig apb_config;
            if (!read_apb_from_registry(g_session_id, apb_context_name, apb_config)) {
                std::string err = std::string(ERROR_PREFIX) + "APB config not found: " + apb_context_name + "\n" + END_MARKER;
                send_all(client_fd, err.c_str(), err.length());
                return;
            }
            if (!g_apb_analyzer.analyze(apb_context_name, g_fsdb_file, apb_config)) {
                std::string err = std::string(ERROR_PREFIX) + "Failed to analyze APB: " + apb_context_name + "\n" + END_MARKER;
                send_all(client_fd, err.c_str(), err.length());
                return;
            }
            for (size_t i = 0; i < records.size(); ++i) {
                npiFsdbTime ctx_begin = records[i].time > context_window ? records[i].time - context_window : 0;
                npiFsdbTime ctx_end = records[i].time + context_window;
                if (ctx_end < records[i].time) ctx_end = 0xFFFFFFFFFFFFFFFFULL;
                if (!g_apb_analyzer.get_transactions_in_range(apb_context_name, ctx_begin, ctx_end, apb_contexts[i])) {
                    std::string err = std::string(ERROR_PREFIX) + "APB config not analyzed: " + apb_context_name + "\n" + END_MARKER;
                    send_all(client_fd, err.c_str(), err.length());
                    return;
                }
            }
        }
        resp = use_json
            ? format_event_records_with_context_json(records, axi_contexts, apb_contexts,
                                                     use_axi_context ? axi_context_name : nullptr,
                                                     use_apb_context ? apb_context_name : nullptr,
                                                     context_window)
            : format_event_records_with_context_text(records, axi_contexts, apb_contexts,
                                                     use_axi_context ? axi_context_name : nullptr,
                                                     use_apb_context ? apb_context_name : nullptr,
                                                     context_window);
    } else {
        resp = use_json ? format_event_records_json(records) : format_event_records_text(records);
    }
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

    if (fsdb_changed()) {
        const char* err = ERROR_PREFIX "FSDB changed; session restart required\n" END_MARKER;
        send_all(client_fd, err, strlen(err));
        return true;
    }

    SessionRegistry registry;
    registry.touch(g_session_id, time(nullptr));

    // Handle TIME_RESOLVE <time_spec> [allow_max]
    if (strncmp(cmd, CMD_TIME_RESOLVE, strlen(CMD_TIME_RESOLVE)) == 0) {
        char time_str[256] = {};
        char allow_str[16] = {};
        if (sscanf(cmd + strlen(CMD_TIME_RESOLVE), " %255s %15s", time_str, allow_str) >= 1) {
            npiFsdbTime t = 0;
            std::string error;
            bool allow_max = strcmp(allow_str, "allow_max") == 0;
            if (!parse_user_time(time_str, allow_max, t, error)) {
                send_error(client_fd, error);
                return true;
            }
            Json out = resolved_time_json(time_str, t);
            std::string payload = json_response(out);
            send_all(client_fd, payload.c_str(), payload.size());
        } else {
            send_error(client_fd, "Usage: TIME_RESOLVE <time_spec> [allow_max]");
        }
        return true;
    }

    // Handle VALUE <signal> <time> <fmt>
    if (strncmp(cmd, CMD_VALUE, strlen(CMD_VALUE)) == 0) {
        char signal_path[1024];
        char time_str[256];
        char fmt;
        if (sscanf(cmd + strlen(CMD_VALUE), " %1023s %255s %c", signal_path, time_str, &fmt) == 3) {
            npiFsdbTime t = 0;
            std::string error;
            if (!parse_user_time(time_str, false, t, error)) {
                send_error(client_fd, error);
                return true;
            }
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
        char time_str[256];
        char fmt[16];
        if (sscanf(cmd + strlen(CMD_LIST_VALUE), " %255s %255s %15s", list_name, time_str, fmt) >= 3) {
            npiFsdbTime t = 0;
            std::string error;
            if (!parse_user_time(time_str, false, t, error)) {
                send_error(client_fd, error);
                return true;
            }
            bool json = (strstr(cmd, "json") != nullptr);
            handle_list_value(client_fd, list_name, t, fmt[0], json);
        } else {
            const char* err = ERROR_PREFIX "Usage: LIST_VALUE <list> <time> <fmt> [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle SIGNAL_CHECK <signal>
    if (strncmp(cmd, CMD_SIGNAL_CHECK, strlen(CMD_SIGNAL_CHECK)) == 0) {
        char signal_path[1024];
        if (sscanf(cmd + strlen(CMD_SIGNAL_CHECK), " %1023s", signal_path) == 1) {
            handle_signal_check(client_fd, signal_path);
        } else {
            const char* err = ERROR_PREFIX "Usage: SIGNAL_CHECK <signal>\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle LIST_VALIDATE <list_name> [json]
    if (strncmp(cmd, CMD_LIST_VALIDATE, strlen(CMD_LIST_VALIDATE)) == 0) {
        char list_name[256];
        if (sscanf(cmd + strlen(CMD_LIST_VALIDATE), " %255s", list_name) == 1) {
            bool json = (strstr(cmd, "json") != nullptr);
            handle_list_validate(client_fd, list_name, json);
        } else {
            const char* err = ERROR_PREFIX "Usage: LIST_VALIDATE <list> [json]\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle SCOPE <scope_path> <recursive> <json|text>
    if (strncmp(cmd, CMD_SCOPE, strlen(CMD_SCOPE)) == 0) {
        char scope_path[1024];
        int recursive = 0;
        char mode[16] = {};
        if (sscanf(cmd + strlen(CMD_SCOPE), " %1023s %d %15s", scope_path, &recursive, mode) >= 2) {
            handle_scope(client_fd, scope_path, recursive != 0, strcmp(mode, "json") == 0);
        } else {
            const char* err = ERROR_PREFIX "Usage: SCOPE <scope> <recursive> <json|text>\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle LIST_DIFF <list_name> <begin_time> <end_time>
    if (strncmp(cmd, CMD_LIST_DIFF, strlen(CMD_LIST_DIFF)) == 0) {
        char list_name[256];
        char begin_str[256];
        char end_str[256];
        if (sscanf(cmd + strlen(CMD_LIST_DIFF), " %255s %255s %255s", list_name, begin_str, end_str) == 3) {
            npiFsdbTime begin = 0;
            npiFsdbTime end = 0;
            std::string error;
            if (!parse_user_time(begin_str, false, begin, error) ||
                !parse_user_time(end_str, true, end, error)) {
                send_error(client_fd, error);
                return true;
            }
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

    // Handle EVENT_FIND_CTX|EVENT_EXPORT_CTX <name> <begin> <end> <limit> <json|text> <context> <axi_name|-> <apb_name|-> expr <expr>
    if (strncmp(cmd, CMD_EVENT_FIND_CTX, strlen(CMD_EVENT_FIND_CTX)) == 0 ||
        strncmp(cmd, CMD_EVENT_EXPORT_CTX, strlen(CMD_EVENT_EXPORT_CTX)) == 0) {
        bool find = strncmp(cmd, CMD_EVENT_FIND_CTX, strlen(CMD_EVENT_FIND_CTX)) == 0;
        size_t base_len = find ? strlen(CMD_EVENT_FIND_CTX) : strlen(CMD_EVENT_EXPORT_CTX);
        char name[256] = {};
        char begin_str[256] = {};
        char end_str[256] = {};
        char context_str[256] = {};
        int limit = -1;
        char mode[16] = {};
        char axi_name[256] = {};
        char apb_name[256] = {};
        const char* p = cmd + base_len;
        int matched = sscanf(p, " %255s %255s %255s %d %15s %255s %255s %255s",
                             name, begin_str, end_str, &limit, mode, context_str, axi_name, apb_name);
        const char* expr_p = strstr(p, " expr ");
        if (matched >= 8 && expr_p) {
            expr_p += strlen(" expr ");
            bool use_json = strcmp(mode, "json") == 0;
            if (find) limit = 1;
            npiFsdbTime begin = 0;
            npiFsdbTime end = 0;
            npiFsdbTime context = 0;
            std::string error;
            if (!parse_user_time(begin_str, false, begin, error) ||
                !parse_user_time(end_str, true, end, error) ||
                !parse_user_time(context_str, false, context, error)) {
                send_error(client_fd, error);
                return true;
            }
            handle_event_query(client_fd,
                               name,
                               begin,
                               end,
                               limit,
                               use_json,
                               find,
                               expr_p,
                               strcmp(axi_name, "-") == 0 ? nullptr : axi_name,
                               strcmp(apb_name, "-") == 0 ? nullptr : apb_name,
                               context);
        } else {
            const char* err = ERROR_PREFIX "Usage: EVENT_FIND_CTX|EVENT_EXPORT_CTX <name> <begin> <end> <limit> <json|text> <context> <axi_name|-> <apb_name|-> expr <expr>\n" END_MARKER;
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
        char begin_str[256] = {};
        char end_str[256] = {};
        int limit = -1;
        char mode[16] = {};
        const char* p = cmd + base_len;
        int matched = sscanf(p, " %255s %255s %255s %d %15s", name, begin_str, end_str, &limit, mode);
        const char* expr_p = strstr(p, " expr ");
        if (matched >= 5 && expr_p) {
            expr_p += strlen(" expr ");
            bool use_json = strcmp(mode, "json") == 0;
            if (find) limit = 1;
            npiFsdbTime begin = 0;
            npiFsdbTime end = 0;
            std::string error;
            if (!parse_user_time(begin_str, false, begin, error) ||
                !parse_user_time(end_str, true, end, error)) {
                send_error(client_fd, error);
                return true;
            }
            handle_event_query(client_fd,
                               name,
                               begin,
                               end,
                               limit,
                               use_json,
                               find,
                               expr_p);
        } else {
            const char* err = ERROR_PREFIX "Usage: EVENT_FIND|EVENT_EXPORT <name> <begin> <end> <limit> <json|text> expr <expr>\n" END_MARKER;
            send_all(client_fd, err, strlen(err));
        }
        return true;
    }

    // Handle AI_QUERY <json>
    if (strncmp(cmd, CMD_AI_QUERY, strlen(CMD_AI_QUERY)) == 0) {
        const char* json_p = cmd + strlen(CMD_AI_QUERY);
        while (*json_p == ' ' || *json_p == '\t') json_p++;
        try {
            Json req = Json::parse(json_p);
            std::string error;
            Json data = ai_dispatch_query(req, error);
            if (!error.empty()) {
                send_error(client_fd, error);
            } else {
                std::string resp = json_response(data);
                send_all(client_fd, resp.c_str(), resp.length());
            }
        } catch (const std::exception& e) {
            send_error(client_fd, std::string("Invalid AI_QUERY JSON: ") + e.what());
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
    g_session_id = argv[arg_idx];
    if (!SessionRegistry::is_valid_session_name(g_session_id)) {
        fprintf(stderr, "Invalid session ID: %s\n", argv[arg_idx]);
        return 1;
    }
    server_debug_open_log();
    server_debug_log("server_main: parsed_session_id=%s", g_session_id.c_str());
    arg_idx++;

    // Parse FSDB file
    const char* fsdb_file = argv[arg_idx];
    g_fsdb_file_path = fsdb_file;
    stat_fsdb(g_fsdb_mtime, g_fsdb_size, g_fsdb_dev, g_fsdb_inode);
    server_debug_log("server_main: fsdb=%s stat mtime=%ld size=%lld dev=%llu inode=%llu",
                     fsdb_file, g_fsdb_mtime, g_fsdb_size, g_fsdb_dev, g_fsdb_inode);

    // Redirect stdout to capture NPI init messages, but keep a copy
    int stdout_copy = dup(STDOUT_FILENO);

    // Initialize NPI
    int npi_argc = 1;
    char** npi_argv = argv;
    server_debug_log("server_main: npi_init_begin");
    int result = npi_init(npi_argc, npi_argv);
    if (result == 0) {
        server_debug_log("server_main: npi_init_failed");
        dprintf(stdout_copy, "[Session %s] ERROR: npi_init failed\n", g_session_id.c_str());
        close(stdout_copy);
        if (g_debug_log) {
            fclose(g_debug_log);
            g_debug_log = nullptr;
        }
        return 1;
    }
    server_debug_log("server_main: npi_init_ok");

    server_debug_log("server_main: npi_fsdb_open_begin fsdb=%s", fsdb_file);
    g_fsdb_file = npi_fsdb_open(fsdb_file);
    if (!g_fsdb_file) {
        server_debug_log("server_main: npi_fsdb_open_failed fsdb=%s", fsdb_file);
        dprintf(stdout_copy, "[Session %s] ERROR: npi_fsdb_open failed: %s\n", g_session_id.c_str(), fsdb_file);
        npi_end();
        close(stdout_copy);
        if (g_debug_log) {
            fclose(g_debug_log);
            g_debug_log = nullptr;
        }
        return 1;
    }
    server_debug_log("server_main: npi_fsdb_open_ok");

    npiFsdbTime minTime, maxTime;
    npi_fsdb_min_time(g_fsdb_file, &minTime);
    npi_fsdb_max_time(g_fsdb_file, &maxTime);
    server_debug_log("server_main: fsdb_time min=%llu max=%llu", minTime, maxTime);

    dprintf(stdout_copy, "[Session %s] Ready (FSDB: %llu ~ %llu)\n", g_session_id.c_str(), minTime, maxTime);
    fflush(stdout);
    close(stdout_copy);

    // Now daemonize I/O
    server_debug_log("server_main: daemonize_io");
    daemonize_io();

    // Set up signal handlers
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGINT, cleanup_and_exit);

    // Create socket
    server_debug_log("server_main: socket_create_begin");
    g_srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv_fd < 0) {
        server_debug_log("server_main: socket_create_failed errno=%d(%s)", errno, strerror(errno));
        npi_fsdb_close(g_fsdb_file);
        npi_end();
        if (g_debug_log) {
            fclose(g_debug_log);
            g_debug_log = nullptr;
        }
        return 1;
    }
    server_debug_log("server_main: socket_create_ok fd=%d", g_srv_fd);

    get_sock_path(g_sock_path, g_session_id);
    unlink(g_sock_path);
    server_debug_log("server_main: socket_path=%s", g_sock_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    server_debug_log("server_main: socket_bind_begin path=%s", g_sock_path);
    if (bind(g_srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        server_debug_log("server_main: socket_bind_failed errno=%d(%s)", errno, strerror(errno));
        close(g_srv_fd);
        npi_fsdb_close(g_fsdb_file);
        npi_end();
        if (g_debug_log) {
            fclose(g_debug_log);
            g_debug_log = nullptr;
        }
        return 1;
    }
    chmod(g_sock_path, 0600);
    server_debug_log("server_main: socket_bind_ok");

    server_debug_log("server_main: socket_listen_begin");
    if (listen(g_srv_fd, 8) < 0) {
        server_debug_log("server_main: socket_listen_failed errno=%d(%s)", errno, strerror(errno));
        close(g_srv_fd);
        unlink(g_sock_path);
        npi_fsdb_close(g_fsdb_file);
        npi_end();
        if (g_debug_log) {
            fclose(g_debug_log);
            g_debug_log = nullptr;
        }
        return 1;
    }
    server_debug_log("server_main: socket_listen_ok");

    const char* env_timeout = getenv("XWAVE_IDLE_TIMEOUT_SEC");
    int idle_timeout = env_timeout ? atoi(env_timeout) : 1800;
    if (idle_timeout <= 0) idle_timeout = 1800;
    server_debug_log("server_main: idle_timeout_sec=%d", idle_timeout);
    time_t last_active = time(nullptr);
    bool idle_timeout_exit = false;
    bool quit_requested = false;

    // Accept loop
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_srv_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ready = select(g_srv_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) continue;
        if (ready == 0) {
            if (time(nullptr) - last_active > idle_timeout) {
                idle_timeout_exit = true;
                server_debug_log("server_main: idle_timeout_exit idle_sec=%ld timeout_sec=%d",
                                  static_cast<long>(time(nullptr) - last_active),
                                  idle_timeout);
                break;
            }
            continue;
        }

        int client_fd = accept(g_srv_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        bool quit = false;
        handle_client(client_fd, quit);
        close(client_fd);
        last_active = time(nullptr);

        if (quit) {
            quit_requested = true;
            server_debug_log("server_main: quit_requested");
            break;
        }
    }

    // Cleanup
    server_debug_log("server_main: cleanup_begin reason=%s",
                     idle_timeout_exit ? "idle_timeout" :
                     (quit_requested ? "quit" : "loop_exit"));
    close(g_srv_fd);
    unlink(g_sock_path);
    if (g_fsdb_file) {
        npi_fsdb_close(g_fsdb_file);
        g_fsdb_file = nullptr;
    }
    {
        SessionRegistry registry;
        registry.remove(g_session_id);
    }
    npi_end();
    if (g_debug_log) {
        server_debug_log("server_main: normal_exit");
        fclose(g_debug_log);
        g_debug_log = nullptr;
    }

    return 0;
}

} // namespace xwave
