#include "fsdb_value_reader.h"
#include "npi_fsdb.h"
#include "npi_L1.h"
#include <set>

namespace xwave {

npiFsdbValType parse_format(char fmt) {
    switch (fmt) {
        case 'B': case 'b': return npiFsdbBinStrVal;
        case 'D': case 'd': return npiFsdbDecStrVal;
        case 'H': case 'h': default: return npiFsdbHexStrVal;
    }
}

bool read_sig_value_at(npiFsdbFileHandle file,
                       const char* signal_path,
                       npiFsdbTime time,
                       char fmt,
                       std::string& out_value) {
    npiFsdbValType vtype = parse_format(fmt);
    if (npi_fsdb_sig_value_at(file, signal_path, time, out_value, vtype)) {
        return true;
    }
    return false;
}

bool read_sig_vec_value_at(npiFsdbFileHandle file,
                           const std::vector<std::string>& signals,
                           npiFsdbTime time,
                           char fmt,
                           std::vector<std::string>& out_values) {
    if (signals.empty()) return false;
    fsdbSigNameVec_t sigNames(signals.begin(), signals.end());
    fsdbValVec_t values;
    npiFsdbValType vtype = parse_format(fmt);
    if (npi_fsdb_sig_vec_value_at(file, sigNames, time, values, vtype)) {
        out_values.assign(values.begin(), values.end());
        return true;
    }
    return false;
}

bool read_sig_vec_value_at_with_status(npiFsdbFileHandle file,
                                       const std::vector<std::string>& signals,
                                       npiFsdbTime time,
                                       char fmt,
                                       std::vector<std::string>& out_values,
                                       std::vector<bool>& out_found) {
    out_values.clear();
    out_found.clear();
    out_values.reserve(signals.size());
    out_found.reserve(signals.size());

    bool all_found = true;
    for (const auto& signal : signals) {
        std::string value;
        bool found = read_sig_value_at(file, signal.c_str(), time, fmt, value);
        out_found.push_back(found);
        out_values.push_back(found ? value : "NOT_FOUND");
        if (!found) all_found = false;
    }
    return all_found;
}

bool find_list_diff(npiFsdbFileHandle file,
                    const std::vector<std::string>& signals,
                    npiFsdbTime begin_time,
                    npiFsdbTime end_time,
                    npiFsdbTime& diff_time) {
    if (signals.empty()) return false;

    // Get handles for all signals
    std::vector<npiFsdbSigHandle> handles;
    for (const auto& path : signals) {
        npiFsdbSigHandle sig = npi_fsdb_sig_by_name(file, path.c_str(), NULL);
        if (!sig) return false;
        handles.push_back(sig);
    }

    // Read initial values at begin_time
    std::vector<std::string> current_values;
    if (!read_sig_vec_value_at(file, signals, begin_time, 'H', current_values)) {
        return false;
    }

    // Check if already different at begin_time
    if (current_values.size() >= 2) {
        bool all_same = true;
        for (size_t i = 1; i < current_values.size(); ++i) {
            if (current_values[i] != current_values[0]) {
                all_same = false;
                break;
            }
        }
        if (!all_same) {
            diff_time = begin_time;
            return true;
        }
    }

    // Use time-based VC iterator to track changes
    npiFsdbTimeBasedVcIter iter;
    for (auto sig : handles) {
        iter.add(sig);
    }

    iter.iter_start(begin_time, end_time);

    npiFsdbTime curr_time;
    npiFsdbSigHandle changed_sig;
    bool found = false;

    while (iter.iter_next(curr_time, changed_sig) > 0) {
        // Find the index of the changed signal
        int changed_idx = -1;
        for (size_t i = 0; i < handles.size(); ++i) {
            if (handles[i] == changed_sig) {
                changed_idx = (int)i;
                break;
            }
        }
        if (changed_idx < 0) continue;

        // Get the new value of the changed signal
        npiFsdbValue val;
        val.format = npiFsdbHexStrVal;
        if (iter.get_value(val) && val.value.str) {
            current_values[changed_idx] = val.value.str;
        }

        // Check if all values are still the same
        bool all_same = true;
        for (size_t i = 1; i < current_values.size(); ++i) {
            if (current_values[i] != current_values[0]) {
                all_same = false;
                break;
            }
        }

        if (!all_same) {
            diff_time = curr_time;
            found = true;
            break;
        }
    }

    iter.iter_stop();
    return found;
}

} // namespace xwave
