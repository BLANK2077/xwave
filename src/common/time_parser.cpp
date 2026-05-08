#include "time_parser.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace xwave {

npiFsdbTime parse_time_string(const char* str) {
    if (!str || *str == '\0') return 0;

    char* endptr = nullptr;
    double val = strtod(str, &endptr);
    if (endptr == str) return 0;

    // Default unit is ns -> 1000 ps
    double multiplier = 1000.0;

    // Trim whitespace from endptr
    while (*endptr && std::isspace(static_cast<unsigned char>(*endptr))) endptr++;

    if (*endptr != '\0') {
        // Parse unit suffix
        if (strcasecmp(endptr, "us") == 0) {
            multiplier = 1000000.0;
        } else if (strcasecmp(endptr, "ns") == 0) {
            multiplier = 1000.0;
        } else if (strcasecmp(endptr, "ps") == 0) {
            multiplier = 1.0;
        } else if (strcasecmp(endptr, "fs") == 0) {
            multiplier = 0.001;
        }
        // If unrecognized suffix, stick with default ns
    }

    return static_cast<npiFsdbTime>(val * multiplier);
}

} // namespace xwave
