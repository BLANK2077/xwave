#include "cmd_event.h"
#include "cmd_session.h"
#include "../client/client.h"
#include "../common/time_parser.h"
#include "../event/event_manager.h"
#include "../json.hpp"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <fstream>
#include <sstream>
#include <vector>

namespace xwave {

static bool resolve_session_info(int sid, SessionInfo& info) {
    SessionManager manager;
    if (sid >= 0) return manager.get_session(sid, info);
    return manager.get_latest_session(info);
}

static bool parse_nonnegative_int(const std::string& text, int& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long parsed = strtol(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > INT_MAX) return false;
    value = static_cast<int>(parsed);
    return true;
}

static bool read_json_int(const nlohmann::json& j, const char* key, int& value) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    long long parsed = it->get<long long>();
    if (parsed < 0 || parsed > INT_MAX) return false;
    value = static_cast<int>(parsed);
    return true;
}

static bool read_json_string(const nlohmann::json& j, const char* key, std::string& value) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    value = it->get<std::string>();
    return true;
}

static bool read_file_to_string(const char* path, std::string& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return true;
}

static bool parse_field_ref(const std::string& text, EventField& field) {
    size_t lb = text.find('[');
    size_t colon = text.find(':', lb == std::string::npos ? 0 : lb);
    size_t rb = text.find(']', colon == std::string::npos ? 0 : colon);
    if (lb == std::string::npos || colon == std::string::npos ||
        rb == std::string::npos || rb != text.size() - 1) return false;
    field.signal_alias = text.substr(0, lb);
    return !field.signal_alias.empty() &&
           parse_nonnegative_int(text.substr(lb + 1, colon - lb - 1), field.left) &&
           parse_nonnegative_int(text.substr(colon + 1, rb - colon - 1), field.right);
}

static bool load_json_config(const char* json_path, EventConfig& config) {
    std::string text;
    if (!read_file_to_string(json_path, text)) {
        fprintf(stderr, "Error: Cannot read %s\n", json_path);
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        fprintf(stderr, "Error: Failed to parse JSON in %s\n", json_path);
        return false;
    }
    if (!j.is_object()) {
        fprintf(stderr, "Error: JSON root must be an object in %s\n", json_path);
        return false;
    }
    auto get = [&j](const char* key) -> std::string {
        auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return "";
        return it->get<std::string>();
    };

    config.clk = get("clk");
    config.rst_n = get("rst_n");
    std::string edge = get("edge");
    if (edge.empty() || edge == "posedge") {
        config.posedge = true;
    } else if (edge == "negedge") {
        config.posedge = false;
    } else {
        fprintf(stderr, "Error: Invalid edge '%s' in %s; expected posedge or negedge\n",
                edge.c_str(), json_path);
        return false;
    }
    config.signals.clear();
    config.fields.clear();

    if (j.contains("signals") && j["signals"].is_object()) {
        for (auto it = j["signals"].begin(); it != j["signals"].end(); ++it) {
            if (!it.value().is_string()) {
                fprintf(stderr, "Error: Signal %s must be a string path\n", it.key().c_str());
                return false;
            }
            config.signals[it.key()] = it.value().get<std::string>();
        }
    }
    if (j.contains("fields") && j["fields"].is_object()) {
        for (auto it = j["fields"].begin(); it != j["fields"].end(); ++it) {
            EventField field;
            if (it.value().is_string()) {
                if (!parse_field_ref(it.value().get<std::string>(), field)) {
                    fprintf(stderr, "Error: Invalid field slice for %s\n", it.key().c_str());
                    return false;
                }
            } else if (it.value().is_object()) {
                if (!read_json_string(it.value(), "signal", field.signal_alias) ||
                    !read_json_int(it.value(), "left", field.left) ||
                    !read_json_int(it.value(), "right", field.right)) {
                    fprintf(stderr, "Error: Invalid field definition for %s\n", it.key().c_str());
                    return false;
                }
            } else {
                fprintf(stderr, "Error: Invalid field definition for %s\n", it.key().c_str());
                return false;
            }
            if (field.signal_alias.empty()) {
                fprintf(stderr, "Error: Field %s requires a signal alias\n", it.key().c_str());
                return false;
            }
            if (config.signals.find(field.signal_alias) == config.signals.end()) {
                fprintf(stderr, "Error: Field %s references unknown signal alias: %s\n",
                        it.key().c_str(), field.signal_alias.c_str());
                return false;
            }
            config.fields[it.key()] = field;
        }
    }
    if (config.clk.empty() || config.signals.empty()) {
        fprintf(stderr, "Error: Event JSON requires clk and signals\n");
        return false;
    }
    return true;
}

static void print_event_config(const EventConfig& config) {
    nlohmann::ordered_json j;
    j["clk"] = config.clk;
    if (!config.rst_n.empty()) j["rst_n"] = config.rst_n;
    j["edge"] = config.posedge ? "posedge" : "negedge";
    j["signals"] = config.signals;
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
    for (const auto& kv : config.fields) {
        fields[kv.first] = kv.second.signal_alias + "[" + std::to_string(kv.second.left) + ":" + std::to_string(kv.second.right) + "]";
    }
    j["fields"] = fields;
    printf("%s\n", j.dump(2).c_str());
}

static bool resolve_event_name(EventManager& em,
                               int session_id,
                               const std::string& fsdb_file,
                               const char* explicit_name,
                               std::string& out_name) {
    if (explicit_name) {
        out_name = explicit_name;
        return true;
    }
    if (!em.get_latest_event(session_id, fsdb_file, out_name)) {
        fprintf(stderr, "Error: No event configs found for session %d and FSDB %s\n",
                session_id, fsdb_file.c_str());
        return false;
    }
    return true;
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s event <json-file> -n <name> [-s <sid>]\n", prog);
    fprintf(stderr, "       %s event list [-n <name>] [-s <sid>]\n", prog);
    fprintf(stderr, "       %s event find -n <name> -expr <expr> [-b T] [-e T] [-json] [-s <sid>]\n", prog);
    fprintf(stderr, "       %s event export -n <name> -expr <expr> [-b T] [-e T] [-limit N] [-json] [-s <sid>]\n", prog);
}

int cmd_event(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* subcmd_or_file = argv[2];
    if (strcmp(subcmd_or_file, "find") != 0 &&
        strcmp(subcmd_or_file, "export") != 0 &&
        strcmp(subcmd_or_file, "list") != 0) {
        int session_id = -1;
        const char* name = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        }
        if (!name) {
            fprintf(stderr, "Error: -n <name> is required for loading event config\n");
            return 1;
        }
        SessionInfo session;
        if (!resolve_session_info(session_id, session)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        session_id = session.session_id;
        EventConfig config;
        if (!load_json_config(subcmd_or_file, config)) return 1;
        config.name = name;
        EventManager em;
        if (!em.create_event(session_id, session.fsdb_file, config)) {
            fprintf(stderr, "Error: Failed to create event config '%s'\n", name);
            return 1;
        }
        printf("Event config '%s' loaded for session %d.\n", name, session_id);
        return 0;
    }

    if (strcmp(subcmd_or_file, "list") == 0) {
        int session_id = -1;
        const char* name = nullptr;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
            else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        }
        SessionInfo session;
        if (!resolve_session_info(session_id, session)) {
            fprintf(stderr, "Error: No active sessions\n");
            return 1;
        }
        session_id = session.session_id;
        EventManager em;
        if (name) {
            EventConfig config;
            if (!em.get_event(session_id, session.fsdb_file, name, config)) {
                fprintf(stderr, "Error: Event config '%s' not found\n", name);
                return 1;
            }
            print_event_config(config);
        } else {
            std::vector<std::string> names = em.list_events(session_id, session.fsdb_file);
            for (const auto& n : names) printf("%s\n", n.c_str());
        }
        return 0;
    }

    const char* subcmd = subcmd_or_file;
    int session_id = -1;
    const char* name = nullptr;
    const char* expr = nullptr;
    const char* begin_str = nullptr;
    const char* end_str = nullptr;
    int limit = -1;
    bool json = false;

    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) session_id = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "-expr") == 0 && i + 1 < argc) expr = argv[++i];
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) begin_str = argv[++i];
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) end_str = argv[++i];
        else if (strcmp(argv[i], "-limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "-json") == 0) json = true;
    }

    if (!expr || expr[0] == '\0') {
        fprintf(stderr, "Error: -expr <expr> is required\n");
        return 1;
    }
    SessionInfo session;
    if (!resolve_session_info(session_id, session)) {
        fprintf(stderr, "Error: No active sessions\n");
        return 1;
    }
    session_id = session.session_id;
    EventManager em;
    std::string event_name;
    if (!resolve_event_name(em, session_id, session.fsdb_file, name, event_name)) return 1;

    npiFsdbTime begin = begin_str ? parse_time_string(begin_str) : 0;
    npiFsdbTime end = end_str ? parse_time_string(end_str) : 0xFFFFFFFFFFFFFFFFULL;
    if (strcmp(subcmd, "find") == 0) limit = 1;
    else if (limit <= 0) limit = -1;

    std::string protocol_cmd = std::string(strcmp(subcmd, "find") == 0 ? CMD_EVENT_FIND : CMD_EVENT_EXPORT);
    protocol_cmd += " " + event_name;
    protocol_cmd += " " + std::to_string(begin);
    protocol_cmd += " " + std::to_string(end);
    protocol_cmd += " " + std::to_string(limit);
    protocol_cmd += json ? " json expr " : " text expr ";
    protocol_cmd += expr;

    if (!send_command_and_print(session_id, protocol_cmd.c_str())) return 1;
    return 0;
}

} // namespace xwave
