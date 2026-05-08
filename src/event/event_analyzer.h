#pragma once

#include "event_config.h"
#include "npi_fsdb.h"
#include <map>
#include <string>
#include <vector>

namespace xwave {

struct EventRecord {
    npiFsdbTime time = 0;
    std::map<std::string, std::string> signals;
    std::map<std::string, std::string> fields;
};

struct EventQuery {
    std::string expr;
    npiFsdbTime begin = 0;
    npiFsdbTime end = 0xFFFFFFFFFFFFFFFFULL;
    int limit = -1;
};

class EventAnalyzer {
public:
    bool analyze(npiFsdbFileHandle file,
                 const EventConfig& config,
                 const EventQuery& query,
                 std::vector<EventRecord>& records,
                 std::string& error);

private:
    bool validate_config(npiFsdbFileHandle file,
                         const EventConfig& config,
                         std::vector<std::string>& ordered_aliases,
                         std::vector<std::string>& ordered_paths,
                         std::string& error);
};

} // namespace xwave
