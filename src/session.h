// A design session: sources are parsed once and the syntax trees cached, so
// checking a new assertion only re-parses the one file it is inserted into.
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

namespace qfv {

struct SessionOptions {
    std::vector<std::string> files;
    std::vector<std::string> defines;     // NAME or NAME=VALUE
    std::vector<std::string> includeDirs;
    std::string top;
};

class Session {
public:
    explicit Session(SessionOptions opts);

    /// Parses all source files. Returns false if a file could not be read.
    bool load(std::string& error);

    /// Elaborates the cached trees, optionally replacing one tree (by index).
    std::unique_ptr<slang::ast::Compilation> compile(
        std::optional<size_t> replaceIndex = {},
        std::shared_ptr<slang::syntax::SyntaxTree> replacement = nullptr);

    /// Parses `text` as a replacement for tree `index` (same file path).
    std::shared_ptr<slang::syntax::SyntaxTree> reparse(size_t index, std::string_view text);

    /// Finds the tree that declares module `name`; returns its index and the
    /// source offset just before that module's `endmodule`.
    std::optional<std::pair<size_t, size_t>> findModuleEnd(std::string_view name) const;

    std::string_view sourceText(size_t index) const;
    slang::BufferID buffer(size_t index) const;

    slang::SourceManager& sourceManager() { return sm; }
    const SessionOptions& options() const { return opts; }
    size_t numTrees() const { return trees.size(); }
    double parseMs() const { return parseMs_; }

private:
    slang::Bag parseOptions() const;

    SessionOptions opts;
    slang::SourceManager sm;
    std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> trees;
    double parseMs_ = 0;
};

double msSince(std::chrono::steady_clock::time_point t0);

} // namespace qfv
