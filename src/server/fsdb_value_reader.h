#pragma once

#include "npi.h"
#include "npi_fsdb.h"
#include <string>
#include <vector>

namespace xwave {

// Map format char to NPI value type
// 'H' -> HexStrVal, 'B' -> BinStrVal, 'D' -> DecStrVal
npiFsdbValType parse_format(char fmt);

// Read a single signal value at a specific time
bool read_sig_value_at(npiFsdbFileHandle file,
                       const char* signal_path,
                       npiFsdbTime time,
                       char fmt,
                       std::string& out_value);

// Read multiple signal values at a specific time
bool read_sig_vec_value_at(npiFsdbFileHandle file,
                           const std::vector<std::string>& signals,
                           npiFsdbTime time,
                           char fmt,
                           std::vector<std::string>& out_values);

// Find the earliest time where not all signals in the list have the same value
bool find_list_diff(npiFsdbFileHandle file,
                    const std::vector<std::string>& signals,
                    npiFsdbTime begin_time,
                    npiFsdbTime end_time,
                    npiFsdbTime& diff_time);

} // namespace xwave
