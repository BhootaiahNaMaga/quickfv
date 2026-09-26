#include "engine/trace.h"

#include <algorithm>
#include <map>

#include "engine/sim.h"
#include "json.h"

namespace qfv {

namespace {

std::string bitsText(const std::vector<int8_t>& bits) {
    std::string s;
    for (size_t i = bits.size(); i-- > 0;)
        s += bits[i] > 0 ? '1' : '0'; // don't-care bits become 0
    return s;
}

/// Latch values per frame, by simulating the trace on the AIG.
std::vector<std::vector<std::vector<int8_t>>> latchValues(const TransitionSystem& ts,
                                                          const Cex& cex) {
    SimTrace t{cex.initLatches, cex.inputs};
    bool held = true;
    std::vector<std::vector<std::vector<int8_t>>> values;
    simulate(ts, t, held, &values);
    return values;
}

} // namespace

bool xDependent(const TransitionSystem& ts, size_t prop, const Cex& cex) {
    bool any = false;
    for (auto& in : ts.inputs)
        any = any || in.synthetic;
    if (!any)
        return false;
    SimTrace t{cex.initLatches, cex.inputs};
    for (auto& frame : t.inputs)
        for (size_t i = 0; i < ts.inputs.size(); i++)
            if (ts.inputs[i].synthetic)
                std::fill(frame[i].begin(), frame[i].end(), 0);
    bool held = true;
    auto bads = simulate(ts, t, held);
    if (bads.empty())
        return true;
    auto& last = bads.back();
    return std::find(last.begin(), last.end(), prop) == last.end() || !held;
}

bool isRtlLatch(const TransitionSystem::Latch& l) {
    return !l.name.empty() && l.name.find('$') == std::string::npos &&
           l.name.find("qfv_") == std::string::npos;
}

std::string btor2Witness(const TransitionSystem& ts, size_t prop, const Cex& cex) {
    std::vector<int32_t> freeOfLatch(ts.latches.size(), -1);
    for (size_t i = 0; i < ts.inputs.size(); i++)
        if (ts.inputs[i].freeNextOf >= 0)
            freeOfLatch[size_t(ts.inputs[i].freeNextOf)] = int32_t(i);
    std::string w = "sat\nb" + std::to_string(prop) + "\n";
    for (size_t f = 0; f < cex.inputs.size(); f++) {
        w += "#" + std::to_string(f) + "\n";
        for (size_t i = 0; i < ts.latches.size(); i++) {
            if (f == 0) {
                bool hasInit = true;
                for (auto b : ts.latches[i].init)
                    hasInit = hasInit && b >= 0;
                if (!hasInit)
                    w += std::to_string(i) + " " + bitsText(cex.initLatches[i]) + "\n";
            }
            else if (freeOfLatch[i] >= 0) { // unconstrained state: its value in frame f
                w += std::to_string(i) + " " + bitsText(cex.inputs[f - 1][size_t(freeOfLatch[i])]) + "\n";
            }
        }
        w += "@" + std::to_string(f) + "\n";
        size_t k = 0; // index among real BTOR2 inputs
        for (size_t i = 0; i < ts.inputs.size(); i++) {
            if (ts.inputs[i].freeNextOf >= 0)
                continue;
            w += std::to_string(k++) + " " + bitsText(cex.inputs[f][i]) + "\n";
        }
    }
    return w + ".\n";
}

std::string jsonTrace(const TransitionSystem& ts, size_t prop, const Cex& cex,
                      const std::string& clock) {
    auto values = latchValues(ts, cex);
    JsonWriter w;
    w.beginObject()
        .field("property", ts.props[prop].name)
        .field("length", cex.depth + 1)
        .field("note", "frame k = values during clock cycle k; registers show their value "
                       "at the start of the cycle");
    w.key("frames").beginArray();
    for (size_t f = 0; f < cex.inputs.size(); f++) {
        w.beginObject().field("cycle", uint64_t(f));
        w.key("inputs").beginObject();
        for (size_t i = 0; i < ts.inputs.size(); i++)
            if (!ts.inputs[i].name.empty() && ts.inputs[i].name != clock && !ts.inputs[i].synthetic)
                w.field(ts.inputs[i].name, bitsText(cex.inputs[f][i]));
        w.endObject();
        w.key("registers").beginObject();
        for (size_t l = 0; l < ts.latches.size(); l++)
            if (isRtlLatch(ts.latches[l]))
                w.field(ts.latches[l].name, bitsText(values[f][l]));
        w.endObject().endObject();
    }
    w.endArray().endObject();
    return w.str();
}

std::string vcdTrace(const TransitionSystem& ts, const Cex& cex, const std::string& clock) {
    auto values = latchValues(ts, cex);
    struct Sig {
        std::string id, name;
        size_t width;
        bool isInput;
        size_t index;
    };
    std::vector<Sig> sigs;
    auto mkId = [](size_t n) {
        std::string id;
        do {
            id += char('!' + n % 94);
            n /= 94;
        } while (n);
        return id;
    };
    for (size_t i = 0; i < ts.inputs.size(); i++)
        if (!ts.inputs[i].name.empty() && ts.inputs[i].name != clock)
            sigs.push_back({mkId(sigs.size() + 1), ts.inputs[i].name, ts.inputs[i].bits.size(), true, i});
    for (size_t l = 0; l < ts.latches.size(); l++)
        if (isRtlLatch(ts.latches[l]))
            sigs.push_back({mkId(sigs.size() + 1), ts.latches[l].name, ts.latches[l].cur.size(), false, l});

    // Group by hierarchical scope (dotted names).
    std::map<std::string, std::vector<const Sig*>> scopes;
    for (auto& s : sigs) {
        auto dot = s.name.rfind('.');
        scopes[dot == std::string::npos ? "" : s.name.substr(0, dot)].push_back(&s);
    }
    std::string v = "$timescale 1ns $end\n$scope module top $end\n";
    v += "$var wire 1 ! " + clock + " $end\n";
    for (auto& [scope, list] : scopes) {
        int depth = 0;
        if (!scope.empty()) {
            size_t start = 0;
            while (true) {
                auto dot = scope.find('.', start);
                v += "$scope module " + scope.substr(start, dot - start) + " $end\n";
                depth++;
                if (dot == std::string::npos)
                    break;
                start = dot + 1;
            }
        }
        for (auto s : list) {
            auto leaf = s->name.substr(s->name.rfind('.') == std::string::npos ? 0 : s->name.rfind('.') + 1);
            for (auto& c : leaf)
                if (c == '[' || c == ']')
                    c = '_';
            v += "$var wire " + std::to_string(s->width) + " " + s->id + " " + leaf + " $end\n";
        }
        while (depth--)
            v += "$upscope $end\n";
    }
    v += "$upscope $end\n$enddefinitions $end\n";
    for (size_t f = 0; f < cex.inputs.size(); f++) {
        v += "#" + std::to_string(f * 10) + "\n0!\n";
        for (auto& s : sigs) {
            auto& bits = s.isInput ? cex.inputs[f][s.index] : values[f][s.index];
            v += s.width == 1 ? bitsText(bits) + s.id + "\n" : "b" + bitsText(bits) + " " + s.id + "\n";
        }
        v += "#" + std::to_string(f * 10 + 5) + "\n1!\n";
    }
    v += "#" + std::to_string(cex.inputs.size() * 10) + "\n";
    return v;
}

std::string replayTestbench(const TransitionSystem& ts, const Cex& cex, const ReplayOptions& o) {
    std::string tb = "// Generated by qfv: replays a counterexample on the original RTL.\n"
                     "module qfv_replay;\n  reg " + o.clock + " = 0;\n";
    std::string ports = "." + o.clock + "(" + o.clock + ")";
    for (auto& in : ts.inputs) {
        if (in.name.empty() || in.name == o.clock || in.synthetic)
            continue;
        tb += "  reg [" + std::to_string(in.bits.size() - 1) + ":0] " + in.name + " = 0;\n";
        ports += ", ." + in.name + "(" + in.name + ")";
    }
    tb += "  integer qfv_cycle = 0;\n";
    tb += "  " + o.top + " dut (" + ports + ");\n" + o.extraChecks;
    tb += "  initial begin\n";
    for (size_t l = 0; l < ts.latches.size(); l++) {
        auto& latch = ts.latches[l];
        if (!isRtlLatch(latch))
            continue;
        bool free = false;
        for (auto b : latch.init)
            free = free || b < 0;
        if (free) // registers with a reset value start there anyway
            tb += "    dut." + latch.name + " = " + std::to_string(latch.cur.size()) + "'b" +
                  bitsText(cex.initLatches[l]) + ";\n";
    }
    for (size_t f = 0; f < cex.inputs.size(); f++) {
        tb += "    qfv_cycle = " + std::to_string(f) + ";";
        for (size_t i = 0; i < ts.inputs.size(); i++) {
            auto& in = ts.inputs[i];
            if (in.name.empty() || in.name == o.clock || in.synthetic)
                continue;
            tb += " " + in.name + " = " + std::to_string(in.bits.size()) + "'b" +
                  bitsText(cex.inputs[f][i]) + ";";
        }
        tb += "\n    #5 " + o.clock + " = 1; #5 " + o.clock + " = 0;\n";
    }
    tb += "    $display(\"QFV_REPLAY_END\");\n    $finish;\n  end\nendmodule\n";
    return tb;
}

} // namespace qfv
