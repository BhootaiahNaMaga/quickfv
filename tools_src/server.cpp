// Session front ends over stdio (SPEC section 7):
//   qfv serve  - newline-delimited JSON requests/responses with streamed events;
//                used by the Tcl package (a pipe to a long-lived process).
//   qfv mcp    - Model Context Protocol server (JSON-RPC 2.0 over stdio) for agents.
#include "server.h"

#include <fstream>
#include <iostream>
#include <regex>

#include "session/design_session.h"
#include "session/setup_file.h"

namespace qfv {

using json = nlohmann::json;

namespace {

/// Accepts either a full directive ("lbl: assert property (...);") or a bare
/// property ("req |=> ack"), which is wrapped with the design clock.
std::string normalizeSva(const std::string& text, const std::string& kind, const std::string& clock,
                         const std::string& name, const std::string& disable) {
    static const std::regex directive(R"(\b(assert|assume|cover)\s+property\b)");
    if (std::regex_search(text, directive))
        return text;
    std::string label = name.empty() ? kind + "_" + std::to_string(std::hash<std::string>{}(text) % 100000) : name;
    std::string dis = disable.empty() ? "" : "disable iff (" + disable + ") ";
    return label + ": " + kind + " property (@(posedge " + clock + ") " + dis + text + ");";
}

class Handler {
public:
    explicit Handler(std::string work) : session(std::move(work)) {}

    json load(const json& p) {
        SetupConfig cfg;
        std::string err;
        if (p.contains("setup_file")) {
            if (!parseSetupFile(p["setup_file"], cfg, err))
                return {{"ok", false}, {"error", err}};
        }
        else {
            for (auto& f : p.value("files", json::array()))
                cfg.files.push_back(f);
            for (auto& d : p.value("defines", json::array()))
                cfg.defines.push_back(d);
            cfg.top = p.value("top", "");
            cfg.clock = p.value("clock", "clk");
            cfg.resetExpr = p.value("reset", "");
            if (cfg.files.empty() || cfg.top.empty())
                return {{"ok", false}, {"error", "load needs setup_file, or files + top"}};
        }
        clock = cfg.clock;
        return session.load(cfg);
    }

    json add(const json& p, const std::string& kind) {
        auto text = normalizeSva(p.value("sva", p.value("property", "")), kind, clock, p.value("name", ""),
                                 p.value("disable_iff", ""));
        return session.add(p.value("module", ""), text);
    }

    json check(const json& p, const DesignSession::Emit& emit) {
        std::vector<std::string> ids;
        for (auto& i : p.value("ids", json::array()))
            ids.push_back(i);
        return session.check(ids, p.value("budget_s", 30.0), p.value("sim_s", 0.25), emit);
    }

    /// Agent loop in one call: lint + add + check the new assertion(s).
    json checkAssertion(const json& p, const DesignSession::Emit& emit) {
        auto added = add(p, "assert");
        json out = {{"lint", added}};
        std::vector<std::string> ids;
        for (auto& a : added.value("added", json::array()))
            if (a["status"] == "ready")
                ids.push_back(a["id"]);
        if (ids.empty()) {
            out["verdicts"] = json::object();
            return out;
        }
        json events = json::array();
        auto res = session.check(ids, p.value("budget_s", 30.0), p.value("sim_s", 0.25), [&](const json& e) {
            events.push_back(e);
            emit(e);
        });
        out["check"] = res;
        out["results"] = events;
        return out;
    }

    json dispatch(const std::string& method, const json& p, const DesignSession::Emit& emit) {
        if (method == "load" || method == "load_design")
            return load(p);
        if (!session.loaded())
            return {{"ok", false}, {"error", "no design loaded: call load first"}};
        if (method == "add")
            return add(p, "assert");
        if (method == "add_assumption" || method == "assume")
            return add(p, "assume");
        if (method == "add_cover" || method == "cover")
            return add(p, "cover");
        if (method == "check" || method == "check_all")
            return check(p, emit);
        if (method == "check_assertion")
            return checkAssertion(p, emit);
        if (method == "remove")
            return session.remove(p.value("id", ""));
        if (method == "list" || method == "list_assertions")
            return session.list();
        if (method == "trace" || method == "get_trace") {
            std::vector<std::string> sigs;
            for (auto& s : p.value("signals", json::array()))
                sigs.push_back(s);
            return session.trace(p.value("id", ""), sigs, p.value("from_cycle", 0), p.value("to_cycle", -1));
        }
        if (method == "export_jg" || method == "export_jaspergold") {
            auto tcl = session.exportJasperGold();
            if (p.contains("path")) {
                std::ofstream(p["path"].get<std::string>()) << tcl;
                return {{"ok", true}, {"path", p["path"]}};
            }
            return {{"ok", true}, {"tcl", tcl}};
        }
        return {{"ok", false}, {"error", "unknown method '" + method + "'"}};
    }

private:
    DesignSession session;
    std::string clock = "clk";
};

json toolSchema(const std::string& name, const std::string& desc, json props, json required) {
    return {{"name", name},
            {"description", desc},
            {"inputSchema", {{"type", "object"}, {"properties", props}, {"required", required}}}};
}

json mcpTools() {
    json sva = {{"type", "string"},
                {"description", "SVA: a full directive 'label: assert property (@(posedge clk) ...);' or a bare "
                                "property like 'req |=> ##[1:3] ack' (clocked on the design clock)"}};
    json budget = {{"type", "number"}, {"description", "time budget in seconds (default 30)"}};
    return json::array({
        toolSchema("load_design",
                   "Load an RTL design once (Yosys runs only here). Give a JasperGold-style setup file "
                   "(analyze/elaborate/clock/reset/assert/assume) or files+top+clock+reset. Assertions in "
                   "the files and the setup file are registered.",
                   {{"setup_file", {{"type", "string"}}},
                    {"files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"top", {{"type", "string"}}},
                    {"clock", {{"type", "string"}}},
                    {"reset", {{"type", "string"}, {"description", "reset expression, e.g. !rst_n"}}}},
                   json::array()),
        toolSchema("check_assertion",
                   "Lint, add and check one assertion in the loaded design in a single call. Returns T0 "
                   "diagnostics, the vacuity result (is the trigger reachable?) and CEX / PASS_BOUNDED "
                   "within the budget. PASS_BOUNDED is NOT a proof. No design reload.",
                   {{"sva", sva},
                    {"module", {{"type", "string"}, {"description", "module scope (default: top)"}}},
                    {"name", {{"type", "string"}}},
                    {"disable_iff", {{"type", "string"}, {"description", "for bare properties, e.g. !rst_n"}}},
                    {"budget_s", budget}},
                   json::array({"sva"})),
        toolSchema("add_assumption",
                   "Add an environment assumption (e.g. to rule out an illegal input sequence behind a "
                   "spurious CEX). Earlier CEX results are marked stale; re-check afterwards.",
                   {{"sva", sva}, {"module", {{"type", "string"}}}, {"name", {{"type", "string"}}}},
                   json::array({"sva"})),
        toolSchema("check_all", "Re-check assertions (all, or the given ids) with the current assumptions.",
                   {{"ids", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"budget_s", budget}},
                   json::array()),
        toolSchema("get_trace",
                   "Counterexample of an assertion, optionally filtered to signals whose name contains any "
                   "of `signals`, and to a cycle window.",
                   {{"id", {{"type", "string"}}},
                    {"signals", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"from_cycle", {{"type", "integer"}}},
                    {"to_cycle", {{"type", "integer"}}}},
                   json::array({"id"})),
        toolSchema("list_assertions", "All assertions/assumptions in the session with their last verdicts.",
                   json::object(), json::array()),
        toolSchema("remove", "Remove an assertion or assumption by id.", {{"id", {{"type", "string"}}}},
                   json::array({"id"})),
        toolSchema("export_jaspergold",
                   "JasperGold Tcl handoff: sources, clock/reset, session assertions and assumptions in top "
                   "scope, QuickFV verdicts as comments, and `prove` on what is still open.",
                   {{"path", {{"type", "string"}, {"description", "write here instead of returning the text"}}}},
                   json::array()),
    });
}

} // namespace

int runServe(const std::string& workDir) {
    Handler h(workDir);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        json req;
        try {
            req = json::parse(line);
        }
        catch (const std::exception& e) {
            std::cout << json{{"error", std::string("bad JSON: ") + e.what()}}.dump() << std::endl;
            continue;
        }
        auto id = req.value("id", json());
        auto emit = [&](const json& ev) { std::cout << json{{"id", id}, {"event", ev}}.dump() << std::endl; };
        json result;
        try {
            result = h.dispatch(req.value("method", ""), req.value("params", json::object()), emit);
        }
        catch (const std::exception& e) {
            result = {{"ok", false}, {"error", e.what()}};
        }
        std::cout << json{{"id", id}, {"result", result}}.dump() << std::endl;
    }
    return 0;
}

int runMcp(const std::string& workDir) {
    Handler h(workDir);
    std::string line;
    auto send = [](const json& msg) { std::cout << msg.dump() << "\n" << std::flush; };
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        json req;
        try {
            req = json::parse(line);
        }
        catch (...) {
            send({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}});
            continue;
        }
        if (!req.contains("id"))
            continue; // notification (e.g. notifications/initialized)
        auto id = req["id"];
        std::string method = req.value("method", "");
        json params = req.value("params", json::object());
        if (method == "initialize") {
            send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result",
                   {{"protocolVersion", params.value("protocolVersion", "2025-06-18")},
                    {"capabilities", {{"tools", json::object()}}},
                    {"serverInfo", {{"name", "quickfv"}, {"version", "0.4"}}},
                    {"instructions",
                     "QuickFV: fast formal pre-checks before JasperGold. load_design once, then "
                     "check_assertion per assertion (T0 lint, vacuity, bug hunt). PASS_BOUNDED means no CEX "
                     "within the budget, not a proof; VACUOUS means the trigger can never fire; every CEX "
                     "is replay-certified by btorsim."}}}});
        }
        else if (method == "ping") {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}});
        }
        else if (method == "tools/list") {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", mcpTools()}}}});
        }
        else if (method == "tools/call") {
            json result;
            bool isError = false;
            try {
                result = h.dispatch(params.value("name", ""), params.value("arguments", json::object()),
                                    [](const json&) {});
                isError = result.contains("ok") && result["ok"] == false;
            }
            catch (const std::exception& e) {
                result = {{"ok", false}, {"error", e.what()}};
                isError = true;
            }
            send({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"result", {{"content", json::array({{{"type", "text"}, {"text", result.dump(1)}}})},
                              {"isError", isError}}}});
        }
        else {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "method not found: " + method}}}});
        }
    }
    return 0;
}

} // namespace qfv
