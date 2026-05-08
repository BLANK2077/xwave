#include "list_manager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

namespace xwave {

ListManager::ListManager() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(lists_path_, sizeof(lists_path_), "%s/.xwave.lists", home);
}

ListManager::~ListManager() {
}

static bool lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

static bool unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

bool ListManager::parse_line(const char* line, SignalList& list, int& session_id) {
    char name_buf[256];
    if (sscanf(line, "%d|%255[^|\n]", &session_id, name_buf) != 2) {
        return false;
    }
    list.name = name_buf;
    list.signals.clear();

    char* mutable_line = strdup(line);
    char* p = strchr(mutable_line, '|');
    if (p) {
        p = strchr(p + 1, '|');
        while (p) {
            p++;
            char* end = strchr(p, '|');
            if (end) *end = '\0';
            if (strlen(p) > 0) {
                list.signals.push_back(p);
            }
            p = end;
        }
    }
    free(mutable_line);
    return true;
}

bool ListManager::load_all(std::vector<SignalList>& lists, std::vector<int>& session_ids) {
    lists.clear();
    session_ids.clear();

    int fd = open(lists_path_, O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        unlock_file(fd);
        close(fd);
        return false;
    }

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        SignalList list;
        int sid;
        if (parse_line(line, list, sid)) {
            lists.push_back(list);
            session_ids.push_back(sid);
        }
    }

    fclose(fp);
    return true;
}

bool ListManager::save_all(const std::vector<SignalList>& lists, const std::vector<int>& session_ids) {
    int fd = open(lists_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    for (size_t i = 0; i < lists.size(); ++i) {
        std::string line = std::to_string(session_ids[i]) + "|" + lists[i].name;
        for (const auto& sig : lists[i].signals) {
            line += "|" + sig;
        }
        line += "\n";
        write(fd, line.c_str(), line.length());
    }

    unlock_file(fd);
    close(fd);
    return true;
}

bool ListManager::create_list(int session_id, const std::string& name) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    for (size_t i = 0; i < lists.size(); ++i) {
        if (session_ids[i] == session_id && lists[i].name == name) {
            return false; // already exists
        }
    }

    SignalList list;
    list.name = name;
    lists.push_back(list);
    session_ids.push_back(session_id);
    return save_all(lists, session_ids);
}

bool ListManager::delete_list(int session_id, const std::string& name) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    std::vector<SignalList> new_lists;
    std::vector<int> new_session_ids;
    bool found = false;
    for (size_t i = 0; i < lists.size(); ++i) {
        if (session_ids[i] == session_id && lists[i].name == name) {
            found = true;
            continue;
        }
        new_lists.push_back(lists[i]);
        new_session_ids.push_back(session_ids[i]);
    }
    if (!found) return false;
    return save_all(new_lists, new_session_ids);
}

bool ListManager::add_signal(int session_id, const std::string& list_name, const std::string& signal) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    for (size_t i = 0; i < lists.size(); ++i) {
        if (session_ids[i] == session_id && lists[i].name == list_name) {
            lists[i].signals.push_back(signal);
            // Move modified list to end so it becomes the latest
            SignalList modified = lists[i];
            int modified_sid = session_ids[i];
            lists.erase(lists.begin() + i);
            session_ids.erase(session_ids.begin() + i);
            lists.push_back(modified);
            session_ids.push_back(modified_sid);
            return save_all(lists, session_ids);
        }
    }
    return false;
}

bool ListManager::del_signal(int session_id, const std::string& list_name, const std::string& path_or_index) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    for (size_t i = 0; i < lists.size(); ++i) {
        if (session_ids[i] == session_id && lists[i].name == list_name) {
            bool is_index = true;
            for (char c : path_or_index) {
                if (!isdigit(static_cast<unsigned char>(c))) {
                    is_index = false;
                    break;
                }
            }

            if (is_index) {
                int idx = atoi(path_or_index.c_str());
                if (idx <= 0 || idx > (int)lists[i].signals.size()) return false;
                lists[i].signals.erase(lists[i].signals.begin() + (idx - 1));
            } else {
                bool found = false;
                for (auto it = lists[i].signals.begin(); it != lists[i].signals.end(); ++it) {
                    if (*it == path_or_index) {
                        lists[i].signals.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            // Move modified list to end so it becomes the latest
            SignalList modified = lists[i];
            int modified_sid = session_ids[i];
            lists.erase(lists.begin() + i);
            session_ids.erase(session_ids.begin() + i);
            lists.push_back(modified);
            session_ids.push_back(modified_sid);
            return save_all(lists, session_ids);
        }
    }
    return false;
}

bool ListManager::get_list(int session_id, const std::string& name, SignalList& list) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    for (size_t i = 0; i < lists.size(); ++i) {
        if (session_ids[i] == session_id && lists[i].name == name) {
            list = lists[i];
            return true;
        }
    }
    return false;
}

bool ListManager::get_latest_list(int session_id, std::string& name) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    for (int i = (int)lists.size() - 1; i >= 0; --i) {
        if (session_ids[i] == session_id) {
            name = lists[i].name;
            return true;
        }
    }
    return false;
}

std::vector<SignalList> ListManager::list_all(int session_id) {
    std::vector<SignalList> lists;
    std::vector<int> session_ids;
    load_all(lists, session_ids);

    std::vector<SignalList> result;
    for (size_t i = 0; i < lists.size(); ++i) {
        if (session_ids[i] == session_id) {
            result.push_back(lists[i]);
        }
    }
    return result;
}

} // namespace xwave
