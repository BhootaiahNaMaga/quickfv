// SV expression → AIG, and SVA monitor → AIG latches.
//
// slang has already applied SystemVerilog's sizing and signedness rules by
// inserting Conversion nodes, so every node's type width is final: operands
// of a binary operator have the result's width, and a conversion says how to
// extend. The builder therefore follows types, never re-deriving SV rules.
// Semantics are 2-state (X/Z are not modelled; SPEC section 6.2).
#include "sva/aig_emit.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/Type.h"

namespace qfv::sva {

using namespace slang;
using namespace slang::ast;
using Bits = std::vector<Lit>;

namespace {

class Builder {
public:
    Builder(TransitionSystem& ts, std::string topPath, const Symbol* scope) :
        ts(ts), aig(ts.aig), top(std::move(topPath)), scope(scope) {}

    /// Sampled-value helpers of the assertion being emitted: call → bits.
    std::map<const CallExpression*, Bits> sampled;

    Bits build(const Expression& e) {
        if (auto c = constant(e))
            return *c;
        uint32_t w = width(e);
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                return signal(e.as<ValueExpressionBase>().symbol, e);
            case ExpressionKind::Conversion: return conversion(e.as<ConversionExpression>(), w);
            case ExpressionKind::UnaryOp: return unary(e.as<UnaryExpression>(), w);
            case ExpressionKind::BinaryOp: return binary(e.as<BinaryExpression>(), w);
            case ExpressionKind::ConditionalOp: {
                auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern)
                    fail("conditional with patterns", e);
                Lit sel = boolOf(*c.conditions[0].expr);
                Bits t = build(c.left()), f = build(c.right()), r;
                for (size_t i = 0; i < t.size(); i++)
                    r.push_back(aig.mkIte(sel, t[i], f[i]));
                return r;
            }
            case ExpressionKind::Concatenation: {
                Bits r;
                auto ops = e.as<ConcatenationExpression>().operands();
                for (size_t i = ops.size(); i-- > 0;) { // first operand is the MSB part
                    Bits b = build(*ops[i]);
                    r.insert(r.end(), b.begin(), b.end());
                }
                return r;
            }
            case ExpressionKind::Replication: {
                auto& rep = e.as<ReplicationExpression>();
                auto n = constInt(rep.count());
                Bits one = build(rep.concat()), r;
                for (int64_t i = 0; i < n; i++)
                    r.insert(r.end(), one.begin(), one.end());
                return r;
            }
            case ExpressionKind::ElementSelect: return elementSelect(e.as<ElementSelectExpression>(), w);
            case ExpressionKind::RangeSelect: return rangeSelect(e.as<RangeSelectExpression>());
            case ExpressionKind::Call: return call(e.as<CallExpression>(), w);
            default: fail("expression kind '" + std::string(toString(e.kind)) + "'", e);
        }
    }

    Lit boolOf(const Expression& e) { return reduceOr(build(e)); }

    Lit reduceOr(const Bits& b) {
        Lit r = kFalse;
        for (Lit x : b)
            r = aig.mkOr(r, x);
        return r;
    }

private:
    [[noreturn]] void fail(const std::string& what, const Expression& e) {
        (void)e;
        throw EmitError("not supported in assertion expressions (session mode): " + what);
    }

    uint32_t width(const Expression& e) {
        if (!e.type->isIntegral())
            fail("non-integral type '" + e.type->toString() + "'", e);
        return uint32_t(e.type->getBitWidth());
    }

    static Bits fromSVInt(const SVInt& v, uint32_t w) {
        Bits b;
        for (uint32_t i = 0; i < w; i++)
            b.push_back(i < v.getBitWidth() && v[int32_t(i)] == logic_t(1) ? kTrue : kFalse);
        return b;
    }

    /// Constant-folds parameters, literals and constant sub-expressions.
    std::optional<Bits> constant(const Expression& e) {
        if (e.kind == ExpressionKind::NamedValue) {
            auto& sym = e.as<NamedValueExpression>().symbol;
            if (sym.kind != SymbolKind::Parameter && sym.kind != SymbolKind::EnumValue)
                return std::nullopt;
        }
        else if (e.kind != ExpressionKind::IntegerLiteral &&
                 e.kind != ExpressionKind::UnbasedUnsizedIntegerLiteral && !e.getConstant()) {
            return std::nullopt;
        }
        if (auto cv = e.getConstant(); cv && cv->isInteger())
            return fromSVInt(cv->integer(), width(e));
        if (!scope)
            return std::nullopt;
        EvalContext ctx(*scope);
        auto cv = e.eval(ctx);
        if (!cv.isInteger())
            return std::nullopt;
        return fromSVInt(cv.integer(), width(e));
    }

    int64_t constInt(const Expression& e) {
        auto c = constant(e);
        if (!c)
            fail("non-constant where a constant is required", e);
        int64_t v = 0;
        for (size_t i = 0; i < c->size() && i < 63; i++)
            if ((*c)[i] == kTrue)
                v |= int64_t(1) << i;
        return v;
    }

    /// Design signal by hierarchical name, relative to the top instance.
    Bits signal(const ValueSymbol& sym, const Expression& e) {
        auto path = sym.getHierarchicalPath();
        if (path.rfind(top + ".", 0) == 0)
            path = path.substr(top.size() + 1);
        auto it = ts.signals.find(path);
        if (it == ts.signals.end())
            throw EmitError("signal '" + path +
                            "' is not visible in the synthesized model (optimized away or an "
                            "unpacked array used as a whole)");
        Bits b = it->second;
        // A width mismatch means the name map does not describe this signal:
        // refuse rather than silently truncate or extend.
        if (b.size() != width(e))
            throw EmitError("signal '" + path + "' has " + std::to_string(b.size()) +
                            " bits in the synthesized model but " + std::to_string(width(e)) +
                            " in the RTL");
        return b;
    }

    Bits resize(Bits b, uint32_t w, bool signExtend) {
        Lit fill = signExtend && !b.empty() ? b.back() : kFalse;
        b.resize(w, fill);
        return b;
    }

    Bits conversion(const ConversionExpression& c, uint32_t w) {
        auto& op = c.operand();
        return resize(build(op), w, op.type->isSigned());
    }

    Bits unary(const UnaryExpression& u, uint32_t w) {
        Bits a = build(u.operand());
        switch (u.op) {
            case UnaryOperator::Plus: return a;
            case UnaryOperator::Minus: return sub(Bits(a.size(), kFalse), a);
            case UnaryOperator::BitwiseNot:
                for (auto& x : a)
                    x = neg(x);
                return a;
            case UnaryOperator::BitwiseAnd: return resize({andAll(a)}, w, false);
            case UnaryOperator::BitwiseNand: return resize({neg(andAll(a))}, w, false);
            case UnaryOperator::BitwiseOr: return resize({reduceOr(a)}, w, false);
            case UnaryOperator::BitwiseNor: return resize({neg(reduceOr(a))}, w, false);
            case UnaryOperator::BitwiseXor: return resize({xorAll(a)}, w, false);
            case UnaryOperator::BitwiseXnor: return resize({neg(xorAll(a))}, w, false);
            case UnaryOperator::LogicalNot: return resize({neg(reduceOr(a))}, w, false);
            default: fail("increment/decrement operators", u);
        }
    }

    Lit andAll(const Bits& a) {
        Lit r = kTrue;
        for (Lit x : a)
            r = aig.mkAnd(r, x);
        return r;
    }
    Lit xorAll(const Bits& a) {
        Lit r = kFalse;
        for (Lit x : a)
            r = aig.mkXor(r, x);
        return r;
    }
    Bits add(const Bits& a, const Bits& b, Lit carry = kFalse) {
        Bits r;
        for (size_t i = 0; i < a.size(); i++) {
            Lit s = aig.mkXor(a[i], b[i]);
            r.push_back(aig.mkXor(s, carry));
            carry = aig.mkOr(aig.mkAnd(a[i], b[i]), aig.mkAnd(s, carry));
        }
        return r;
    }
    Bits sub(const Bits& a, Bits b) {
        for (auto& x : b)
            x = neg(x);
        return add(a, b, kTrue);
    }
    Lit equal(const Bits& a, const Bits& b) {
        Lit r = kTrue;
        for (size_t i = 0; i < a.size(); i++)
            r = aig.mkAnd(r, aig.mkXnor(a[i], b[i]));
        return r;
    }
    Lit lessThan(Bits a, Bits b, bool isSigned, bool orEqual) {
        if (isSigned && !a.empty()) {
            a.back() = neg(a.back());
            b.back() = neg(b.back());
        }
        Lit lt = orEqual ? kTrue : kFalse;
        for (size_t i = 0; i < a.size(); i++)
            lt = aig.mkOr(aig.mkAnd(neg(a[i]), b[i]), aig.mkAnd(aig.mkXnor(a[i], b[i]), lt));
        return lt;
    }
    Bits mul(const Bits& a, const Bits& b) {
        Bits acc(a.size(), kFalse);
        for (size_t i = 0; i < b.size(); i++) {
            Bits partial(a.size(), kFalse);
            for (size_t j = 0; j + i < a.size(); j++)
                partial[j + i] = aig.mkAnd(a[j], b[i]);
            acc = add(acc, partial);
        }
        return acc;
    }
    /// Barrel shift; mode 0 = left, 1 = logical right, 2 = arithmetic right.
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
            Bits sh(n);
            for (size_t i = 0; i < n; i++)
                sh[i] = mode == 0 ? (i >= dist ? a[i - dist] : kFalse) : (i + dist < n ? a[i + dist] : fill);
            for (size_t i = 0; i < n; i++)
                a[i] = aig.mkIte(amt[s], sh[i], a[i]);
        }
        for (auto& x : a)
            x = aig.mkIte(overflow, fill, x);
        return a;
    }

    Bits binary(const BinaryExpression& b, uint32_t w) {
        using Op = BinaryOperator;
        auto boolRes = [&](Lit l) { return resize({l}, w, false); };
        switch (b.op) {
            case Op::LogicalAnd: return boolRes(aig.mkAnd(boolOf(b.left()), boolOf(b.right())));
            case Op::LogicalOr: return boolRes(aig.mkOr(boolOf(b.left()), boolOf(b.right())));
            case Op::LogicalImplication: return boolRes(aig.mkOr(neg(boolOf(b.left())), boolOf(b.right())));
            case Op::LogicalEquivalence: return boolRes(aig.mkXnor(boolOf(b.left()), boolOf(b.right())));
            default: break;
        }
        Bits l = build(b.left()), r = build(b.right());
        bool sgn = b.left().type->isSigned() && b.right().type->isSigned();
        auto bitwise = [&](auto f) {
            Bits out;
            for (size_t i = 0; i < l.size(); i++)
                out.push_back(f(l[i], r[i]));
            return out;
        };
        switch (b.op) {
            case Op::Add: return add(l, r);
            case Op::Subtract: return sub(l, r);
            case Op::Multiply: return mul(l, r);
            case Op::BinaryAnd: return bitwise([&](Lit x, Lit y) { return aig.mkAnd(x, y); });
            case Op::BinaryOr: return bitwise([&](Lit x, Lit y) { return aig.mkOr(x, y); });
            case Op::BinaryXor: return bitwise([&](Lit x, Lit y) { return aig.mkXor(x, y); });
            case Op::BinaryXnor: return bitwise([&](Lit x, Lit y) { return aig.mkXnor(x, y); });
            case Op::Equality:
            case Op::CaseEquality: return boolRes(equal(l, r));
            case Op::Inequality:
            case Op::CaseInequality: return boolRes(neg(equal(l, r)));
            case Op::LessThan: return boolRes(lessThan(l, r, sgn, false));
            case Op::LessThanEqual: return boolRes(lessThan(l, r, sgn, true));
            case Op::GreaterThan: return boolRes(lessThan(r, l, sgn, false));
            case Op::GreaterThanEqual: return boolRes(lessThan(r, l, sgn, true));
            case Op::LogicalShiftLeft:
            case Op::ArithmeticShiftLeft: return shift(l, r, 0);
            case Op::LogicalShiftRight: return shift(l, r, 1);
            case Op::ArithmeticShiftRight: return shift(l, r, b.left().type->isSigned() ? 2 : 1);
            default: fail("operator '" + std::string(toString(b.op)) + "'", b);
        }
    }

    Bits elementSelect(const ElementSelectExpression& es, uint32_t w) {
        auto& val = es.value();
        auto& vt = val.type->getCanonicalType();
        if (vt.isUnpackedArray()) {
            // Unpacked arrays (memories) exist in the model element by element.
            auto nv = val.as_if<ValueExpressionBase>();
            if (!nv)
                fail("select from a computed unpacked array", es);
            auto range = vt.getFixedRange();
            std::string base = nv->symbol.getHierarchicalPath();
            if (base.rfind(top + ".", 0) == 0)
                base = base.substr(top.size() + 1);
            auto elem = [&](int32_t i) {
                auto it = ts.signals.find(base + "[" + std::to_string(i) + "]");
                if (it == ts.signals.end())
                    throw EmitError("array element '" + base + "[" + std::to_string(i) +
                                    "]' is not visible in the synthesized model");
                Bits b = it->second;
                if (b.size() != w)
                    throw EmitError("array element '" + base + "[" + std::to_string(i) + "]' has " +
                                    std::to_string(b.size()) + " bits in the synthesized model but " +
                                    std::to_string(w) + " in the RTL");
                return b;
            };
            bool sgn = es.selector().type->isSigned();
            if (auto c = constant(es.selector())) {
                int64_t i = bitsToInt(*c, sgn);
                return i >= range.lower() && i <= range.upper() ? elem(int32_t(i)) : undefined(w);
            }
            Bits sel = build(es.selector());
            Bits out = coversAll(range, sel.size(), sgn) ? Bits(w, kFalse) : undefined(w);
            for (int32_t i = range.lower(); i <= range.upper(); i++) {
                if (!representable(i, sel.size(), sgn))
                    continue;
                Lit hit = equal(sel, intBits(i, sel.size()));
                Bits e = elem(i);
                for (uint32_t k = 0; k < w; k++)
                    out[k] = aig.mkIte(hit, e[k], out[k]);
            }
            return out;
        }
        // Packed bit/part select: map SV index to bit position with the declared range.
        Bits v = build(val);
        auto range = vt.getFixedRange();
        uint32_t elemW = w;
        auto pos = [&](int64_t idx) { return int64_t(range.translateIndex(int32_t(idx))) * elemW; };
        bool sgn = es.selector().type->isSigned();
        if (auto c = constant(es.selector())) {
            int64_t i = bitsToInt(*c, sgn);
            if (i < range.lower() || i > range.upper())
                return undefined(elemW);
            int64_t p = pos(i);
            Bits out;
            for (uint32_t k = 0; k < elemW; k++)
                out.push_back(v[size_t(p + k)]);
            return out;
        }
        Bits sel = build(es.selector());
        Bits out = coversAll(range, sel.size(), sgn) ? Bits(w, kFalse) : undefined(w);
        for (int32_t i = range.lower(); i <= range.upper(); i++) {
            if (!representable(i, sel.size(), sgn))
                continue;
            Lit hit = equal(sel, intBits(i, sel.size()));
            int64_t p = pos(i);
            for (uint32_t k = 0; k < elemW; k++)
                out[k] = aig.mkIte(hit, v[size_t(p + k)], out[k]);
        }
        return out;
    }

    Bits rangeSelect(const RangeSelectExpression& rs) {
        if (rs.getSelectionKind() != RangeSelectionKind::Simple)
            fail("indexed part-select +: / -: (v1.1)", rs);
        Bits v = build(rs.value());
        auto range = rs.value().type->getCanonicalType().getFixedRange();
        int64_t a = range.translateIndex(int32_t(constInt(rs.left())));
        int64_t b = range.translateIndex(int32_t(constInt(rs.right())));
        if (a > b)
            std::swap(a, b);
        Bits out, x;
        for (int64_t i = a; i <= b; i++) {
            if (i >= 0 && size_t(i) < v.size()) {
                out.push_back(v[size_t(i)]);
                continue;
            }
            if (x.empty())
                x = undefined(uint32_t(b - a + 1));
            out.push_back(x[size_t(i - a)]);
        }
        return out;
    }

    /// The value of an out-of-range select: X in SystemVerilog, which formal
    /// semantics (and the Yosys flow's `setundef -anyseq`) treat as a fresh
    /// unconstrained value in every cycle, never as a convenient constant.
    Bits undefined(uint32_t w) {
        TransitionSystem::Input in;
        in.name = "$qfv_undef$" + std::to_string(ts.inputs.size());
        in.synthetic = true;
        for (uint32_t k = 0; k < w; k++)
            in.bits.push_back(aig.newInput());
        ts.inputs.push_back(in);
        return in.bits;
    }

    /// Can a `w`-bit selector hold index `i`? (Otherwise its bit pattern would
    /// alias a different index.)
    static bool representable(int64_t i, size_t w, bool sgn) {
        if (w >= 63)
            return true;
        int64_t lo = sgn ? -(int64_t(1) << (w - 1)) : 0;
        int64_t hi = sgn ? (int64_t(1) << (w - 1)) - 1 : (int64_t(1) << w) - 1;
        return i >= lo && i <= hi;
    }

    /// Does every value of a `w`-bit selector fall inside `range`?
    static bool coversAll(const ConstantRange& range, size_t w, bool sgn) {
        if (w >= 31)
            return false;
        int64_t lo = sgn ? -(int64_t(1) << (w - 1)) : 0;
        int64_t hi = sgn ? (int64_t(1) << (w - 1)) - 1 : (int64_t(1) << w) - 1;
        return range.lower() <= lo && range.upper() >= hi;
    }

    Bits call(const CallExpression& c, uint32_t w) {
        if (!c.isSystemCall())
            fail("user function calls", c);
        auto name = c.getSubroutineName();
        auto args = c.arguments();
        if (auto it = sampled.find(&c); it != sampled.end())
            return resize(it->second, w, false);
        if (name == "$sampled")
            return build(*args[0]);
        if (name == "$isunknown")
            return Bits(w, kFalse); // 2-state model
        if (name == "$onehot" || name == "$onehot0" || name == "$countones") {
            Bits a = build(*args[0]);
            // Population count, then compare.
            uint32_t cw = 1;
            while ((1u << cw) <= a.size())
                cw++;
            Bits count(cw, kFalse);
            for (Lit x : a) {
                Bits inc(cw, kFalse);
                inc[0] = x;
                count = add(count, inc);
            }
            if (name == "$countones")
                return resize(count, w, false);
            Lit isOne = equal(count, intBits(1, cw));
            if (name == "$onehot")
                return resize({isOne}, w, false);
            return resize({aig.mkOr(isOne, equal(count, intBits(0, cw)))}, w, false);
        }
        fail("system function " + std::string(name), c);
    }

    static int64_t bitsToInt(const Bits& b, bool isSigned) {
        int64_t v = 0;
        for (size_t i = 0; i < b.size() && i < 63; i++)
            if (b[i] == kTrue)
                v |= int64_t(1) << i;
        if (isSigned && !b.empty() && b.back() == kTrue && b.size() < 64)
            v -= int64_t(1) << b.size();
        return v;
    }
    static Bits intBits(int64_t v, size_t w) {
        Bits b;
        for (size_t i = 0; i < w; i++)
            b.push_back(((v >> (i < 63 ? i : 63)) & 1) ? kTrue : kFalse);
        return b;
    }

    TransitionSystem& ts;
    Aig& aig;
    std::string top;
    const Symbol* scope;
};

/// Adds a register (latch) with init 0 whose next value is set later.
struct Reg {
    size_t latch;
    Lit cur;
};

class MonitorBuilder {
public:
    MonitorBuilder(const AssertionIR& ir, TransitionSystem& ts, Builder& b) :
        ir(ir), ts(ts), aig(ts.aig), b(b), prefix("qfv_" + ir.label) {}

    EmitResult run() {
        emitHelpers();
        dis = ir.disableExpr ? b.boolOf(*ir.disableExpr) : kFalse;
        Lit ante = antecedent();
        EmitResult res;
        if (ir.kind == DirectiveKind::Cover) {
            res.triggerProp = addProp("qfv_cover__" + ir.label, aig.mkAnd(ante, neg(dis)),
                                      TransitionSystem::Property::Kind::Reach, false);
        }
        else {
            Lit fail = consequent(ante);
            Lit bad = aig.mkAnd(fail, neg(dis));
            if (ir.kind == DirectiveKind::Assert) {
                res.assertProp = addProp(ir.label, bad, TransitionSystem::Property::Kind::Assert, false);
                if (!ir.ante.elems.empty())
                    res.triggerProp = addProp("qfv_trigger__" + ir.label, aig.mkAnd(ante, neg(dis)),
                                              TransitionSystem::Property::Kind::Reach, true);
            }
            else {
                res.assumeOk = neg(bad);
            }
        }
        // Next-state functions: registers clear while disabled (helpers do not).
        for (auto& [reg, next, clears] : pending)
            ts.latches[reg.latch].next = {clears ? aig.mkAnd(next, neg(dis)) : next};
        return res;
    }

private:
    Reg newReg(const std::string& name) {
        TransitionSystem::Latch l;
        l.name = prefix + "_" + name;
        l.cur = {aig.newInput()};
        l.next = l.cur;
        l.init = {0};
        l.synthetic = true;
        ts.latches.push_back(l);
        return {ts.latches.size() - 1, l.cur[0]};
    }
    void setNext(Reg r, Lit next, bool clears = true) { pending.push_back({r, next, clears}); }

    size_t addProp(const std::string& name, Lit bad, TransitionSystem::Property::Kind kind, bool trigger) {
        TransitionSystem::Property p;
        p.name = name;
        p.label = ir.label;
        p.kind = kind;
        p.isTrigger = trigger;
        p.bad = bad;
        ts.props.push_back(p);
        return ts.props.size() - 1;
    }

    void emitHelpers() {
        for (auto& h : ir.helpers) {
            Bits now = b.build(*h.arg);
            std::vector<Bits> stages{now};
            for (uint32_t k = 1; k <= h.depth; k++) {
                Bits prev;
                for (size_t i = 0; i < now.size(); i++) {
                    Reg r = newReg(h.name.substr(prefix.size() + 1) + "_" + std::to_string(k) + "_" +
                                   std::to_string(i));
                    setNext(r, stages[k - 1][i], /*clears=*/false);
                    prev.push_back(r.cur);
                }
                stages.push_back(prev);
            }
            Bits out;
            auto& cur = stages[0];
            auto& old = stages[1];
            if (h.function == "$past")
                out = stages[h.depth];
            else if (h.function == "$rose")
                out = {aig.mkAnd(cur[0], neg(old[0]))};
            else if (h.function == "$fell")
                out = {aig.mkAnd(neg(cur[0]), old[0])};
            else {
                Lit eq = kTrue;
                for (size_t i = 0; i < cur.size(); i++)
                    eq = aig.mkAnd(eq, aig.mkXnor(cur[i], old[i]));
                out = {h.function == "$stable" ? eq : neg(eq)};
            }
            b.sampled[h.call] = out;
        }
    }

    Lit antecedent() {
        if (ir.ante.elems.empty())
            return kTrue;
        Lit entry = kTrue; // an attempt starts every cycle
        for (size_t i = 0; i < ir.ante.elems.size(); i++) {
            auto& el = ir.ante.elems[i];
            Lit bi = b.boolOf(*el.leaf.expr);
            Lit taps = el.lo == 0 ? entry : kFalse;
            Lit prev = entry;
            for (uint32_t w = 1; w <= el.hi; w++) {
                Reg d = newReg("ad" + std::to_string(i) + "_" + std::to_string(w));
                setNext(d, prev);
                prev = d.cur;
                if (w >= el.lo)
                    taps = aig.mkOr(taps, d.cur);
            }
            entry = aig.mkAnd(bi, taps);
        }
        return entry;
    }

    /// Age pipeline, one stage per attempt age (see monitor.cpp for the idea).
    Lit consequent(Lit start) {
        auto& elems = ir.cons.elems;
        std::vector<Lit> bl;
        for (auto& el : elems)
            bl.push_back(b.boolOf(*el.leaf.expr));
        uint32_t L = ir.cons.maxLength();
        // state[age][i][w] for w in 1..hi_i; age 0 has no registers.
        std::vector<std::vector<std::vector<Lit>>> state(L + 2);
        for (uint32_t age = 1; age <= L; age++) {
            state[age].resize(elems.size());
            for (size_t i = 0; i < elems.size(); i++)
                for (uint32_t w = 0; w <= elems[i].hi; w++)
                    state[age][i].push_back(w == 0 ? kFalse
                                                   : newReg("g" + std::to_string(age) + "_" +
                                                            std::to_string(i) + "_" + std::to_string(w))
                                                         .cur);
        }
        Lit fail = kFalse;
        for (uint32_t age = 0; age <= L; age++) {
            Lit entry = age == 0 ? start : kFalse;
            Lit live = kFalse, active = age == 0 ? start : kFalse;
            std::vector<std::pair<Lit, Lit>> nexts; // (register cur at age+1, next value)
            for (size_t i = 0; i < elems.size(); i++) {
                auto& el = elems[i];
                Lit taps = el.lo == 0 ? entry : kFalse;
                for (uint32_t w = 1; w <= el.hi; w++) {
                    Lit cur = age == 0 ? kFalse : state[age][i][w];
                    if (age > 0)
                        active = aig.mkOr(active, cur);
                    if (w >= el.lo)
                        taps = aig.mkOr(taps, cur);
                    if (w + 1 <= el.hi) {
                        live = aig.mkOr(live, cur);
                        if (age < L)
                            nexts.push_back({state[age + 1][i][w + 1], cur});
                    }
                }
                if (el.hi >= 1) {
                    live = aig.mkOr(live, entry);
                    if (age < L)
                        nexts.push_back({state[age + 1][i][1], entry});
                }
                entry = aig.mkAnd(bl[i], taps);
            }
            Lit acc = entry;
            fail = aig.mkOr(fail, aig.mkAnd(active, aig.mkAnd(neg(acc), neg(live))));
            for (auto& [reg, next] : nexts)
                pendingByCur(reg, aig.mkAnd(neg(acc), next));
        }
        return fail;
    }

    void pendingByCur(Lit cur, Lit next) {
        for (size_t l = ts.latches.size(); l-- > 0;)
            if (ts.latches[l].cur.size() == 1 && ts.latches[l].cur[0] == cur) {
                setNext({l, cur}, next);
                return;
            }
    }

    const AssertionIR& ir;
    TransitionSystem& ts;
    Aig& aig;
    Builder& b;
    std::string prefix;
    Lit dis = kFalse;
    struct Pending {
        Reg reg;
        Lit next;
        bool clears;
    };
    std::vector<Pending> pending;
};

} // namespace

EmitResult emitIntoAig(const AssertionIR& ir, TransitionSystem& ts, const std::string& topPath,
                       const std::string& clockName) {
    Builder b(ts, topPath, ir.scope);
    if (!ir.clockPosedge)
        throw EmitError("negedge-clocked assertions are not supported in session mode (v1.1)");
    if (ir.clockExpr) {
        // The assertion clock must be the design clock (possibly through a port).
        auto clk = ts.signals.find(clockName);
        Bits c = b.build(*ir.clockExpr);
        if (clk == ts.signals.end() || c != clk->second)
            throw EmitError("assertion is not clocked by the design clock '" + clockName + "'");
    }
    // Roll back on failure so a rejected assertion leaves the model unchanged.
    size_t nLatches = ts.latches.size(), nProps = ts.props.size();
    try {
        return MonitorBuilder(ir, ts, b).run();
    }
    catch (...) {
        ts.latches.resize(nLatches);
        ts.props.resize(nProps);
        throw;
    }
}

} // namespace qfv::sva
