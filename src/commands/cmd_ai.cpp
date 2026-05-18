#include "cmd_ai.h"

#include "../apb/apb_config.h"
#include "../apb/apb_manager.h"
#include "../axi/axi_config.h"
#include "../axi/axi_manager.h"
#include "../client/client.h"
#include "../event/event_config.h"
#include "../event/event_manager.h"
#include "../json.hpp"
#include "../list/list_manager.h"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"
#include "../session/session_registry.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace xwave {

using Json = nlohmann::ordered_json;

static const char* kApiVersion = "xwave.ai.v1";

static std::string read_stream(std::istream& is) {
    std::ostringstream oss;
    oss << is.rdbuf();
    return oss.str();
}

static bool read_file(const std::string& path, std::string& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    out = read_stream(ifs);
    return true;
}

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static std::string compact_expr_ws(const std::string& expr) {
    std::string out;
    out.reserve(expr.size());
    for (char c : expr) {
        if (!std::isspace(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out;
}

static bool contains_xz(const std::string& value) {
    std::string v = trim(value);
    size_t start = 0;
    if (v.size() >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        start = 2;
    } else if (v.size() >= 2 && v[0] == '\'' &&
               (v[1] == 'h' || v[1] == 'H' || v[1] == 'b' || v[1] == 'B' ||
                v[1] == 'd' || v[1] == 'D')) {
        start = 2;
    }
    for (size_t i = start; i < v.size(); ++i) {
        char c = v[i];
        if (c == 'x' || c == 'X' || c == 'z' || c == 'Z') return true;
    }
    return false;
}

static std::string normalize_numeric(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && value[0] == '\'' && (value[1] == 'h' || value[1] == 'H')) {
        value = "0x" + value.substr(2);
    } else if (value.size() >= 2 && value[0] == '\'' && (value[1] == 'b' || value[1] == 'B')) {
        value = "0b" + value.substr(2);
    } else if (value.size() >= 2 && value[0] == '\'' && (value[1] == 'd' || value[1] == 'D')) {
        value = value.substr(2);
    }
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value = value.substr(2);
    } else if (value.size() > 2 && value[0] == '0' && (value[1] == 'b' || value[1] == 'B')) {
        unsigned long long n = strtoull(value.substr(2).c_str(), nullptr, 2);
        char buf[64];
        snprintf(buf, sizeof(buf), "%llx", n);
        value = buf;
    } else {
        bool decimal = !value.empty();
        for (char c : value) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                decimal = false;
                break;
            }
        }
        if (decimal) {
            unsigned long long n = strtoull(value.c_str(), nullptr, 10);
            char buf[64];
            snprintf(buf, sizeof(buf), "%llx", n);
            value = buf;
        }
    }
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t first = value.find_first_not_of('0');
    if (first == std::string::npos) return "0";
    return value.substr(first);
}

static Json make_value_object(const std::string& raw) {
    Json v;
    std::string text = trim(raw);
    v["text"] = text;
    v["known"] = !contains_xz(text);
    if (!v["known"].get<bool>()) {
        v["unknown_reason"] = contains_xz(text) ? "contains_xz" : "unknown";
    }
    if (text.size() >= 2 && text[0] == '\'' && (text[1] == 'h' || text[1] == 'H')) {
        v["hex"] = "0x" + text.substr(2);
    } else if (text.size() >= 2 && text[0] == '\'' && (text[1] == 'b' || text[1] == 'B')) {
        v["bits"] = text.substr(2);
    }
    if (v["known"].get<bool>()) {
        std::string n = normalize_numeric(text);
        unsigned long long u = strtoull(n.c_str(), nullptr, 16);
        v["unsigned"] = u;
    }
    return v;
}

static Json base_response(const Json& req,
                          const std::string& action,
                          bool ok,
                          long long elapsed_ms) {
    Json out;
    out["api_version"] = kApiVersion;
    if (req.contains("request_id")) out["request_id"] = req["request_id"];
    out["ok"] = ok;
    out["action"] = action;
    out["tool"] = {{"name", "xwave"}, {"version", "0.1.0"}};
    out["session"] = Json::object();
    out["summary"] = Json::object();
    out["data"] = ok ? Json::object() : Json(nullptr);
    out["findings"] = Json::array();
    out["suggested_next_actions"] = Json::array();
    out["warnings"] = Json::array();
    out["error"] = nullptr;
    out["meta"] = {{"elapsed_ms", elapsed_ms}, {"truncated", false}};
    return out;
}

static Json error_response(const Json& req,
                           const std::string& action,
                           const std::string& code,
                           const std::string& message,
                           bool recoverable,
                           long long elapsed_ms) {
    Json out = base_response(req, action, false, elapsed_ms);
    out["error"] = {
        {"code", code},
        {"message", message},
        {"recoverable", recoverable},
        {"candidates", Json::array()},
        {"suggested_actions", Json::array()}
    };
    if (code == "SIGNAL_NOT_FOUND") {
        out["suggested_next_actions"].push_back({
            {"tool", "xwave"},
            {"action", "scope.list"},
            {"reason", "exact signal was not found"}
        });
    }
    return out;
}

static void print_json(const Json& j) {
    printf("%s\n", j.dump(2).c_str());
}

static bool get_string(const Json& obj, const char* key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

static std::string string_or(const Json& obj, const char* key, const std::string& def) {
    std::string v;
    return get_string(obj, key, v) ? v : def;
}

static int int_or(const Json& obj, const char* key, int def) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return def;
    return it->get<int>();
}

static bool bool_or(const Json& obj, const char* key, bool def) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

static std::string create_session_quiet(SessionManager& manager, const std::string& fsdb, const std::string& name);

static bool resolve_session(const Json& target,
                            bool allow_auto_open,
                            std::string& session_id,
                            SessionInfo& info,
                            std::string& error) {
    SessionManager manager;
    session_id.clear();
    auto sid_it = target.find("session_id");
    if (sid_it != target.end()) {
        if (!sid_it->is_string()) {
            error = "target.session_id must be a string";
            return false;
        }
        session_id = sid_it->get<std::string>();
        if (!manager.get_session(session_id, info)) {
            error = "session not found: " + session_id;
            return false;
        }
        if (!manager.ensure_session_current(session_id) || !manager.get_session(session_id, info)) {
            error = "session unavailable: " + session_id;
            return false;
        }
        return true;
    }

    std::string fsdb;
    if (get_string(target, "fsdb", fsdb)) {
        bool auto_open = bool_or(target, "auto_open", allow_auto_open);
        if (!auto_open) {
            error = "target.fsdb requires auto_open=true when session_id is omitted";
            return false;
        }
        std::string name;
        if (!get_string(target, "name", name)) {
            error = "target.name is required when auto-opening an FSDB";
            return false;
        }
        session_id = create_session_quiet(manager, fsdb, name);
        if (session_id.empty() || !manager.get_session(session_id, info)) {
            error = "failed to open fsdb: " + fsdb;
            return false;
        }
        return true;
    }

    if (!manager.get_latest_session(info)) {
        error = "no active session";
        return false;
    }
    if (!manager.ensure_session_current(info.session_id) || !manager.get_session(info.session_id, info)) {
        error = "latest session unavailable";
        return false;
    }
    session_id = info.session_id;
    return true;
}

static std::string create_session_quiet(SessionManager& manager, const std::string& fsdb, const std::string& name) {
    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (saved_stdout >= 0) fcntl(saved_stdout, F_SETFD, FD_CLOEXEC);
    if (saved_stderr >= 0) fcntl(saved_stderr, F_SETFD, FD_CLOEXEC);
    if (devnull >= 0) fcntl(devnull, F_SETFD, FD_CLOEXEC);
    if (saved_stdout >= 0 && devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
    }
    if (saved_stderr >= 0 && devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
    }
    std::string sid = manager.create_session(fsdb, name);
    fflush(stdout);
    fflush(stderr);
    if (saved_stdout >= 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    if (devnull >= 0) close(devnull);
    return sid;
}

static void fill_session(Json& out, const SessionInfo& info) {
    out["session"] = {
        {"id", info.session_id},
        {"fsdb", info.fsdb_file},
        {"pid", info.server_pid},
        {"socket_path", info.socket_path}
    };
}

static bool capture_server_json(const std::string& session_id,
                                const std::string& cmd,
                                Json& data,
                                std::string& error) {
    std::string payload;
    if (!send_command_capture(session_id, cmd.c_str(), payload)) {
        error = trim(payload);
        if (error.compare(0, strlen(ERROR_PREFIX), ERROR_PREFIX) == 0) {
            error = trim(error.substr(strlen(ERROR_PREFIX)));
        }
        if (error.empty()) error = "server command failed";
        return false;
    }
    try {
        data = Json::parse(payload);
    } catch (const std::exception&) {
        data = trim(payload);
    }
    return true;
}

static bool capture_server_text(const std::string& session_id,
                                const std::string& cmd,
                                std::string& payload,
                                std::string& error) {
    if (!send_command_capture(session_id, cmd.c_str(), payload)) {
        error = trim(payload);
        if (error.compare(0, strlen(ERROR_PREFIX), ERROR_PREFIX) == 0) {
            error = trim(error.substr(strlen(ERROR_PREFIX)));
        }
        if (error.empty()) error = "server command failed";
        return false;
    }
    payload = trim(payload);
    return true;
}

static Json session_info_json(const SessionInfo& s) {
    Json j;
    j["id"] = s.session_id;
    j["pid"] = s.server_pid;
    j["socket_path"] = s.socket_path;
    j["fsdb"] = s.fsdb_file;
    j["created_at"] = static_cast<long long>(s.created_at);
    j["last_active"] = static_cast<long long>(s.last_active);
    j["fsdb_mtime"] = s.fsdb_mtime;
    j["fsdb_size"] = s.fsdb_size;
    j["fsdb_dev"] = s.fsdb_dev;
    j["fsdb_inode"] = s.fsdb_inode;
    return j;
}

static bool parse_apb_config(const Json& j, ApbConfig& c, std::string& err) {
    auto get = [&j](const char* key) -> std::string {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : "";
    };
    c.paddr = get("paddr");
    c.pwdata = get("pwdata");
    c.prdata = get("prdata");
    c.pwrite = get("pwrite");
    c.penable = get("penable");
    c.psel = get("psel");
    c.clk = get("clk");
    c.rst_n = get("rst_n");
    std::string edge = get("edge");
    if (edge.empty() || edge == "posedge") c.posedge = true;
    else if (edge == "negedge") c.posedge = false;
    else { err = "invalid APB edge: " + edge; return false; }
    if (c.paddr.empty() || c.pwdata.empty() || c.prdata.empty() || c.pwrite.empty() ||
        c.penable.empty() || c.psel.empty() || c.clk.empty() || c.rst_n.empty()) {
        err = "missing required APB config field";
        return false;
    }
    return true;
}

static Json apb_config_json(const ApbConfig& c) {
    return {
        {"name", c.name}, {"paddr", c.paddr}, {"pwdata", c.pwdata}, {"prdata", c.prdata},
        {"pwrite", c.pwrite}, {"penable", c.penable}, {"psel", c.psel},
        {"clk", c.clk}, {"rst_n", c.rst_n}, {"edge", c.posedge ? "posedge" : "negedge"}
    };
}

static bool parse_axi_config(const Json& j, AxiConfig& c, std::string& err) {
    auto get = [&j](const char* key) -> std::string {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : "";
    };
    c.awaddr = get("awaddr"); c.awid = get("awid"); c.awlen = get("awlen");
    c.awsize = get("awsize"); c.awburst = get("awburst"); c.awvalid = get("awvalid");
    c.awready = get("awready"); c.wdata = get("wdata"); c.wstrb = get("wstrb");
    c.wlast = get("wlast"); c.wvalid = get("wvalid"); c.wready = get("wready");
    c.bid = get("bid"); c.bresp = get("bresp"); c.bvalid = get("bvalid"); c.bready = get("bready");
    c.araddr = get("araddr"); c.arid = get("arid"); c.arlen = get("arlen");
    c.arsize = get("arsize"); c.arburst = get("arburst"); c.arvalid = get("arvalid");
    c.arready = get("arready"); c.rid = get("rid"); c.rdata = get("rdata");
    c.rresp = get("rresp"); c.rlast = get("rlast"); c.rvalid = get("rvalid");
    c.rready = get("rready"); c.clk = get("clk"); c.rst_n = get("rst_n");
    std::string edge = get("edge");
    if (edge.empty() || edge == "posedge") c.posedge = true;
    else if (edge == "negedge") c.posedge = false;
    else { err = "invalid AXI edge: " + edge; return false; }
    if (c.awaddr.empty() || c.awid.empty() || c.awlen.empty() || c.awsize.empty() ||
        c.awburst.empty() || c.awvalid.empty() || c.awready.empty() || c.wdata.empty() ||
        c.wstrb.empty() || c.wlast.empty() || c.wvalid.empty() || c.wready.empty() ||
        c.bid.empty() || c.bresp.empty() || c.bvalid.empty() || c.bready.empty() ||
        c.araddr.empty() || c.arid.empty() || c.arlen.empty() || c.arsize.empty() ||
        c.arburst.empty() || c.arvalid.empty() || c.arready.empty() || c.rid.empty() ||
        c.rdata.empty() || c.rresp.empty() || c.rlast.empty() || c.rvalid.empty() ||
        c.rready.empty() || c.clk.empty() || c.rst_n.empty()) {
        err = "missing required AXI config field";
        return false;
    }
    return true;
}

static Json axi_config_json(const AxiConfig& c) {
    Json j;
    j["name"] = c.name;
    j["awaddr"] = c.awaddr; j["awid"] = c.awid; j["awlen"] = c.awlen;
    j["awsize"] = c.awsize; j["awburst"] = c.awburst; j["awvalid"] = c.awvalid;
    j["awready"] = c.awready; j["wdata"] = c.wdata; j["wstrb"] = c.wstrb;
    j["wlast"] = c.wlast; j["wvalid"] = c.wvalid; j["wready"] = c.wready;
    j["bid"] = c.bid; j["bresp"] = c.bresp; j["bvalid"] = c.bvalid; j["bready"] = c.bready;
    j["araddr"] = c.araddr; j["arid"] = c.arid; j["arlen"] = c.arlen;
    j["arsize"] = c.arsize; j["arburst"] = c.arburst; j["arvalid"] = c.arvalid;
    j["arready"] = c.arready; j["rid"] = c.rid; j["rdata"] = c.rdata;
    j["rresp"] = c.rresp; j["rlast"] = c.rlast; j["rvalid"] = c.rvalid;
    j["rready"] = c.rready; j["clk"] = c.clk; j["rst_n"] = c.rst_n;
    j["edge"] = c.posedge ? "posedge" : "negedge";
    return j;
}

static bool parse_nonnegative_int(const Json& v, int& out) {
    if (!v.is_number_integer()) return false;
    long long n = v.get<long long>();
    if (n < 0 || n > INT_MAX) return false;
    out = static_cast<int>(n);
    return true;
}

static bool parse_field_ref(const std::string& text, EventField& field) {
    size_t lb = text.find('[');
    size_t colon = text.find(':', lb == std::string::npos ? 0 : lb);
    size_t rb = text.find(']', colon == std::string::npos ? 0 : colon);
    if (lb == std::string::npos || colon == std::string::npos ||
        rb == std::string::npos || rb != text.size() - 1) return false;
    field.signal_alias = text.substr(0, lb);
    char* end = nullptr;
    long left = strtol(text.substr(lb + 1, colon - lb - 1).c_str(), &end, 10);
    if (!end || *end != '\0' || left < 0 || left > INT_MAX) return false;
    long right = strtol(text.substr(colon + 1, rb - colon - 1).c_str(), &end, 10);
    if (!end || *end != '\0' || right < 0 || right > INT_MAX) return false;
    field.left = static_cast<int>(left);
    field.right = static_cast<int>(right);
    return !field.signal_alias.empty();
}

static bool parse_event_config(const Json& j, EventConfig& c, std::string& err) {
    if (!get_string(j, "clk", c.clk)) {
        err = "event config requires clk";
        return false;
    }
    get_string(j, "rst_n", c.rst_n);
    std::string edge = string_or(j, "edge", "posedge");
    if (edge == "posedge") c.posedge = true;
    else if (edge == "negedge") c.posedge = false;
    else { err = "invalid event edge: " + edge; return false; }
    auto sig_it = j.find("signals");
    if (sig_it == j.end() || !sig_it->is_object() || sig_it->empty()) {
        err = "event config requires non-empty signals object";
        return false;
    }
    for (auto it = sig_it->begin(); it != sig_it->end(); ++it) {
        if (!it.value().is_string()) {
            err = "event signal alias must map to string path: " + it.key();
            return false;
        }
        c.signals[it.key()] = it.value().get<std::string>();
    }
    auto fields_it = j.find("fields");
    if (fields_it != j.end()) {
        if (!fields_it->is_object()) {
            err = "event fields must be object";
            return false;
        }
        for (auto it = fields_it->begin(); it != fields_it->end(); ++it) {
            EventField f;
            if (it.value().is_string()) {
                if (!parse_field_ref(it.value().get<std::string>(), f)) {
                    err = "invalid field slice: " + it.key();
                    return false;
                }
            } else if (it.value().is_object()) {
                auto left_it = it.value().find("left");
                auto right_it = it.value().find("right");
                if (!get_string(it.value(), "signal", f.signal_alias) ||
                    left_it == it.value().end() || right_it == it.value().end() ||
                    !parse_nonnegative_int(*left_it, f.left) ||
                    !parse_nonnegative_int(*right_it, f.right)) {
                    err = "invalid field object: " + it.key();
                    return false;
                }
            } else {
                err = "invalid field definition: " + it.key();
                return false;
            }
            if (c.signals.find(f.signal_alias) == c.signals.end()) {
                err = "field references unknown signal alias: " + f.signal_alias;
                return false;
            }
            c.fields[it.key()] = f;
        }
    }
    return true;
}

static Json event_config_json(const EventConfig& c) {
    Json j;
    j["name"] = c.name;
    j["clk"] = c.clk;
    if (!c.rst_n.empty()) j["rst_n"] = c.rst_n;
    j["edge"] = c.posedge ? "posedge" : "negedge";
    j["signals"] = c.signals;
    Json fields = Json::object();
    for (const auto& kv : c.fields) {
        fields[kv.first] = {
            {"signal", kv.second.signal_alias},
            {"left", kv.second.left},
            {"right", kv.second.right}
        };
    }
    j["fields"] = fields;
    return j;
}

static bool load_config_json_arg(const Json& args, Json& config, std::string& err) {
    auto cfg_it = args.find("config");
    if (cfg_it != args.end()) {
        if (!cfg_it->is_object()) {
            err = "args.config must be an object";
            return false;
        }
        config = *cfg_it;
        return true;
    }
    std::string path;
    if (!get_string(args, "config_path", path)) {
        err = "missing args.config or args.config_path";
        return false;
    }
    std::string text;
    if (!read_file(path, text)) {
        err = "cannot read config_path: " + path;
        return false;
    }
    try {
        config = Json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("failed to parse config_path: ") + e.what();
        return false;
    }
    return true;
}

static char fmt_char(const Json& args) {
    std::string fmt = string_or(args, "format", "hex");
    if (fmt == "binary" || fmt == "bin") return 'B';
    if (fmt == "decimal" || fmt == "dec") return 'D';
    return 'H';
}

static std::string arg_text(const Json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    return v.dump();
}

static bool query_value(const std::string& session_id,
                        const std::string& signal,
                        const std::string& time,
                        char fmt,
                        std::string& raw,
                        std::string& err) {
    std::string cmd = std::string(CMD_VALUE) + " " + signal + " " + time + " " + fmt;
    return capture_server_text(session_id, cmd, raw, err);
}

static Json resolve_time_spec_json(const std::string& session_id,
                                   const std::string& spec,
                                   bool allow_max,
                                   std::string& err) {
    Json out;
    if (spec.empty()) return out;
    std::string cmd = std::string(CMD_TIME_RESOLVE) + " " + spec + (allow_max ? " allow_max" : "");
    if (!capture_server_json(session_id, cmd, out, err)) return Json();
    return out;
}

enum class Tri { False, True, Unknown };

static Tri tri_not(Tri v) {
    if (v == Tri::Unknown) return Tri::Unknown;
    return v == Tri::True ? Tri::False : Tri::True;
}

static Tri tri_and(Tri a, Tri b) {
    if (a == Tri::False || b == Tri::False) return Tri::False;
    if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
    return Tri::True;
}

static Tri tri_or(Tri a, Tri b) {
    if (a == Tri::True || b == Tri::True) return Tri::True;
    if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
    return Tri::False;
}

static Tri value_to_bool(const std::string& raw) {
    if (contains_xz(raw)) return Tri::Unknown;
    return normalize_numeric(raw) == "0" ? Tri::False : Tri::True;
}

class ExprParser {
public:
    ExprParser(const std::string& text, const Json& values)
        : text_(text), values_(values), pos_(0), ok_(true) {}

    Tri parse() {
        Tri v = parse_or();
        skip_ws();
        if (pos_ != text_.size()) ok_ = false;
        return ok_ ? v : Tri::Unknown;
    }

    bool ok() const { return ok_; }

private:
    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool eat(const std::string& token) {
        skip_ws();
        if (text_.compare(pos_, token.size(), token) == 0) {
            pos_ += token.size();
            return true;
        }
        return false;
    }

    std::string ident() {
        skip_ws();
        size_t start = pos_;
        if (pos_ < text_.size() && (std::isalpha(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_')) {
            ++pos_;
            while (pos_ < text_.size() &&
                   (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_' || text_[pos_] == '.')) {
                ++pos_;
            }
        }
        return text_.substr(start, pos_ - start);
    }

    std::string literal() {
        skip_ws();
        size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '\'') {
            ++pos_;
            if (pos_ < text_.size()) ++pos_;
            while (pos_ < text_.size() && std::isxdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
            return text_.substr(start, pos_ - start);
        }
        while (pos_ < text_.size() &&
               (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == 'x' ||
                text_[pos_] == 'X' || text_[pos_] == '_' || text_[pos_] == '\'')) {
            ++pos_;
        }
        return text_.substr(start, pos_ - start);
    }

    std::string value_for(const std::string& name) {
        auto it = values_.find(name);
        if (it == values_.end() || !it->is_object() || !it->contains("value")) {
            ok_ = false;
            return "";
        }
        return (*it)["value"]["text"].get<std::string>();
    }

    Tri parse_primary() {
        if (eat("(")) {
            Tri v = parse_or();
            if (!eat(")")) ok_ = false;
            return v;
        }
        if (eat("!")) return tri_not(parse_primary());

        std::string name = ident();
        if (name.empty()) {
            ok_ = false;
            return Tri::Unknown;
        }
        bool neq = false;
        if (eat("==") || (neq = eat("!="))) {
            std::string rhs = literal();
            if (rhs.empty()) {
                ok_ = false;
                return Tri::Unknown;
            }
            std::string lhs_val = value_for(name);
            if (contains_xz(lhs_val) || contains_xz(rhs)) return Tri::Unknown;
            bool eq = normalize_numeric(lhs_val) == normalize_numeric(rhs);
            return (neq ? !eq : eq) ? Tri::True : Tri::False;
        }
        return value_to_bool(value_for(name));
    }

    Tri parse_and() {
        Tri v = parse_primary();
        while (eat("&&")) v = tri_and(v, parse_primary());
        return v;
    }

    Tri parse_or() {
        Tri v = parse_and();
        while (eat("||")) v = tri_or(v, parse_and());
        return v;
    }

    std::string text_;
    Json values_;
    size_t pos_;
    bool ok_;
};

static const char* tri_text(Tri v) {
    if (v == Tri::True) return "true";
    if (v == Tri::False) return "false";
    return "unknown";
}

static bool action_known(const std::string& action);
static int run_query(const Json& req, long long elapsed_ms);

static void print_actions() {
    Json actions = Json::array({
        "session.open", "session.list", "session.doctor", "session.gc", "session.kill",
        "cursor.set", "cursor.get", "cursor.list", "cursor.delete", "cursor.use",
        "scope.list",
        "value.at", "value.batch_at",
        "list.create", "list.add", "list.delete", "list.show", "list.value_at", "list.validate", "list.diff",
        "apb.config.load", "apb.config.list", "apb.query", "apb.cursor",
        "axi.config.load", "axi.config.list", "axi.query", "axi.cursor", "axi.analysis",
        "event.config.load", "event.config.list", "event.find", "event.export",
        "verify.conditions", "expr.eval_at",
        "window.verify", "signal.changes", "signal.stability", "signal.trend",
        "inspect_signal", "detect_anomaly", "handshake.inspect",
        "axi.channel_stall", "axi.outstanding_timeline", "axi.request_response_pair",
        "axi.latency_outlier", "apb.transfer_window"
    });
    Json out;
    out["api_version"] = kApiVersion;
    out["actions"] = actions;
    out["implemented"] = Json::array({
        "session.open", "session.list", "session.doctor", "session.gc", "session.kill",
        "cursor.set", "cursor.get", "cursor.list", "cursor.delete", "cursor.use",
        "scope.list", "value.at", "value.batch_at",
        "list.create", "list.add", "list.delete", "list.show", "list.value_at", "list.validate", "list.diff",
        "apb.config.load", "apb.config.list", "apb.query", "apb.cursor",
        "axi.config.load", "axi.config.list", "axi.query", "axi.cursor", "axi.analysis",
        "event.config.load", "event.config.list", "event.find", "event.export",
        "verify.conditions", "expr.eval_at",
        "window.verify", "signal.changes", "signal.stability", "signal.trend",
        "inspect_signal", "detect_anomaly", "handshake.inspect",
        "axi.channel_stall", "axi.outstanding_timeline", "axi.request_response_pair",
        "axi.latency_outlier", "apb.transfer_window"
    });
    out["planned"] = Json::array();
    print_json(out);
}

static void print_schema() {
    Json schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "xwave.ai.v1 request";
    schema["type"] = "object";
    schema["required"] = Json::array({"api_version", "action"});
    schema["properties"] = {
        {"api_version", {{"const", kApiVersion}}},
        {"request_id", {{"type", "string"}}},
        {"action", {{"type", "string"}}},
        {"target", {{"type", "object"}}},
        {"args", {{"type", "object"}, {"description", "Action arguments. Time fields accept TimeSpec strings such as 100ns, @deadlock, @deadlock-20ns, or @+5ns. Structured TimeSpec objects are planned but not implemented in this build."}}},
        {"limits", {{"type", "object"}}},
        {"output", {{"type", "object"}}}
    };
    schema["xwave_time_spec"] = {
        {"implemented", Json::array({"absolute time", "@cursor", "@cursor+duration", "@cursor-duration", "@+duration", "@-duration"})},
        {"planned", Json::array({"structured TimeSpec object", "cycle offset"})}
    };
    print_json(schema);
}

static int print_error_and_return(const Json& req,
                                  const std::string& action,
                                  const std::string& code,
                                  const std::string& msg,
                                  long long elapsed_ms) {
    print_json(error_response(req, action, code, msg, true, elapsed_ms));
    return 1;
}

static bool action_known(const std::string& action) {
    static const std::vector<std::string> implemented = {
        "session.open", "session.list", "session.doctor", "session.gc", "session.kill",
        "cursor.set", "cursor.get", "cursor.list", "cursor.delete", "cursor.use",
        "scope.list", "value.at", "value.batch_at",
        "list.create", "list.add", "list.delete", "list.show", "list.value_at", "list.validate", "list.diff",
        "apb.config.load", "apb.config.list", "apb.query", "apb.cursor",
        "axi.config.load", "axi.config.list", "axi.query", "axi.cursor", "axi.analysis",
        "event.config.load", "event.config.list", "event.find", "event.export",
        "verify.conditions", "expr.eval_at",
        "window.verify", "signal.changes", "signal.stability", "signal.trend",
        "inspect_signal", "detect_anomaly", "handshake.inspect",
        "axi.channel_stall", "axi.outstanding_timeline", "axi.request_response_pair",
        "axi.latency_outlier", "apb.transfer_window"
    };
    return std::find(implemented.begin(), implemented.end(), action) != implemented.end();
}

static bool server_ai_action(const std::string& action) {
    static const std::vector<std::string> server_actions = {
        "cursor.set", "cursor.get", "cursor.list", "cursor.delete", "cursor.use",
        "expr.eval_at",
        "window.verify", "signal.changes", "signal.stability", "signal.trend",
        "inspect_signal", "detect_anomaly", "handshake.inspect",
        "axi.channel_stall", "axi.outstanding_timeline", "axi.request_response_pair",
        "axi.latency_outlier", "apb.transfer_window"
    };
    return std::find(server_actions.begin(), server_actions.end(), action) != server_actions.end();
}

static int run_query(const Json& req, long long elapsed_ms) {
    std::string action;
    if (!get_string(req, "action", action)) {
        return print_error_and_return(req, "", "MISSING_FIELD", "request.action is required", elapsed_ms);
    }
    if (!action_known(action)) {
        return print_error_and_return(req, action, "UNKNOWN_ACTION", "action is not implemented: " + action, elapsed_ms);
    }
    Json target = req.value("target", Json::object());
    Json args = req.value("args", Json::object());
    Json limits = req.value("limits", Json::object());
    int max_rows = int_or(limits, "max_rows", int_or(limits, "max_events", 1000));

    auto ok_out = [&](const SessionInfo* info = nullptr) {
        Json out = base_response(req, action, true, elapsed_ms);
        if (info) fill_session(out, *info);
        return out;
    };

    if (action == "session.open") {
        std::string fsdb;
        if (!get_string(target, "fsdb", fsdb)) {
            return print_error_and_return(req, action, "MISSING_FIELD", "target.fsdb is required", elapsed_ms);
        }
        std::string name;
        if (!get_string(args, "name", name) && !get_string(target, "name", name)) {
            return print_error_and_return(req, action, "MISSING_FIELD", "session.open requires args.name", elapsed_ms);
        }
        if (!SessionRegistry::is_valid_session_name(name)) {
            return print_error_and_return(req, action, "INVALID_SESSION_ID", "invalid session name: " + name, elapsed_ms);
        }
        SessionManager manager;
        SessionRegistry registry;
        if (registry.exists(name)) {
            return print_error_and_return(req, action, "SESSION_ID_EXISTS", "session id already exists: " + name, elapsed_ms);
        }
        std::string sid = create_session_quiet(manager, fsdb, name);
        SessionInfo info;
        if (sid.empty() || !manager.get_session(sid, info)) {
            return print_error_and_return(req, action, "INVALID_TARGET", "failed to open fsdb: " + fsdb, elapsed_ms);
        }
        Json out = ok_out(&info);
        out["summary"] = {{"session_id", sid}, {"fsdb", info.fsdb_file}};
        out["data"]["session"] = session_info_json(info);
        print_json(out);
        return 0;
    }

    if (action == "session.list") {
        SessionManager manager;
        Json out = ok_out();
        Json arr = Json::array();
        for (const auto& s : manager.list_sessions()) arr.push_back(session_info_json(s));
        out["summary"] = {{"session_count", arr.size()}};
        out["data"]["sessions"] = arr;
        print_json(out);
        return 0;
    }

    if (action == "session.gc") {
        SessionManager manager;
        manager.gc_sessions();
        Json out = ok_out();
        out["summary"] = {{"status", "completed"}};
        print_json(out);
        return 0;
    }

    if (action == "session.kill") {
        SessionManager manager;
        bool ok = false;
        if (string_or(args, "id", "") == "all" || string_or(args, "session_id", "") == "all") {
            ok = manager.kill_all_sessions();
        } else {
            std::string sid = string_or(target, "session_id", string_or(args, "session_id", string_or(args, "id", "")));
            if (sid.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "session.kill requires target.session_id or args.id", elapsed_ms);
            ok = manager.kill_session(sid);
        }
        if (!ok) return print_error_and_return(req, action, "SESSION_UNHEALTHY", "failed to kill session", elapsed_ms);
        Json out = ok_out();
        out["summary"] = {{"status", "removed"}};
        print_json(out);
        return 0;
    }

    if (action == "session.doctor") {
        std::string sid = string_or(target, "session_id", string_or(args, "session_id", ""));
        if (sid.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "session.doctor requires session_id", elapsed_ms);
        SessionManager manager;
        SessionHealth h = manager.diagnose_session(sid);
        Json out = base_response(req, action, h.healthy, elapsed_ms);
        fill_session(out, h.info);
        out["summary"] = {{"healthy", h.healthy}, {"status", session_health_status_name(h.status)}, {"message", h.message}};
        out["data"]["health"] = out["summary"];
        if (!h.healthy) {
            out["error"] = {{"code", "SESSION_UNHEALTHY"}, {"message", h.message}, {"recoverable", true}, {"candidates", Json::array()}, {"suggested_actions", Json::array()}};
        }
        print_json(out);
        return h.healthy ? 0 : 1;
    }

    std::string sid;
    SessionInfo info;
    std::string err;
    if (!resolve_session(target, true, sid, info, err)) {
        return print_error_and_return(req, action, "SESSION_NOT_FOUND", err, elapsed_ms);
    }

    if (server_ai_action(action)) {
        Json data;
        std::string cmd = std::string(CMD_AI_QUERY) + " " + req.dump();
        if (!capture_server_json(sid, cmd, data, err)) {
            std::string code = err.find("Signal not found") != std::string::npos ? "SIGNAL_NOT_FOUND" :
                               err.find("Clock signal not found") != std::string::npos ? "SIGNAL_NOT_FOUND" :
                               err.find("CURSOR_NOT_FOUND") != std::string::npos ? "CURSOR_NOT_FOUND" :
                               err.find("TIME_OUT_OF_RANGE") != std::string::npos ? "TIME_OUT_OF_RANGE" :
                               err.find("CLOCK_OFFSET_UNSUPPORTED") != std::string::npos ? "CLOCK_OFFSET_UNSUPPORTED" :
                               err.find("Invalid time") != std::string::npos ? "TIME_SPEC_INVALID" :
                               err.find("Invalid TimeSpec") != std::string::npos ? "TIME_SPEC_INVALID" :
                               err.find("config not found") != std::string::npos ? "INVALID_REQUEST" :
                               err.find("expression") != std::string::npos ? "EXPR_PARSE_FAILED" :
                               "WAVE_QUERY_FAILED";
            return print_error_and_return(req, action, code, err, elapsed_ms);
        }
        Json out = ok_out(&info);
        out["data"] = data;
        Json tr = args.value("time_range", Json::object());
        std::string begin_spec = string_or(tr, "begin", string_or(args, "begin", ""));
        std::string end_spec = string_or(tr, "end", string_or(args, "end", ""));
        if (!begin_spec.empty() || !end_spec.empty()) {
            Json range;
            if (!begin_spec.empty()) range["begin"] = resolve_time_spec_json(sid, begin_spec, false, err);
            if (!end_spec.empty()) range["end"] = resolve_time_spec_json(sid, end_spec, true, err);
            out["data"]["resolved_time_range"] = range;
        }
        std::string at_spec = string_or(args, "at", string_or(args, "time", ""));
        if (!at_spec.empty() && out["data"].is_object() && !out["data"].contains("resolved_time")) {
            Json resolved = resolve_time_spec_json(sid, at_spec, false, err);
            if (!resolved.is_null()) out["data"]["resolved_time"] = resolved;
        }
        if (data.contains("truncated")) out["meta"]["truncated"] = data["truncated"];
        if (data.contains("findings")) out["findings"] = data["findings"];
        if (action == "window.verify") {
            out["summary"] = {{"all_passed", data.value("all_passed", false)},
                              {"sample_count", data.value("sample_count", 0)},
                              {"failed_samples", data.value("failed_samples", 0)},
                              {"unknown_samples", data.value("unknown_samples", 0)}};
        } else if (action == "handshake.inspect") {
            out["summary"] = {{"transfer_count", data.value("transfer_count", 0)},
                              {"max_stall_cycles", data.value("max_stall_cycles", 0)}};
        } else if (action == "detect_anomaly") {
            out["summary"] = {{"finding_count", data.value("finding_count", 0)}};
        } else if (data.contains("transaction_count")) {
            out["summary"] = {{"transaction_count", data["transaction_count"]}};
        } else if (data.contains("sample_count")) {
            out["summary"] = {{"sample_count", data["sample_count"]}};
        } else if (data.contains("transition_count")) {
            out["summary"] = {{"transition_count", data["transition_count"]}};
        } else if (data.contains("status")) {
            out["summary"] = {{"status", data["status"]}, {"known", data.value("known", false)}};
        }
        print_json(out);
        return 0;
    }

    if (action == "value.at") {
        std::string signal, time;
        if (!get_string(args, "signal", signal)) {
            return print_error_and_return(req, action, "MISSING_FIELD", "value.at requires args.signal", elapsed_ms);
        }
        if (!get_string(args, "at", time) && !get_string(args, "time", time)) {
            return print_error_and_return(req, action, "MISSING_FIELD", "value.at requires args.time or args.at", elapsed_ms);
        }
        std::string raw;
        if (!query_value(sid, signal, time, fmt_char(args), raw, err)) {
            bool not_found = err.find("Signal not found") != std::string::npos ||
                             err.find("Failed to read value for signal") != std::string::npos;
            std::string code = not_found ? "SIGNAL_NOT_FOUND" :
                               err.find("CURSOR_NOT_FOUND") != std::string::npos ? "CURSOR_NOT_FOUND" :
                               err.find("TIME_OUT_OF_RANGE") != std::string::npos ? "TIME_OUT_OF_RANGE" :
                               err.find("CLOCK_OFFSET_UNSUPPORTED") != std::string::npos ? "CLOCK_OFFSET_UNSUPPORTED" :
                               err.find("Invalid") != std::string::npos ? "TIME_SPEC_INVALID" :
                               "WAVE_QUERY_FAILED";
            return print_error_and_return(req, action, code, err, elapsed_ms);
        }
        Json out = ok_out(&info);
        out["summary"] = {{"signal", signal}, {"time", time}, {"known", !contains_xz(raw)}};
        out["data"]["signal"] = signal;
        out["data"]["time"] = time;
        Json resolved = resolve_time_spec_json(sid, time, false, err);
        if (!resolved.is_null()) out["data"]["resolved_time"] = resolved;
        out["data"]["value"] = make_value_object(raw);
        print_json(out);
        return 0;
    }

    if (action == "value.batch_at") {
        std::string time;
        if ((!get_string(args, "at", time) && !get_string(args, "time", time)) ||
            !args.contains("signals") || !args["signals"].is_array()) {
            return print_error_and_return(req, action, "MISSING_FIELD", "value.batch_at requires args.time/args.at and args.signals[]", elapsed_ms);
        }
        Json arr = Json::array();
        int unknown = 0, missing = 0;
        for (const auto& s : args["signals"]) {
            if (!s.is_string()) continue;
            std::string signal = s.get<std::string>();
            std::string raw;
            Json item;
            item["signal"] = signal;
            item["time"] = time;
            if (query_value(sid, signal, time, fmt_char(args), raw, err)) {
                item["status"] = "ok";
                item["value"] = make_value_object(raw);
                if (contains_xz(raw)) unknown++;
            } else {
                item["status"] = "not_found";
                item["value"] = nullptr;
                item["error"] = err;
                missing++;
            }
            arr.push_back(item);
        }
        Json out = ok_out(&info);
        out["summary"] = {{"time", time}, {"signal_count", arr.size()}, {"unknown_count", unknown}, {"missing_count", missing}};
        Json resolved = resolve_time_spec_json(sid, time, false, err);
        if (!resolved.is_null()) out["data"]["resolved_time"] = resolved;
        out["data"]["values"] = arr;
        print_json(out);
        return missing == 0 ? 0 : 1;
    }

    if (action == "scope.list") {
        std::string path;
        if (!get_string(args, "path", path)) return print_error_and_return(req, action, "MISSING_FIELD", "scope.list requires args.path", elapsed_ms);
        bool recursive = bool_or(args, "recursive", false);
        Json data;
        std::string cmd = std::string(CMD_SCOPE) + " " + path + " " + (recursive ? "1" : "0") + " json";
        if (!capture_server_json(sid, cmd, data, err)) return print_error_and_return(req, action, "WAVE_QUERY_FAILED", err, elapsed_ms);
        bool truncated = false;
        if (data.is_array() && max_rows >= 0 && data.size() > static_cast<size_t>(max_rows)) {
            Json limited = Json::array();
            for (int i = 0; i < max_rows; ++i) limited.push_back(data[i]);
            data = limited;
            truncated = true;
        }
        Json out = ok_out(&info);
        out["summary"] = {{"path", path}, {"recursive", recursive}, {"signal_count", data.is_array() ? data.size() : 0}, {"truncated", truncated}};
        out["data"]["signals"] = data;
        out["meta"]["truncated"] = truncated;
        print_json(out);
        return 0;
    }

    if (action.compare(0, 5, "list.") == 0) {
        ListManager lm;
        std::string name = string_or(args, "name", string_or(args, "list", ""));
        if (name.empty() && action != "list.create") lm.get_latest_list(sid, name);
        if (action == "list.create") {
            if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "list.create requires args.name", elapsed_ms);
            if (!lm.create_list(sid, name)) return print_error_and_return(req, action, "INTERNAL_ERROR", "failed to create list", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"status", "created"}}; print_json(out); return 0;
        }
        if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "list action requires args.name or latest list", elapsed_ms);
        if (action == "list.add") {
            std::string signal;
            if (!get_string(args, "signal", signal)) return print_error_and_return(req, action, "MISSING_FIELD", "list.add requires args.signal", elapsed_ms);
            std::string payload;
            if (!capture_server_text(sid, std::string(CMD_SIGNAL_CHECK) + " " + signal, payload, err)) {
                return print_error_and_return(req, action, "SIGNAL_NOT_FOUND", err, elapsed_ms);
            }
            if (!lm.add_signal(sid, name, signal)) return print_error_and_return(req, action, "INTERNAL_ERROR", "failed to add signal", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"signal", signal}, {"status", "added"}}; print_json(out); return 0;
        }
        if (action == "list.delete") {
            std::string signal = string_or(args, "signal", string_or(args, "index", ""));
            if (signal.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "list.delete requires args.signal or args.index", elapsed_ms);
            if (!lm.del_signal(sid, name, signal)) return print_error_and_return(req, action, "INTERNAL_ERROR", "failed to delete signal", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"removed", signal}}; print_json(out); return 0;
        }
        if (action == "list.show") {
            SignalList list;
            if (!lm.get_list(sid, name, list)) return print_error_and_return(req, action, "INVALID_REQUEST", "list not found", elapsed_ms);
            Json arr = Json::array();
            for (size_t i = 0; i < list.signals.size(); ++i) arr.push_back({{"index", i + 1}, {"signal", list.signals[i]}});
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"signal_count", arr.size()}}; out["data"]["signals"] = arr; print_json(out); return 0;
        }
        if (action == "list.value_at") {
            std::string time;
            if (!get_string(args, "at", time) && !get_string(args, "time", time)) {
                return print_error_and_return(req, action, "MISSING_FIELD", "list.value_at requires args.time or args.at", elapsed_ms);
            }
            Json data;
            std::string cmd = std::string(CMD_LIST_VALUE) + " " + name + " " + time + " " + fmt_char(args) + " json";
            bool ok = capture_server_json(sid, cmd, data, err);
            Json out = base_response(req, action, ok, elapsed_ms);
            fill_session(out, info);
            out["summary"] = {{"name", name}, {"time", time}};
            out["data"] = data;
            Json resolved = resolve_time_spec_json(sid, time, false, err);
            if (!resolved.is_null()) out["data"]["resolved_time"] = resolved;
            if (!ok) out["error"] = {{"code", "SIGNAL_NOT_FOUND"}, {"message", err}, {"recoverable", true}, {"candidates", Json::array()}, {"suggested_actions", Json::array()}};
            print_json(out);
            return ok ? 0 : 1;
        }
        if (action == "list.validate") {
            Json data;
            bool ok = capture_server_json(sid, std::string(CMD_LIST_VALIDATE) + " " + name + " json", data, err);
            Json out = base_response(req, action, ok, elapsed_ms);
            fill_session(out, info);
            out["summary"] = {{"name", name}, {"all_found", ok}};
            out["data"]["signals"] = data;
            if (!ok) out["error"] = {{"code", "SIGNAL_NOT_FOUND"}, {"message", err}, {"recoverable", true}, {"candidates", Json::array()}, {"suggested_actions", Json::array()}};
            print_json(out);
            return ok ? 0 : 1;
        }
        if (action == "list.diff") {
            std::string begin = string_or(args, "begin", "0ns");
            std::string end = string_or(args, "end", "max");
            std::string payload;
            if (!capture_server_text(sid, std::string(CMD_LIST_DIFF) + " " + name + " " + begin + " " + end, payload, err)) {
                return print_error_and_return(req, action, "WAVE_QUERY_FAILED", err, elapsed_ms);
            }
            Json out = ok_out(&info);
            out["summary"] = {{"name", name}, {"diff_time", payload}};
            out["data"]["time"] = payload;
            out["data"]["resolved_time_range"]["begin"] = resolve_time_spec_json(sid, begin, false, err);
            out["data"]["resolved_time_range"]["end"] = resolve_time_spec_json(sid, end, true, err);
            print_json(out); return 0;
        }
    }

    if (action.compare(0, 4, "apb.") == 0) {
        ApbManager am;
        std::string name = string_or(args, "name", "");
        if (action == "apb.config.load") {
            if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "apb.config.load requires args.name", elapsed_ms);
            Json cfg_json; if (!load_config_json_arg(args, cfg_json, err)) return print_error_and_return(req, action, "INVALID_REQUEST", err, elapsed_ms);
            ApbConfig cfg; if (!parse_apb_config(cfg_json, cfg, err)) return print_error_and_return(req, action, "INVALID_REQUEST", err, elapsed_ms);
            cfg.name = name;
            if (!am.create_apb(sid, cfg)) return print_error_and_return(req, action, "INTERNAL_ERROR", "failed to save APB config", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"status", "loaded"}}; out["data"]["config"] = apb_config_json(cfg); print_json(out); return 0;
        }
        if (name.empty()) am.get_latest_apb(sid, name);
        if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "APB action requires args.name or latest config", elapsed_ms);
        if (action == "apb.config.list") {
            ApbConfig cfg; if (!am.get_apb(sid, name, cfg)) return print_error_and_return(req, action, "INVALID_REQUEST", "APB config not found", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}}; out["data"]["config"] = apb_config_json(cfg); print_json(out); return 0;
        }
        std::string cmd;
        if (action == "apb.query") {
            std::string dir = string_or(args, "direction", "wr");
            cmd = std::string(dir == "rd" ? CMD_APB_RD : CMD_APB_WR) + " " + name;
            if (args.contains("address")) cmd += " addr " + arg_text(args["address"]);
            if (args.contains("num")) cmd += " num " + std::to_string(args["num"].get<int>());
            if (bool_or(args, "last", false)) cmd += " last";
            cmd += " json";
        } else {
            std::string op = string_or(args, "op", "begin");
            const char* pcmd = op == "next" ? CMD_APB_NEXT : op == "pre" ? CMD_APB_PREV : op == "last" ? CMD_APB_LAST : CMD_APB_BEGIN;
            cmd = std::string(pcmd) + " " + name + " " + string_or(args, "direction", "all") + " json";
        }
        Json data; if (!capture_server_json(sid, cmd, data, err)) return print_error_and_return(req, action, "WAVE_QUERY_FAILED", err, elapsed_ms);
        Json out = ok_out(&info); out["summary"] = {{"name", name}}; out["data"] = data; print_json(out); return 0;
    }

    if (action.compare(0, 4, "axi.") == 0) {
        AxiManager am;
        std::string name = string_or(args, "name", "");
        if (action == "axi.config.load") {
            if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "axi.config.load requires args.name", elapsed_ms);
            Json cfg_json; if (!load_config_json_arg(args, cfg_json, err)) return print_error_and_return(req, action, "INVALID_REQUEST", err, elapsed_ms);
            AxiConfig cfg; if (!parse_axi_config(cfg_json, cfg, err)) return print_error_and_return(req, action, "INVALID_REQUEST", err, elapsed_ms);
            cfg.name = name;
            if (!am.create_axi(sid, cfg)) return print_error_and_return(req, action, "INTERNAL_ERROR", "failed to save AXI config", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"status", "loaded"}}; out["data"]["config"] = axi_config_json(cfg); print_json(out); return 0;
        }
        if (name.empty()) am.get_latest_axi(sid, name);
        if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "AXI action requires args.name or latest config", elapsed_ms);
        if (action == "axi.config.list") {
            AxiConfig cfg; if (!am.get_axi(sid, name, cfg)) return print_error_and_return(req, action, "INVALID_REQUEST", "AXI config not found", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}}; out["data"]["config"] = axi_config_json(cfg); print_json(out); return 0;
        }
        std::string cmd;
        if (action == "axi.query") {
            std::string dir = string_or(args, "direction", "wr");
            cmd = std::string(dir == "rd" ? CMD_AXI_RD : CMD_AXI_WR) + " " + name;
            if (args.contains("address")) cmd += " addr " + arg_text(args["address"]);
            if (args.contains("id")) cmd += " id " + arg_text(args["id"]);
            if (args.contains("num")) cmd += " num " + std::to_string(args["num"].get<int>());
            if (bool_or(args, "last", false)) cmd += " last";
            cmd += " json";
        } else if (action == "axi.analysis") {
            std::string analysis = string_or(args, "analysis", "latency");
            cmd = std::string(analysis == "osd" ? CMD_AXI_OSD : CMD_AXI_LATENCY) + " " + name + " " + string_or(args, "direction", "all");
            if (args.contains("id")) cmd += " id " + arg_text(args["id"]);
            cmd += " json";
        } else {
            std::string op = string_or(args, "op", "begin");
            const char* pcmd = op == "next" ? CMD_AXI_NEXT : op == "pre" ? CMD_AXI_PREV : op == "last" ? CMD_AXI_LAST : CMD_AXI_BEGIN;
            cmd = std::string(pcmd) + " " + name + " " + string_or(args, "direction", "all") + " json";
        }
        Json data; if (!capture_server_json(sid, cmd, data, err)) return print_error_and_return(req, action, "WAVE_QUERY_FAILED", err, elapsed_ms);
        Json out = ok_out(&info); out["summary"] = {{"name", name}}; out["data"] = data; print_json(out); return 0;
    }

    if (action.compare(0, 6, "event.") == 0) {
        EventManager em;
        std::string name = string_or(args, "name", "");
        if (action == "event.config.load") {
            if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "event.config.load requires args.name", elapsed_ms);
            Json cfg_json; if (!load_config_json_arg(args, cfg_json, err)) return print_error_and_return(req, action, "INVALID_REQUEST", err, elapsed_ms);
            EventConfig cfg; if (!parse_event_config(cfg_json, cfg, err)) return print_error_and_return(req, action, "INVALID_REQUEST", err, elapsed_ms);
            cfg.name = name;
            if (!em.create_event(sid, info.fsdb_file, cfg)) return print_error_and_return(req, action, "INTERNAL_ERROR", "failed to save event config", elapsed_ms);
            Json out = ok_out(&info); out["summary"] = {{"name", name}, {"status", "loaded"}}; out["data"]["config"] = event_config_json(cfg); print_json(out); return 0;
        }
        if (action == "event.config.list") {
            Json out = ok_out(&info);
            if (name.empty()) {
                Json arr = em.list_events(sid, info.fsdb_file);
                out["summary"] = {{"count", arr.size()}};
                out["data"]["events"] = arr;
            } else {
                EventConfig cfg; if (!em.get_event(sid, info.fsdb_file, name, cfg)) return print_error_and_return(req, action, "INVALID_REQUEST", "event config not found", elapsed_ms);
                out["summary"] = {{"name", name}};
                out["data"]["config"] = event_config_json(cfg);
            }
            print_json(out); return 0;
        }
        if (name.empty()) em.get_latest_event(sid, info.fsdb_file, name);
        if (name.empty()) return print_error_and_return(req, action, "MISSING_FIELD", "event action requires args.name or latest config", elapsed_ms);
        std::string expr; if (!get_string(args, "expr", expr)) return print_error_and_return(req, action, "MISSING_FIELD", "event.find/export requires args.expr", elapsed_ms);
        expr = compact_expr_ws(expr);
        Json tr = args.value("time_range", Json::object());
        std::string begin = string_or(tr, "begin", string_or(args, "begin", "0ns"));
        std::string end = string_or(tr, "end", string_or(args, "end", "max"));
        int limit = action == "event.find" ? 1 : int_or(limits, "max_rows", int_or(args, "limit", 1000));
        Json ctx = args.value("context", Json::object());
        std::string mode = "json";
        std::string cmd;
        if (ctx.is_object() && ctx.contains("window")) {
            std::string window = string_or(ctx, "window", "0ns");
            std::string axi = string_or(ctx, "axi", "-"); if (axi.empty()) axi = "-";
            std::string apb = string_or(ctx, "apb", "-"); if (apb.empty()) apb = "-";
            cmd = std::string(action == "event.find" ? CMD_EVENT_FIND_CTX : CMD_EVENT_EXPORT_CTX) + " " + name + " " + begin + " " + end + " " + std::to_string(limit) + " " + mode + " " + window + " " + axi + " " + apb + " expr " + expr;
        } else {
            cmd = std::string(action == "event.find" ? CMD_EVENT_FIND : CMD_EVENT_EXPORT) + " " + name + " " + begin + " " + end + " " + std::to_string(limit) + " " + mode + " expr " + expr;
        }
        Json data; if (!capture_server_json(sid, cmd, data, err)) return print_error_and_return(req, action, "WAVE_QUERY_FAILED", err, elapsed_ms);
        Json out = ok_out(&info);
        out["summary"] = {{"name", name}, {"begin", begin}, {"end", end}};
        out["data"]["events"] = data;
        out["data"]["resolved_time_range"]["begin"] = resolve_time_spec_json(sid, begin, false, err);
        out["data"]["resolved_time_range"]["end"] = resolve_time_spec_json(sid, end, true, err);
        print_json(out); return 0;
    }

    if (action == "verify.conditions") {
        std::string time;
        if ((!get_string(args, "at", time) && !get_string(args, "time", time)) ||
            !args.contains("conditions") || !args["conditions"].is_array()) {
            return print_error_and_return(req, action, "MISSING_FIELD", "verify.conditions requires args.time/args.at and args.conditions[]", elapsed_ms);
        }
        Json checks = Json::array();
        int passed = 0, failed = 0, unknown = 0;
        for (const auto& cond : args["conditions"]) {
            std::string signal, op, expected;
            get_string(cond, "signal", signal);
            get_string(cond, "op", op);
            get_string(cond, "value", expected);
            Json item = {{"signal", signal}, {"time", time}, {"op", op}, {"expected", expected}};
            std::string raw;
            if (!query_value(sid, signal, time, 'H', raw, err)) {
                item["status"] = "unknown"; item["known"] = false; item["pass"] = nullptr; item["error"] = err; unknown++;
            } else if (contains_xz(raw) || contains_xz(expected)) {
                item["observed"] = raw; item["status"] = "unknown"; item["known"] = false; item["pass"] = nullptr; unknown++;
            } else {
                bool eq = normalize_numeric(raw) == normalize_numeric(expected);
                bool pass = (op == "!=") ? !eq : eq;
                item["observed"] = raw; item["status"] = pass ? "pass" : "fail"; item["known"] = true; item["pass"] = pass;
                if (pass) passed++; else failed++;
            }
            checks.push_back(item);
        }
        Json out = ok_out(&info);
        out["summary"] = {{"all_passed", failed == 0 && unknown == 0}, {"passed", passed}, {"failed", failed}, {"unknown", unknown}};
        Json resolved = resolve_time_spec_json(sid, time, false, err);
        if (!resolved.is_null()) out["data"]["resolved_time"] = resolved;
        out["data"]["checks"] = checks;
        print_json(out);
        return 0;
    }

    if (action == "expr.eval_at") {
        std::string time, expr;
        if ((!get_string(args, "at", time) && !get_string(args, "time", time)) ||
            !get_string(args, "expr", expr) || !args.contains("signals") || !args["signals"].is_object()) {
            return print_error_and_return(req, action, "MISSING_FIELD", "expr.eval_at requires args.time/args.at, args.expr, args.signals", elapsed_ms);
        }
        Json values = Json::object();
        Json operands = Json::array();
        int unknown = 0;
        for (auto it = args["signals"].begin(); it != args["signals"].end(); ++it) {
            std::string alias = it.key();
            std::string signal = it.value().get<std::string>();
            std::string raw;
            Json item = {{"alias", alias}, {"signal", signal}};
            if (query_value(sid, signal, time, 'H', raw, err)) {
                item["value"] = make_value_object(raw);
                if (contains_xz(raw)) unknown++;
            } else {
                item["value"] = nullptr;
                item["error"] = err;
                unknown++;
            }
            values[alias] = item;
            operands.push_back(item);
        }
        ExprParser parser(expr, values);
        Tri v = parser.parse();
        if (!parser.ok()) return print_error_and_return(req, action, "EXPR_PARSE_FAILED", "failed to parse expression", elapsed_ms);
        Json out = ok_out(&info);
        out["summary"] = {{"expr", expr}, {"expr_value", v == Tri::True ? Json(true) : v == Tri::False ? Json(false) : Json(nullptr)}, {"status", tri_text(v)}, {"known", v != Tri::Unknown}};
        Json resolved = resolve_time_spec_json(sid, time, false, err);
        if (!resolved.is_null()) out["data"]["resolved_time"] = resolved;
        out["data"]["operands"] = operands;
        out["data"]["unknown_count"] = unknown;
        print_json(out);
        return 0;
    }

    return print_error_and_return(req, action, "UNKNOWN_ACTION", "unhandled action: " + action, elapsed_ms);
}

int cmd_ai(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s ai <query|schema|actions> ...\n", argv[0]);
        return 1;
    }
    std::string sub = argv[2];
    if (sub == "actions") {
        print_actions();
        return 0;
    }
    if (sub == "schema") {
        print_schema();
        return 0;
    }
    if (sub != "query") {
        fprintf(stderr, "Unknown ai subcommand: %s\n", sub.c_str());
        return 1;
    }

    std::string text;
    if (argc >= 5 && std::string(argv[3]) == "--json") {
        text = argv[4];
    } else if (argc >= 4 && std::string(argv[3]) == "-") {
        text = read_stream(std::cin);
    } else if (argc >= 4) {
        if (!read_file(argv[3], text)) {
            Json empty = Json::object();
            print_json(error_response(empty, "", "INVALID_REQUEST", std::string("cannot read request file: ") + argv[3], true, 0));
            return 1;
        }
    } else {
        fprintf(stderr, "Usage: %s ai query <request.json>|-|--json '<json>'\n", argv[0]);
        return 1;
    }

    auto start = std::chrono::steady_clock::now();
    Json req;
    try {
        req = Json::parse(text);
    } catch (const std::exception& e) {
        Json empty = Json::object();
        print_json(error_response(empty, "", "INVALID_REQUEST", std::string("invalid JSON: ") + e.what(), true, 0));
        return 1;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::string api;
    if (!get_string(req, "api_version", api) || api != kApiVersion) {
        return print_error_and_return(req, string_or(req, "action", ""), "UNSUPPORTED_API_VERSION", "api_version must be xwave.ai.v1", elapsed);
    }
    return run_query(req, elapsed);
}

} // namespace xwave
