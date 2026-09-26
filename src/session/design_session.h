// A persistent design session (SPEC section 6.2, rule 1: no reload per assertion).
//
// load():  parse the sources, strip every assertion, run Yosys ONCE (all named
//          wires kept and exposed), and bit-blast the design. Assertions from
//          the files and the setup file then enter the session like any other.
// add():   bind one assertion with slang (only its file is re-parsed), lower it,
//          and emit its monitor straight into the loaded AIG. No Yosys.
// check(): hunt on the chosen assertions with the session's long-lived
//          unrollers (encodings and learned clauses carry over).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/trace.h"
#include "model/ts.h"
#include "session.h"
#include "session/setup_file.h"

namespace qfv {

class Unroller;

class DesignSession {
public:
    using json = nlohmann::json;
    using Emit = std::function<void(const json&)>;

    explicit DesignSession(std::string workDir);
    ~DesignSession();

    json load(const SetupConfig& cfg);
    /// Adds assertions written as SV text (`label: assert property (...);`)
    /// into `module` (default: top). Returns the new entries and diagnostics.
    json add(const std::string& module, const std::string& text, const std::string& source = "session");
    /// Checks the given entries (all asserts and covers if empty).
    /// `certify`: every UNSAT answer is certified (LIDRUP + lidrup-check).
    json check(const std::vector<std::string>& ids, double budgetSeconds, double simSeconds,
               const Emit& emit, bool certify = false);
    json remove(const std::string& id);
    json list() const;
    json trace(const std::string& id, const std::vector<std::string>& signals, int fromCycle,
               int toCycle) const;
    std::string exportJasperGold() const;

    bool loaded() const { return ts != nullptr; }

private:
    struct Entry {
        std::string id, label, kind, module, source, text, topScopeText;
        std::string instancePath; // set when the directive has several instances
        std::string status = "ready"; // ready | unsupported | error
        std::string reason;
        size_t assertProp = SIZE_MAX, triggerProp = SIZE_MAX;
        size_t assumeBmc = SIZE_MAX, assumeStep = SIZE_MAX;
        Lit assumeLit = kTrue;
        bool active = true;
        json last; // last verdict and trace paths
    };

    json loadNew(const SetupConfig& cfg);
    std::string uniqueId(const std::string& label) const;
    json addFromCompilation(slang::ast::Compilation& comp, size_t probeBuffer, size_t lo, size_t hi,
                            const std::string& module, const std::string& source);
    void markStale();
    void rebuildUnrollers();
    static constexpr size_t kMaxSessionClauses = 2'000'000;
    size_t rebuilds = 0;
    std::vector<Lit> activeAssumptions() const;

    std::string work;
    SetupConfig cfg;
    std::unique_ptr<Session> src;
    std::unique_ptr<TransitionSystem> ts;
    std::unique_ptr<Unroller> bmc, step;
    std::vector<Entry> entries;
    std::vector<std::string> removedFileAssumptions; // still in the sources JasperGold analyzes
    json loadInfo;
};

} // namespace qfv
