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

/// slang and Verilator expand $VAR in file names. A path containing '$' is
/// returned as a path relative to `cwd` (where the tool will run) through a
/// symlink `cwd/links/qfv_dir_*` to its directory, so relative `include`s still
/// resolve; other paths are returned unchanged. Throws std::runtime_error if
/// the file or directory name itself contains '$'.
std::string dollarFreePath(const std::string& path, const std::string& cwd, bool isDir = false);

/// Writes a slang command file (`read_slang -F FILE`) holding `args`, one per
/// line, each quoted: paths, defines and include dirs never pass through the
/// Yosys script tokenizer (which read_slang does not unquote). Returns "" or an
/// error (an argument slang's command-file syntax cannot quote).
std::string writeSlangArgsFile(const std::string& path, const std::vector<std::string>& args);

/// Replays a BTOR2 witness with btorsim ($QFV_BTORSIM). "confirmed" only if
/// btorsim ran to completion, exited 0 and reports bad property `bad` reached
/// in frame `depth`; otherwise "unavailable", "timeout" or "not-reproduced".
/// The full btorsim output goes to `log`.
std::string replayBtorsim(const std::string& model, const std::string& witness, size_t bad, int depth,
                          std::chrono::seconds timeout, std::string* log = nullptr);

/// Copies a BTOR2 file keeping only the `bad` line with id `badId`, so an
/// external engine checks exactly one property.
bool writeSingleBadBtor2(const std::string& in, const std::string& out, int64_t badId);

} // namespace qfv
