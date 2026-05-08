#pragma once

#include "npi_fsdb.h"

namespace xwave {

// Parse a time string with optional unit suffix.
// Defaults to ns if no suffix is provided.
// Supported suffixes: us, ns, ps, fs (case-insensitive)
// Returns the FSDB internal time (using 1ps as base unit for conversion).
npiFsdbTime parse_time_string(const char* str);

} // namespace xwave