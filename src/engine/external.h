// Runs an external model checker as a subprocess with a deadline and a cancel
// flag (the portfolio's building block; SPEC section 6.1). External tools are
// only ever invoked as processes, which keeps their licenses separate.
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace qfv {

struct ProcessResult {
    bool finished = false; // false: killed on deadline or cancel
    int exitCode = -1;
    std::string output;    // stdout + stderr
};

ProcessResult runProcess(const std::vector<std::string>& argv,
                         std::chrono::steady_clock::time_point deadline,
                         const std::atomic<bool>& cancel, const std::string& cwd = "");

/// Copies a BTOR2 file keeping only the `bad` line with id `badId`, so an
/// external engine checks exactly one property.
bool writeSingleBadBtor2(const std::string& in, const std::string& out, int64_t badId);

} // namespace qfv
