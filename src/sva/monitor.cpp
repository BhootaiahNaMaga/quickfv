// Monitor construction
// ====================
//
// A delay chain  b0 ##[lo1:hi1] b1 ... ##[loN:hiN] bN  is matched by shift
// registers. Let e_i ("entry") mean "element i may start being matched now":
// e_0 is the attempt start, and e_{i+1} = hit_i. Element i matches now if b_i
// holds and e_i happened w cycles ago for some w in [lo_i, hi_i]:
//
//     hit_i = b_i && OR_{w=lo_i..hi_i} delay(e_i, w)
//
// delay(e_i, 0) is e_i itself; delay(e_i, w) for w >= 1 is a register chain.
//
// Antecedent: an attempt starts every cycle and we only need to know whether
// *some* attempt matches now, so all attempts share one set of registers.
//
// Consequent: each attempt must be tracked on its own. An attempt fails in the
// first cycle in which it has not matched and has no live thread left, and a
// shared register set would let a younger attempt keep an older, failing one
// alive. So attempts sit in an age pipeline: stage a holds the delay registers
// of the attempt that started a cycles ago. Attempts live at most L = sum(hi)
// cycles, so L stages suffice. An attempt that matches is retired (weak
// semantics: one match satisfies it).
//
// disable iff (D): when D holds, all attempt state is cleared and the final
// directive's own `disable iff` masks the failure in that cycle. Sampled-value
// helpers ($past etc.) are not cleared; per IEEE 1800 they ignore disable iff.
//
// All state starts at 0, so no attempt exists before the first clock and $past
// before the first clock returns 0.
#include "sva/monitor.h"

#include <sstream>
#include <vector>

namespace qfv::sva {

namespace {

class Emitter {
public:
    Emitter(const AssertionIR& ir, const MonitorOptions& opts) :
        ir(ir), opts(opts), p("qfv_" + sanitize(ir.label)) {}

    std::string run() {
        out << "/* qfv monitor for: " << escapeComment(ir.toString()) << " */\n";
        emitHelpers();
        std::string dis = ir.disableText.empty() ? "1'b0" : "(" + ir.disableText + ")";
        wire(p + "_dis", dis);

        std::string anteMatch = emitAntecedent();
        if (ir.kind == DirectiveKind::Cover) {
            reach(anteMatch, "qfv_cover__" + ir.label, ir.label);
        }
        else {
            std::string fail = emitConsequent(anteMatch);
            directive(ir.kind == DirectiveKind::Assert ? "assert" : "assume", "!" + fail);
            if (opts.vacuityCover && ir.kind == DirectiveKind::Assert && !ir.ante.elems.empty())
                reach(anteMatch, "qfv_trigger__" + ir.label, ir.label + "_qfv_trigger");
        }
        emitRegisters();
        return out.str();
    }

private:
    static std::string sanitize(const std::string& s) {
        std::string r;
        for (char c : s)
            r += (isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
        return r;
    }

    static std::string escapeComment(std::string s) {
        for (size_t i; (i = s.find("*/")) != std::string::npos;)
            s.replace(i, 2, "* /");
        for (auto& c : s)
            if (c == '\n')
                c = ' ';
        return s;
    }

    void wire(const std::string& name, const std::string& expr) {
        out << "wire " << name << " = " << expr << ";\n";
    }

    /// Declares a 1-bit register that starts at 0 and gets `next` on each clock
    /// (0 while disabled, unless `survivesDisable`).
    void reg(const std::string& name, const std::string& next, bool survivesDisable = false,
             uint32_t width = 1) {
        std::string range = width > 1 ? "[" + std::to_string(width - 1) + ":0] " : "";
        out << "reg " << range << name << " = " << width << "'d0;\n";
        (survivesDisable ? keepRegs : clearRegs).push_back({name, next, width});
    }

    void emitHelpers() {
        for (auto& h : ir.helpers) {
            std::string range = h.width > 1 ? "[" + std::to_string(h.width - 1) + ":0] " : "";
            out << "wire " << range << h.name << "_0 = (" << h.exprText << ");\n";
            for (uint32_t k = 1; k <= h.depth; k++)
                reg(h.name + "_" + std::to_string(k), h.name + "_" + std::to_string(k - 1),
                    /*survivesDisable=*/true, h.width);
        }
    }

    std::string leafWire(const std::string& tag, size_t i, const Leaf& leaf) {
        std::string name = p + "_" + tag + "b" + std::to_string(i);
        wire(name, "((" + leaf.text + ") ? 1'b1 : 1'b0)");
        return name;
    }

    /// Matches a chain for a single, shared attempt stream. Returns the match wire.
    std::string emitAntecedent() {
        if (ir.ante.elems.empty())
            return "1'b1";
        std::string entry = "1'b1"; // an attempt starts every cycle
        for (size_t i = 0; i < ir.ante.elems.size(); i++) {
            auto& el = ir.ante.elems[i];
            std::string b = leafWire("a", i, el.leaf);
            std::string e = p + "_ae" + std::to_string(i);
            wire(e, entry);
            std::vector<std::string> taps;
            if (el.lo == 0)
                taps.push_back(e);
            for (uint32_t w = 1; w <= el.hi; w++) {
                std::string d = p + "_ad" + std::to_string(i) + "_" + std::to_string(w);
                reg(d, w == 1 ? e : p + "_ad" + std::to_string(i) + "_" + std::to_string(w - 1));
                if (w >= el.lo)
                    taps.push_back(d);
            }
            std::string hit = p + "_ah" + std::to_string(i);
            wire(hit, b + " && (" + join(taps, " || ") + ")");
            entry = hit;
        }
        wire(p + "_ante", entry);
        return p + "_ante";
    }

    /// Tracks each consequent attempt separately (age pipeline). Returns the fail wire.
    std::string emitConsequent(const std::string& start) {
        auto& elems = ir.cons.elems;
        std::vector<std::string> b;
        for (size_t i = 0; i < elems.size(); i++)
            b.push_back(leafWire("c", i, elems[i].leaf));

        uint32_t L = ir.cons.maxLength();
        std::vector<std::string> fails;
        // Stage a: registers  <p>_g<a>_<i>_<w>  = delay(e_i, w) of the attempt of age a.
        auto stateName = [&](uint32_t age, size_t i, uint32_t w) {
            return p + "_g" + std::to_string(age) + "_" + std::to_string(i) + "_" +
                   std::to_string(w);
        };
        for (uint32_t age = 0; age <= L; age++) {
            std::string sa = p + "_g" + std::to_string(age);
            std::string entry = age == 0 ? start : "1'b0";
            std::vector<std::string> nextBits, nextNames;
            for (size_t i = 0; i < elems.size(); i++) {
                auto& el = elems[i];
                std::string e = sa + "_e" + std::to_string(i);
                wire(e, entry);
                std::vector<std::string> taps;
                if (el.lo == 0)
                    taps.push_back(e);
                for (uint32_t w = 1; w <= el.hi; w++) {
                    std::string cur = age == 0 ? "1'b0" : stateName(age, i, w);
                    if (w >= el.lo)
                        taps.push_back(cur);
                    // next value of delay(e_i, w+1) for this attempt, at age+1
                    if (w + 1 <= el.hi) {
                        nextBits.push_back(cur);
                        nextNames.push_back(stateName(age + 1, i, w + 1));
                    }
                }
                if (el.hi >= 1) {
                    nextBits.push_back(e);
                    nextNames.push_back(stateName(age + 1, i, 1));
                }
                std::string hit = sa + "_h" + std::to_string(i);
                wire(hit, b[i] + " && (" + (taps.empty() ? "1'b0" : join(taps, " || ")) + ")");
                entry = hit;
            }
            std::string acc = entry;
            // Live threads that carry into the next cycle.
            std::string live = sa + "_live";
            wire(live, nextBits.empty() ? "1'b0" : join(nextBits, " || "));
            std::string active = sa + "_active";
            if (age == 0) {
                wire(active, start);
            }
            else {
                std::vector<std::string> cur;
                for (size_t i = 0; i < elems.size(); i++)
                    for (uint32_t w = 1; w <= elems[i].hi; w++)
                        cur.push_back(stateName(age, i, w));
                wire(active, cur.empty() ? "1'b0" : join(cur, " || "));
            }
            std::string fail = sa + "_fail";
            wire(fail, active + " && !(" + acc + ") && !" + live);
            fails.push_back(fail);
            // Register the next stage; an attempt that matched is retired.
            if (age < L) {
                for (size_t k = 0; k < nextBits.size(); k++)
                    reg(nextNames[k], "!(" + acc + ") && " + nextBits[k]);
            }
        }
        wire(p + "_fail", join(fails, " || "));
        return p + "_fail";
    }

    void directive(const std::string& kind, const std::string& expr, std::string label = "") {
        if (label.empty())
            label = ir.label;
        out << label << ": " << kind << " property (@(" << ir.clockEvent << ") disable iff ("
            << p << "_dis) " << expr << ");\n";
    }

    /// A reachability goal: a real cover, or (for the engine) a negated assert.
    void reach(const std::string& goal, const std::string& badLabel, const std::string& coverLabel) {
        if (opts.reachAsBad)
            directive("assert", "!" + goal, badLabel);
        else
            directive("cover", goal, coverLabel);
    }

    void emitRegisters() {
        if (clearRegs.empty() && keepRegs.empty())
            return;
        out << "always @(" << ir.clockEvent << ") begin\n";
        for (auto& r : keepRegs)
            out << "  " << r.name << " <= " << r.next << ";\n";
        if (!clearRegs.empty()) {
            out << "  if (" << p << "_dis) begin\n";
            for (auto& r : clearRegs)
                out << "    " << r.name << " <= " << r.width << "'d0;\n";
            out << "  end else begin\n";
            for (auto& r : clearRegs)
                out << "    " << r.name << " <= " << r.next << ";\n";
            out << "  end\n";
        }
        out << "end\n";
    }

    static std::string join(const std::vector<std::string>& v, const std::string& sep) {
        std::string r;
        for (size_t i = 0; i < v.size(); i++)
            r += (i ? sep : "") + v[i];
        return r;
    }

    struct Reg {
        std::string name, next;
        uint32_t width;
    };

    const AssertionIR& ir;
    const MonitorOptions& opts;
    std::string p;
    std::ostringstream out;
    std::vector<Reg> clearRegs, keepRegs;
};

} // namespace

std::string emitMonitor(const AssertionIR& ir, const MonitorOptions& opts) {
    return Emitter(ir, opts).run();
}

} // namespace qfv::sva
