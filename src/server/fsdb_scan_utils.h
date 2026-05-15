#pragma once

#include "npi_fsdb.h"
#include "npi_L1.h"

#include <map>
#include <string>
#include <vector>

namespace xwave {

class VctHandle {
public:
    explicit VctHandle(npiFsdbSigHandle sig);
    ~VctHandle();

    VctHandle(const VctHandle&) = delete;
    VctHandle& operator=(const VctHandle&) = delete;

    bool valid() const { return vct_ != nullptr; }
    npiFsdbVctHandle get() const { return vct_; }

private:
    npiFsdbVctHandle vct_ = nullptr;
};

class ClockEdgeCursor {
public:
    ClockEdgeCursor(npiFsdbSigHandle clk, bool posedge);

    bool valid() const { return vct_.valid(); }
    bool first_at_or_after(npiFsdbTime time, npiFsdbTime& out_time);
    bool next(npiFsdbTime& out_time);
    bool prev_before(npiFsdbTime time, npiFsdbTime& out_time);

private:
    VctHandle vct_;
    npiFsdbL1Edge_e edge_;
};

class SignalChangeCursor {
public:
    SignalChangeCursor(npiFsdbSigHandle sig, npiFsdbValType format);

    bool valid() const { return vct_.valid(); }
    bool first_at_or_after(npiFsdbTime time, npiFsdbTime& out_time, std::string& out_value);
    bool next(npiFsdbTime& out_time, std::string& out_value);
    bool prev_before(npiFsdbTime time, npiFsdbTime& out_time, std::string& out_value);

private:
    bool read_current(npiFsdbTime& out_time, std::string& out_value);

    VctHandle vct_;
    npiFsdbValType format_;
};

class TimeBasedVcIterGuard {
public:
    TimeBasedVcIterGuard() = default;
    ~TimeBasedVcIterGuard();

    TimeBasedVcIterGuard(const TimeBasedVcIterGuard&) = delete;
    TimeBasedVcIterGuard& operator=(const TimeBasedVcIterGuard&) = delete;

    npiFsdbTimeBasedVcIter& iter() { return iter_; }
    void start(npiFsdbTime begin, npiFsdbTime end);
    void stop();

private:
    npiFsdbTimeBasedVcIter iter_;
    bool started_ = false;
};

class SampleCache {
public:
    bool read(const std::vector<npiFsdbSigHandle>& handles,
              npiFsdbTime time,
              npiFsdbValType format,
              std::vector<std::string>& out_values);
    void clear();

private:
    std::map<std::string, std::vector<std::string>> cache_;
};

std::string value_with_prefix(const std::string& value, char prefix);

} // namespace xwave
