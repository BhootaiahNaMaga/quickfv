// BTOR2 → bit-level transition system (bit-blasting).
//
// Every BTOR2 node becomes a vector of AIG literals, least significant bit
// first. A negative argument id means bitwise NOT of that node (BTOR2 syntax).
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "model/ts.h"

extern "C" {
#include "btor2parser/btor2parser.h"
}

namespace qfv {

namespace {

using Bits = std::vector<Lit>;

struct LoadError {
    std::string msg;
};

[[noreturn]] void fail(const Btor2Line* l, const std::string& msg) {
    throw LoadError{"line " + std::to_string(l ? l->lineno : 0) + ": " + msg};
}

/// Decimal or hex digit string → `width` bits, LSB first (two's complement if negative).
Bits parseNumber(std::string s, int base, uint32_t width) {
    bool negative = !s.empty() && s[0] == '-';
    if (negative)
        s.erase(0, 1);
    std::vector<int> digits;
    for (char c : s)
        digits.push_back(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    std::vector<bool> bits;
    for (uint32_t i = 0; i < width; i++) {
        // Divide the digit string by 2; the remainder is the next bit.
        int rem = 0;
        for (auto& d : digits) {
            int cur = rem * base + d;
            d = cur / 2;
            rem = cur % 2;
        }
        bits.push_back(rem);
    }
    if (negative) {
        bool carry = true; // invert and add one
        for (size_t i = 0; i < bits.size(); i++) {
            bool v = !bits[i];
            bits[i] = v != carry;
            carry = v && carry;
        }
    }
    Bits out;
    for (bool v : bits)
        out.push_back(v ? kTrue : kFalse);
    return out;
}

class Blaster {
public:
    Blaster(Btor2Parser* p, TransitionSystem& ts) : p(p), ts(ts), aig(ts.aig) {}

    void run() {
        auto it = btor2parser_iter_init(p);
        std::vector<Btor2Line*> lines;
        while (auto l = btor2parser_iter_next(&it))
            lines.push_back(l);

        // Pass 1: inputs and states become AIG inputs.
        for (auto l : lines) {
            if (l->tag == BTOR2_TAG_input || l->tag == BTOR2_TAG_state) {
                if (l->sort.tag != BTOR2_TAG_SORT_bitvec)
                    fail(l, "array-sorted " + std::string(l->name) +
                                " (memories must be mapped to registers; array support is v1.1)");
                Bits bits;
                for (uint32_t i = 0; i < l->sort.bitvec.width; i++)
                    bits.push_back(aig.newInput());
                nodes[l->id] = bits;
                std::string name = l->symbol ? l->symbol : "";
                if (!name.empty())
                    ts.signals[name] = bits;
                if (l->tag == BTOR2_TAG_input) {
                    ts.inputs.push_back({name, l->id, bits, name.empty() || name.find('$') != std::string::npos});
                }
                else {
                    TransitionSystem::Latch latch;
                    latch.name = name;
                    latch.btorId = l->id;
                    latch.cur = bits;
                    latch.next = bits; // no next: keeps its value
                    latch.init.assign(bits.size(), -1);
                    latch.synthetic = name.empty() || name.find('$') != std::string::npos ||
                                      name.find("qfv_") != std::string::npos;
                    latchIndex[l->id] = ts.latches.size();
                    ts.latches.push_back(std::move(latch));
                }
            }
        }
        // Pass 2: everything else, in file order (BTOR2 is topologically ordered
        // except that next/init refer back to states).
        for (auto l : lines) {
            switch (l->tag) {
                case BTOR2_TAG_sort:
                case BTOR2_TAG_input:
                case BTOR2_TAG_state: break;
                case BTOR2_TAG_output:
                    if (l->symbol) {
                        ts.signals[l->symbol] = arg(l, 0);
                        // With every wire exposed (session models), Yosys names the
                        // output, not the state it reads: name the state from it.
                        auto li = latchIndex.find(l->args[0]);
                        if (l->args[0] > 0 && li != latchIndex.end()) {
                            auto& latch = ts.latches[li->second];
                            latch.aliases.push_back(l->symbol);
                            if (latch.name.empty() || latch.synthetic) {
                                latch.name = l->symbol;
                                latch.synthetic = latch.name.find('$') != std::string::npos ||
                                                  latch.name.find("qfv_") != std::string::npos;
                            }
                        }
                    }
                    break;
                case BTOR2_TAG_init: {
                    auto& latch = ts.latches.at(latchIndex.at(l->args[0]));
                    Bits v = arg(l, 1);
                    for (size_t i = 0; i < v.size(); i++) {
                        if (v[i] != kTrue && v[i] != kFalse)
                            fail(l, "non-constant init of " + latch.name);
                        latch.init[i] = v[i] == kTrue ? 1 : 0;
                    }
                    break;
                }
                case BTOR2_TAG_next:
                    ts.latches.at(latchIndex.at(l->args[0])).next = arg(l, 1);
                    hasNext.insert(l->args[0]);
                    break;
                case BTOR2_TAG_bad: {
                    TransitionSystem::Property prop;
                    prop.name = l->symbol ? l->symbol : "bad_" + std::to_string(l->id);
                    prop.btorId = l->id;
                    prop.bad = arg(l, 0).at(0);
                    // Leaf label, then the qfv reachability prefixes (see sva/monitor.h).
                    std::string leaf = prop.name.substr(prop.name.rfind('.') == std::string::npos
                                                            ? 0
                                                            : prop.name.rfind('.') + 1);
                    for (auto [prefix, trigger] : {std::pair{"qfv_trigger__", true},
                                                   std::pair{"qfv_cover__", false}}) {
                        if (leaf.rfind(prefix, 0) == 0) {
                            prop.kind = TransitionSystem::Property::Kind::Reach;
                            prop.isTrigger = trigger;
                            leaf = leaf.substr(std::string(prefix).size());
                        }
                    }
                    prop.label = leaf;
                    ts.props.push_back(prop);
                    break;
                }
                case BTOR2_TAG_constraint: ts.constraints.push_back(arg(l, 0).at(0)); break;
                case BTOR2_TAG_fair:
                case BTOR2_TAG_justice:
                    fail(l, "liveness (justice/fairness) is not supported in v1");
                default: nodes[l->id] = blast(l);
            }
        }
        // BTOR2: a state without a next function is unconstrained in every frame
        // (its init, if any, applies to frame 0 only), exactly like Yosys's
        // $anyseq. Model it with a fresh free input as its next value.
        for (auto& [id, index] : latchIndex) {
            if (hasNext.count(id))
                continue;
            auto& latch = ts.latches[index];
            TransitionSystem::Input in;
            in.name = "$free_next$" + (latch.name.empty() ? std::to_string(id) : latch.name);
            in.btorId = 0;
            in.synthetic = true;
            in.freeNextOf = int32_t(index);
            for (size_t b = 0; b < latch.cur.size(); b++)
                in.bits.push_back(aig.newInput());
            latch.next = in.bits;
            ts.inputs.push_back(in);
        }
    }

private:
    Bits arg(const Btor2Line* l, int i) {
        int64_t id = l->args[i];
        auto it = nodes.find(id < 0 ? -id : id);
        if (it == nodes.end())
            fail(l, "reference to undefined node " + std::to_string(id));
        Bits b = it->second;
        if (id < 0)
            for (auto& x : b)
                x = neg(x);
        return b;
    }

    uint32_t width(const Btor2Line* l) {
        if (l->sort.tag != BTOR2_TAG_SORT_bitvec)
            fail(l, "array operation '" + std::string(l->name) + "' (array support is v1.1)");
        return l->sort.bitvec.width;
    }

    Bits blast(const Btor2Line* l) {
        uint32_t w = width(l);
        switch (l->tag) {
            case BTOR2_TAG_const: {
                std::string s = l->constant;
                Bits b;
                for (size_t i = s.size(); i-- > 0;)
                    b.push_back(s[i] == '1' ? kTrue : kFalse);
                return b;
            }
            case BTOR2_TAG_constd: return parseNumber(l->constant, 10, w);
            case BTOR2_TAG_consth: return parseNumber(l->constant, 16, w);
            case BTOR2_TAG_zero: return Bits(w, kFalse);
            case BTOR2_TAG_ones: return Bits(w, kTrue);
            case BTOR2_TAG_one: {
                Bits b(w, kFalse);
                b[0] = kTrue;
                return b;
            }
            case BTOR2_TAG_not: return bitwise1(arg(l, 0), [](Lit a) { return neg(a); });
            case BTOR2_TAG_and: return bitwise2(l, [&](Lit a, Lit b) { return aig.mkAnd(a, b); });
            case BTOR2_TAG_nand: return bitwise2(l, [&](Lit a, Lit b) { return neg(aig.mkAnd(a, b)); });
            case BTOR2_TAG_or: return bitwise2(l, [&](Lit a, Lit b) { return aig.mkOr(a, b); });
            case BTOR2_TAG_nor: return bitwise2(l, [&](Lit a, Lit b) { return neg(aig.mkOr(a, b)); });
            case BTOR2_TAG_xor: return bitwise2(l, [&](Lit a, Lit b) { return aig.mkXor(a, b); });
            case BTOR2_TAG_xnor: return bitwise2(l, [&](Lit a, Lit b) { return aig.mkXnor(a, b); });
            case BTOR2_TAG_implies: return bitwise2(l, [&](Lit a, Lit b) { return aig.mkOr(neg(a), b); });
            case BTOR2_TAG_iff: return bitwise2(l, [&](Lit a, Lit b) { return aig.mkXnor(a, b); });
            case BTOR2_TAG_ite: {
                Lit c = arg(l, 0).at(0);
                Bits t = arg(l, 1), e = arg(l, 2), r;
                for (size_t i = 0; i < t.size(); i++)
                    r.push_back(aig.mkIte(c, t[i], e[i]));
                return r;
            }
            case BTOR2_TAG_redand: return {reduce(arg(l, 0), true)};
            case BTOR2_TAG_redor: return {neg(reduce(negAll(arg(l, 0)), true))};
            case BTOR2_TAG_redxor: {
                Lit x = kFalse;
                for (Lit b : arg(l, 0))
                    x = aig.mkXor(x, b);
                return {x};
            }
            case BTOR2_TAG_eq: return {equal(arg(l, 0), arg(l, 1))};
            case BTOR2_TAG_neq: return {neg(equal(arg(l, 0), arg(l, 1)))};
            case BTOR2_TAG_ult: return {lessThan(arg(l, 0), arg(l, 1), false, false)};
            case BTOR2_TAG_ulte: return {lessThan(arg(l, 0), arg(l, 1), false, true)};
            case BTOR2_TAG_ugt: return {lessThan(arg(l, 1), arg(l, 0), false, false)};
            case BTOR2_TAG_ugte: return {lessThan(arg(l, 1), arg(l, 0), false, true)};
            case BTOR2_TAG_slt: return {lessThan(arg(l, 0), arg(l, 1), true, false)};
            case BTOR2_TAG_slte: return {lessThan(arg(l, 0), arg(l, 1), true, true)};
            case BTOR2_TAG_sgt: return {lessThan(arg(l, 1), arg(l, 0), true, false)};
            case BTOR2_TAG_sgte: return {lessThan(arg(l, 1), arg(l, 0), true, true)};
            case BTOR2_TAG_add: return add(arg(l, 0), arg(l, 1), kFalse);
            case BTOR2_TAG_sub: return add(arg(l, 0), negAll(arg(l, 1)), kTrue);
            case BTOR2_TAG_neg: return add(Bits(w, kFalse), negAll(arg(l, 0)), kTrue);
            case BTOR2_TAG_inc: return add(arg(l, 0), Bits(w, kFalse), kTrue);
            case BTOR2_TAG_dec: return add(arg(l, 0), Bits(w, kTrue), kFalse);
            case BTOR2_TAG_mul: return mul(arg(l, 0), arg(l, 1));
            case BTOR2_TAG_udiv: return divmod(arg(l, 0), arg(l, 1)).first;
            case BTOR2_TAG_urem: return divmod(arg(l, 0), arg(l, 1)).second;
            case BTOR2_TAG_sdiv:
            case BTOR2_TAG_srem:
            case BTOR2_TAG_smod: return signedDiv(l);
            case BTOR2_TAG_sll: return shift(arg(l, 0), arg(l, 1), 0);
            case BTOR2_TAG_srl: return shift(arg(l, 0), arg(l, 1), 1);
            case BTOR2_TAG_sra: return shift(arg(l, 0), arg(l, 1), 2);
            case BTOR2_TAG_rol: return rotate(arg(l, 0), arg(l, 1), true);
            case BTOR2_TAG_ror: return rotate(arg(l, 0), arg(l, 1), false);
            case BTOR2_TAG_concat: {
                Bits hi = arg(l, 0), lo = arg(l, 1);
                lo.insert(lo.end(), hi.begin(), hi.end());
                return lo;
            }
            case BTOR2_TAG_slice: {
                Bits a = arg(l, 0);
                auto upper = l->args[1], lower = l->args[2];
                return Bits(a.begin() + lower, a.begin() + upper + 1);
            }
            case BTOR2_TAG_uext:
            case BTOR2_TAG_sext: {
                Bits a = arg(l, 0);
                Lit fill = l->tag == BTOR2_TAG_sext && !a.empty() ? a.back() : kFalse;
                a.resize(w, fill);
                return a;
            }
            default:
                fail(l, "unsupported BTOR2 operator '" + std::string(l->name) + "'");
        }
    }

    Bits bitwise1(Bits a, const std::function<Lit(Lit)>& f) {
        for (auto& x : a)
            x = f(x);
        return a;
    }
    Bits bitwise2(const Btor2Line* l, const std::function<Lit(Lit, Lit)>& f) {
        Bits a = arg(l, 0), b = arg(l, 1), r;
        for (size_t i = 0; i < a.size(); i++)
            r.push_back(f(a[i], b[i]));
        return r;
    }
    static Bits negAll(Bits a) {
        for (auto& x : a)
            x = neg(x);
        return a;
    }
    Lit reduce(const Bits& a, bool) {
        Lit r = kTrue;
        for (Lit b : a)
            r = aig.mkAnd(r, b);
        return r;
    }
    Lit equal(const Bits& a, const Bits& b) {
        Lit r = kTrue;
        for (size_t i = 0; i < a.size(); i++)
            r = aig.mkAnd(r, aig.mkXnor(a[i], b[i]));
        return r;
    }
    /// a < b (or a <= b), unsigned or signed.
    Lit lessThan(Bits a, Bits b, bool isSigned, bool orEqual) {
        if (isSigned && !a.empty()) {
            // Flipping the sign bits turns a signed compare into an unsigned one.
            a.back() = neg(a.back());
            b.back() = neg(b.back());
        }
        Lit lt = orEqual ? kTrue : kFalse; // result for equal prefixes, LSB upward
        for (size_t i = 0; i < a.size(); i++) {
            Lit bitLt = aig.mkAnd(neg(a[i]), b[i]);
            Lit bitEq = aig.mkXnor(a[i], b[i]);
            lt = aig.mkOr(bitLt, aig.mkAnd(bitEq, lt));
        }
        return lt;
    }
    Bits add(const Bits& a, const Bits& b, Lit carry) {
        Bits r;
        for (size_t i = 0; i < a.size(); i++) {
            Lit s = aig.mkXor(a[i], b[i]);
            r.push_back(aig.mkXor(s, carry));
            carry = aig.mkOr(aig.mkAnd(a[i], b[i]), aig.mkAnd(s, carry));
        }
        return r;
    }
    Bits mul(const Bits& a, const Bits& b) {
        Bits acc(a.size(), kFalse);
        for (size_t i = 0; i < b.size(); i++) {
            Bits partial(a.size(), kFalse);
            for (size_t j = 0; j + i < a.size(); j++)
                partial[j + i] = aig.mkAnd(a[j], b[i]);
            acc = add(acc, partial, kFalse);
        }
        return acc;
    }
    /// Restoring division. Division by zero follows BTOR2/SMT-LIB:
    /// a / 0 = all ones, a % 0 = a.
    std::pair<Bits, Bits> divmod(const Bits& a, const Bits& b) {
        size_t n = a.size();
        // The partial remainder needs n+1 bits: before each shift r < b < 2^n.
        Bits bx = b, r(n + 1, kFalse), q(n, kFalse);
        bx.push_back(kFalse);
        for (size_t i = n; i-- > 0;) {
            r.insert(r.begin(), a[i]); // r = (r << 1) | a[i]
            r.pop_back();
            Lit geq = neg(lessThan(r, bx, false, false));
            Bits diff = add(r, negAll(bx), kTrue);
            for (size_t k = 0; k <= n; k++)
                r[k] = aig.mkIte(geq, diff[k], r[k]);
            q[i] = geq;
        }
        r.pop_back();
        return {q, r};
    }
    Bits signedDiv(const Btor2Line* l) {
        Bits a = arg(l, 0), b = arg(l, 1);
        size_t n = a.size();
        Lit sa = a.back(), sb = b.back();
        auto absv = [&](const Bits& x, Lit s) {
            Bits negx = add(Bits(n, kFalse), negAll(x), kTrue);
            Bits r;
            for (size_t i = 0; i < n; i++)
                r.push_back(aig.mkIte(s, negx[i], x[i]));
            return r;
        };
        auto negIf = [&](const Bits& x, Lit s) { return absv(x, s); };
        auto [q, r] = divmod(absv(a, sa), absv(b, sb));
        if (l->tag == BTOR2_TAG_sdiv)
            return negIf(q, aig.mkXor(sa, sb));
        Bits srem = negIf(r, sa);
        if (l->tag == BTOR2_TAG_srem)
            return srem;
        // smod: sign follows the divisor; if remainder != 0 and signs differ, add b.
        Lit rzero = neg(reduceOr(srem));
        Lit adjust = aig.mkAnd(neg(rzero), aig.mkXor(sa, sb));
        Bits sum = add(srem, b, kFalse);
        Bits out;
        for (size_t i = 0; i < n; i++)
            out.push_back(aig.mkIte(adjust, sum[i], srem[i]));
        return out;
    }
    Lit reduceOr(const Bits& a) {
        Lit r = kFalse;
        for (Lit b : a)
            r = aig.mkOr(r, b);
        return r;
    }
    /// Barrel shifter. mode 0 = sll, 1 = srl, 2 = sra.
    Bits shift(Bits a, const Bits& amt, int mode) {
        size_t n = a.size();
        Lit fill = mode == 2 ? a.back() : kFalse;
        Lit overflow = kFalse;
        for (size_t s = 0; s < amt.size(); s++) {
            size_t dist = s < 63 ? (size_t(1) << s) : n;
            if (dist >= n) {
                overflow = aig.mkOr(overflow, amt[s]);
                continue;
            }
            Bits shifted(n);
            for (size_t i = 0; i < n; i++) {
                if (mode == 0)
                    shifted[i] = i >= dist ? a[i - dist] : kFalse;
                else
                    shifted[i] = i + dist < n ? a[i + dist] : fill;
            }
            for (size_t i = 0; i < n; i++)
                a[i] = aig.mkIte(amt[s], shifted[i], a[i]);
        }
        for (auto& x : a)
            x = aig.mkIte(overflow, fill, x);
        return a;
    }
    Bits rotate(Bits a, const Bits& amt, bool left) {
        size_t n = a.size();
        // Amount bit s rotates by 2^s mod n; every bit counts (for widths that
        // are not powers of two, high bits still change the result).
        size_t dist = 1 % n;
        for (size_t s = 0; s < amt.size(); s++, dist = (2 * dist) % n) {
            Bits rot(n);
            for (size_t i = 0; i < n; i++)
                rot[i] = left ? a[(i + n - dist) % n] : a[(i + dist) % n];
            for (size_t i = 0; i < n; i++)
                a[i] = aig.mkIte(amt[s], rot[i], a[i]);
        }
        return a;
    }

    Btor2Parser* p;
    TransitionSystem& ts;
    Aig& aig;
    std::map<int64_t, Bits> nodes;
    std::map<int64_t, size_t> latchIndex;
    std::set<int64_t> hasNext;
};

} // namespace

bool loadBtor2(const std::string& path, TransitionSystem& ts, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::unique_ptr<Btor2Parser, void (*)(Btor2Parser*)> p(btor2parser_new(), btor2parser_delete);
    bool ok = btor2parser_read_lines(p.get(), f);
    std::fclose(f);
    if (!ok) {
        error = std::string("BTOR2 parse error: ") + btor2parser_error(p.get());
        return false;
    }
    try {
        Blaster(p.get(), ts).run();
    }
    catch (const LoadError& e) {
        error = e.msg;
        return false;
    }
    return true;
}

} // namespace qfv
