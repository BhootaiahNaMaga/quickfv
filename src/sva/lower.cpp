#include "sva/lower.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/text/SourceManager.h"

namespace qfv::sva {

using namespace slang;
using namespace slang::ast;

Location toLocation(const SourceManager& sm, SourceLocation loc) {
    Location l;
    if (!loc.valid())
        return l;
    auto fl = sm.getFullyOriginalLoc(loc);
    l.file = std::string(sm.getFileName(fl));
    l.line = sm.getLineNumber(fl);
    l.column = sm.getColumnNumber(fl);
    return l;
}

namespace {

/// Thrown internally when a construct is outside the v1 subset.
struct Unsupported {
    std::string reason;
    SourceRange range;
    bool isError = false; // the assertion itself is erroneous
};

[[noreturn]] void unsupported(std::string reason, SourceRange range) {
    throw Unsupported{std::move(reason), range};
}

[[noreturn]] void erroneous(SourceRange range) {
    throw Unsupported{"assertion has errors; see diagnostics", range, true};
}

const char* sampledFunctions[] = {"$past", "$rose", "$fell", "$stable", "$changed", "$sampled"};

bool isSampledCall(const Expression& e) {
    auto call = e.as_if<CallExpression>();
    if (!call || !call->isSystemCall())
        return false;
    auto name = call->getSubroutineName();
    return std::ranges::find(sampledFunctions, name) != std::end(sampledFunctions);
}

/// Finds sampled-value calls inside a boolean leaf.
struct SampledCallFinder : public ASTVisitor<SampledCallFinder, VisitFlags::Expressions> {
    std::vector<const CallExpression*> calls;

    void handle(const CallExpression& call) {
        if (isSampledCall(call)) {
            calls.push_back(&call);
            return; // arguments are checked separately for nesting
        }
        visitDefault(call);
    }
};

class Lowerer {
public:
    Lowerer(const SourceManager& sm, const Symbol& contextSym, std::string prefix) :
        sm(sm), contextSym(contextSym), prefix(std::move(prefix)) {}

    AssertionIR lower(const ConcurrentAssertionStatement& stmt) {
        AssertionIR ir;
        ir.scope = &contextSym;
        switch (stmt.assertionKind) {
            case AssertionKind::Assert: ir.kind = DirectiveKind::Assert; break;
            case AssertionKind::Assume: ir.kind = DirectiveKind::Assume; break;
            case AssertionKind::CoverProperty:
            case AssertionKind::CoverSequence: ir.kind = DirectiveKind::Cover; break;
            default: unsupported("only assert/assume/cover directives are supported", stmt.sourceRange);
        }
        auto isAction = [](const Statement* st) {
            return st && st->kind != StatementKind::Empty;
        };
        if (isAction(stmt.ifTrue) || isAction(stmt.ifFalse))
            unsupported("action blocks (pass/fail statements) are not supported", stmt.sourceRange);

        if (stmt.propertySpec.bad())
            erroneous(stmt.sourceRange);
        const AssertionExpr* p = &stmt.propertySpec;
        bool sawClock = false;
        // Peel clocking and disable iff, in whichever order they appear, looking
        // through named property instances.
        while (true) {
            p = unwrapInstance(*p);
            if (auto c = p->as_if<ClockingAssertionExpr>()) {
                if (sawClock)
                    unsupported("multiple clocks", sourceRange(*p));
                ir.clockEvent = clockText(c->clocking);
                if (auto ev = c->clocking.as_if<SignalEventControl>()) {
                    ir.clockExpr = &ev->expr;
                    ir.clockPosedge = ev->edge == EdgeKind::PosEdge;
                }
                sawClock = true;
                p = &c->expr;
            }
            else if (auto d = p->as_if<DisableIffAssertionExpr>()) {
                if (!ir.disableText.empty())
                    unsupported("nested disable iff", sourceRange(*p));
                ir.disableText = leafText(d->condition, /*allowSampled=*/false);
                ir.disableExpr = &d->condition;
                p = &d->expr;
            }
            else {
                break;
            }
        }
        if (!sawClock)
            unsupported("no explicit clock; write @(posedge clk) in the property "
                        "(default clocking is v1.1)",
                        stmt.sourceRange);

        if (ir.kind == DirectiveKind::Cover) {
            ir.ante = lowerSeq(*p);
        }
        else {
            lowerProperty(*p, ir.ante, ir.cons);
        }
        ir.helpers = std::move(helpers);
        return ir;
    }

private:
    SourceRange sourceRange(const AssertionExpr& e) {
        return e.syntax ? e.syntax->sourceRange() : SourceRange();
    }

    const AssertionExpr* unwrapInstance(const AssertionExpr& e) {
        auto simple = e.as_if<SimpleAssertionExpr>();
        if (!simple || simple->repetition)
            return &e;
        auto inst = simple->expr.as_if<AssertionInstanceExpression>();
        if (!inst)
            return &e;
        if (!inst->arguments.empty())
            unsupported("property/sequence arguments are not supported yet (v1.1)",
                        simple->expr.sourceRange);
        if (!inst->localVars.empty())
            unsupported("local assertion variables", simple->expr.sourceRange);
        if (inst->isRecursiveProperty)
            unsupported("recursive property", simple->expr.sourceRange);
        // The monitor is emitted at the assertion's location, so the named
        // declaration's text must resolve names in the same scope.
        auto declScope = inst->symbol.getParentScope();
        auto useScope = contextSym.getParentScope();
        if (!declScope || !useScope ||
            declScope->getContainingInstance() != useScope->getContainingInstance())
            unsupported("property/sequence declared outside the asserting module",
                        simple->expr.sourceRange);
        return unwrapInstance(inst->body);
    }

    std::string clockText(const TimingControl& tc) {
        auto ev = tc.as_if<SignalEventControl>();
        if (!ev)
            unsupported("clock must be a single edge event like @(posedge clk)", tc.sourceRange);
        if (ev->iffCondition)
            unsupported("iff in clocking event", tc.sourceRange);
        std::string edge;
        switch (ev->edge) {
            case EdgeKind::PosEdge: edge = "posedge "; break;
            case EdgeKind::NegEdge: edge = "negedge "; break;
            default: unsupported("clock must be posedge or negedge", tc.sourceRange);
        }
        return edge + leafText(ev->expr, /*allowSampled=*/false);
    }

    void lowerProperty(const AssertionExpr& e0, Chain& ante, Chain& cons) {
        auto& e = *unwrapInstance(e0);
        if (auto b = e.as_if<BinaryAssertionExpr>()) {
            if (b->op == BinaryAssertionOperator::OverlappedImplication ||
                b->op == BinaryAssertionOperator::NonOverlappedImplication) {
                ante = lowerSeq(b->left);
                cons = lowerSeq(b->right, "the consequent of an implication must be a sequence "
                                          "(nested implications and property operators are "
                                          "not supported)");
                if (b->op == BinaryAssertionOperator::NonOverlappedImplication) {
                    cons.elems.front().lo += 1;
                    cons.elems.front().hi += 1;
                }
                return;
            }
        }
        // A sequence (or boolean) used as a property: checked from every cycle.
        cons = lowerSeq(e, "unsupported property operator");
    }

    Chain lowerSeq(const AssertionExpr& e0,
                   const char* context = "unsupported sequence operator") {
        auto& e = *unwrapInstance(e0);
        switch (e.kind) {
            case AssertionExprKind::Simple: {
                auto& s = e.as<SimpleAssertionExpr>();
                if (s.repetition)
                    unsupported("sequence repetition [*], [->], [=] is v1.1", sourceRange(e));
                Chain c;
                c.elems.push_back(ChainElem{0, 0, Leaf{leafText(s.expr, true), &s.expr}});
                return c;
            }
            case AssertionExprKind::SequenceConcat: {
                Chain out;
                for (auto& el : e.as<SequenceConcatExpr>().elements) {
                    if (!el.delay.max)
                        unsupported("unbounded delay ##[m:$] is liveness-like; not supported in v1",
                                    el.delayRange);
                    Chain sub = lowerSeq(*el.sequence, context);
                    sub.elems.front().lo += el.delay.min;
                    sub.elems.front().hi += *el.delay.max;
                    for (auto& x : sub.elems)
                        out.elems.push_back(std::move(x));
                }
                return out;
            }
            case AssertionExprKind::StrongWeak:
                if (e.as<StrongWeakAssertionExpr>().strength == StrongWeakAssertionExpr::Strong)
                    unsupported("strong() sequences need liveness checking; not supported in v1",
                                sourceRange(e));
                return lowerSeq(e.as<StrongWeakAssertionExpr>().expr, context);
            case AssertionExprKind::Unary:
                unsupported("property operators (not, nexttime, always, eventually, ...) are "
                            "not supported in v1",
                            sourceRange(e));
            case AssertionExprKind::Binary: {
                auto op = e.as<BinaryAssertionExpr>().op;
                if (op == BinaryAssertionOperator::OverlappedImplication ||
                    op == BinaryAssertionOperator::NonOverlappedImplication)
                    unsupported(context, sourceRange(e));
                unsupported("sequence/property operator '" + std::string(toString(op)) +
                                "' is not supported in v1",
                            sourceRange(e));
            }
            case AssertionExprKind::Clocking:
                unsupported("multi-clock or nested clocking", sourceRange(e));
            case AssertionExprKind::Invalid:
                erroneous(sourceRange(e));
            default:
                unsupported(context, sourceRange(e));
        }
    }

    /// Source text of a boolean expression, with sampled-value calls replaced by
    /// helper register names.
    std::string leafText(const Expression& e, bool allowSampled) {
        if (e.bad())
            erroneous(e.sourceRange);
        auto range = e.sourceRange;
        std::string text = rawText(range);

        SampledCallFinder finder;
        e.visit(finder);
        if (finder.calls.empty())
            return text;
        if (!allowSampled)
            unsupported("sampled-value functions are not allowed here", range);

        struct Repl {
            size_t begin, end;
            std::string with;
        };
        std::vector<Repl> repls;
        for (auto call : finder.calls) {
            auto cr = call->sourceRange;
            if (cr.start().buffer() != range.start().buffer() ||
                cr.start().offset() < range.start().offset() ||
                cr.end().offset() > range.end().offset())
                unsupported("sampled-value call outside its expression's text", cr);
            repls.push_back({cr.start().offset() - range.start().offset(),
                             cr.end().offset() - range.start().offset(), sampledReplacement(*call)});
        }
        std::ranges::sort(repls, [](auto& a, auto& b) { return a.begin > b.begin; });
        for (auto& r : repls)
            text.replace(r.begin, r.end - r.begin, r.with);
        return text;
    }

    std::string rawText(SourceRange range) {
        if (!range.start().valid() || sm.isMacroLoc(range.start()) || sm.isMacroLoc(range.end()))
            unsupported("assertion text comes from a macro expansion", range);
        auto src = sm.getSourceText(range.start().buffer());
        return std::string(src.substr(range.start().offset(),
                                      range.end().offset() - range.start().offset()));
    }

    std::string sampledReplacement(const CallExpression& call) {
        auto name = call.getSubroutineName();
        auto args = call.arguments();
        if (args.empty())
            unsupported("sampled-value call without arguments", call.sourceRange);
        const Expression& arg = *args[0];
        SampledCallFinder nested;
        arg.visit(nested);
        if (!nested.calls.empty())
            unsupported("nested sampled-value functions", call.sourceRange);

        uint32_t depth = 1;
        if (name == "$past") {
            if (args.size() > 2)
                unsupported("$past with gating expression or clock is not supported",
                            call.sourceRange);
            if (args.size() == 2) {
                EvalContext ctx(contextSym);
                auto cv = args[1]->eval(ctx);
                std::optional<uint32_t> n;
                if (cv.isInteger())
                    n = cv.integer().as<uint32_t>();
                if (!n || *n == 0 || *n > 64)
                    unsupported("$past number of ticks must be a constant in 1..64",
                                args[1]->sourceRange);
                depth = *n;
            }
        }
        else if (args.size() > 1) {
            unsupported(std::string(name) + " with a clocking argument is not supported",
                        call.sourceRange);
        }
        if (name == "$sampled")
            return "(" + leafText(arg, false) + ")";

        uint32_t width = uint32_t(arg.type->getBitWidth());
        if (width == 0)
            unsupported("sampled expression has no bit width", arg.sourceRange);

        SampledHelper h;
        h.name = prefix + "_s" + std::to_string(helpers.size());
        h.exprText = leafText(arg, false);
        h.width = width;
        h.depth = depth;
        h.function = std::string(name);
        h.call = &call;
        h.arg = &arg;
        helpers.push_back(h);

        // $rose/$fell look at bit 0; a 1-bit wire cannot be indexed in every tool.
        std::string bit0 = width > 1 ? "[0]" : "";
        auto now = h.name + "_0";
        auto prev = h.name + "_1";
        if (name == "$past")
            return h.name + "_" + std::to_string(depth);
        if (name == "$rose")
            return "(" + now + bit0 + " && !" + prev + bit0 + ")";
        if (name == "$fell")
            return "(!" + now + bit0 + " && " + prev + bit0 + ")";
        if (name == "$stable")
            return "(" + now + " == " + prev + ")";
        return "(" + now + " != " + prev + ")"; // $changed
    }

    const SourceManager& sm;
    const Symbol& contextSym;
    std::string prefix;
    std::vector<SampledHelper> helpers;
};

std::string sanitize(std::string_view s) {
    std::string r;
    for (char c : s)
        r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return r;
}

std::string directiveName(AssertionKind k) {
    switch (k) {
        case AssertionKind::Assert: return "assert";
        case AssertionKind::Assume: return "assume";
        case AssertionKind::CoverProperty:
        case AssertionKind::CoverSequence: return "cover";
        case AssertionKind::Restrict: return "restrict";
        case AssertionKind::Expect: return "expect";
    }
    return "?";
}

/// The label of `label: assert property ...`, taken from the syntax.
std::string labelOf(const syntax::SyntaxNode* member) {
    if (!member)
        return {};
    const syntax::StatementSyntax* stmt = nullptr;
    if (member->kind == syntax::SyntaxKind::ConcurrentAssertionMember)
        stmt = &member->as<syntax::ConcurrentAssertionMemberSyntax>().statement->as<syntax::StatementSyntax>();
    else if (syntax::StatementSyntax::isKind(member->kind))
        stmt = &member->as<syntax::StatementSyntax>();
    if (stmt && stmt->label)
        return std::string(stmt->label->name.valueText());
    return {};
}

struct Collector : public ASTVisitor<Collector, VisitFlags::Statements> {
    Collector(Compilation& comp, bool perInstance) : sm(*comp.getSourceManager()), perInstance(perInstance) {}

    const SourceManager& sm;
    bool perInstance;
    std::map<const syntax::SyntaxNode*, std::vector<size_t>> instancesOf; // perInstance: sites per directive
    std::vector<AssertionSite> sites;
    std::map<const syntax::SyntaxNode*, size_t> bySyntax;   // dedupe across instances
    std::map<std::string, int> unlabeledCount;
    const ProceduralBlockSymbol* currentBlock = nullptr;

    void handle(const ProceduralBlockSymbol& block) {
        auto saved = currentBlock;
        currentBlock = &block;
        visitDefault(block);
        currentBlock = saved;
    }

    void handle(const ConcurrentAssertionStatement& stmt) {
        bool moduleLevel = currentBlock && currentBlock->isFromAssertion;
        const syntax::SyntaxNode* key = moduleLevel ? currentBlock->getSyntax() : stmt.syntax;
        if (!key)
            key = stmt.syntax;

        auto body = currentBlock ? currentBlock->getParentScope()->getContainingInstance() : nullptr;
        std::string module = body ? std::string(body->getDefinition().name) : "";

        AssertionSite site;
        site.label = labelOf(key);
        site.directive = directiveName(stmt.assertionKind);
        site.module = module;
        site.loc = toLocation(sm, stmt.sourceRange.start());
        site.replaceRange = key ? key->sourceRange() : stmt.sourceRange;
        site.stmt = &stmt;
        if (body && body->parentInstance) {
            // "top.a.b" -> "a.b": paths in the flattened model are relative to the top.
            auto path = body->parentInstance->getHierarchicalPath();
            auto dot = path.find('.');
            site.instancePath = dot == std::string::npos ? "" : path.substr(dot + 1);
        }

        std::string tag = site.label;
        if (tag.empty())
            tag = "qfv_" + site.directive + "_L" + std::to_string(site.loc.line);
        std::string prefix = "qfv_" + sanitize(tag);

        if (!moduleLevel) {
            site.reason = "concurrent assertion inside a procedural block (v1.1)";
            site.reasonLoc = site.loc;
        }
        else {
            try {
                Lowerer lowerer(sm, *currentBlock, prefix);
                site.ir = lowerer.lower(stmt);
                site.ir.label = tag;
                site.supported = true;
            }
            catch (const Unsupported& u) {
                site.reason = u.reason;
                site.error = u.isError;
                site.reasonLoc = toLocation(sm, u.range.start().valid() ? u.range.start()
                                                                        : stmt.sourceRange.start());
            }
        }

        if (perInstance) {
            auto& same = instancesOf[key];
            same.push_back(sites.size());
            sites.push_back(std::move(site));
            for (size_t i : same)
                sites[i].numInstances = int(same.size());
            return;
        }
        auto it = bySyntax.find(key);
        if (it == bySyntax.end()) {
            bySyntax[key] = sites.size();
            sites.push_back(std::move(site));
            return;
        }
        // Another instance of the same module: the lowering must be identical
        // (e.g. parameter-dependent delays must agree), since there is one text.
        auto& prev = sites[it->second];
        if (prev.supported && site.supported && prev.ir.toString() != site.ir.toString()) {
            prev.supported = false;
            prev.reason = "lowering differs between instances (parameter-dependent delays)";
            prev.reasonLoc = prev.loc;
        }
        else if (prev.supported && !site.supported) {
            prev.supported = false;
            prev.reason = site.reason;
            prev.reasonLoc = site.reasonLoc;
        }
    }
};

} // namespace

std::vector<AssertionSite> collectAssertions(Compilation& compilation, bool perInstance) {
    Collector collector(compilation, perInstance);
    compilation.getRoot().visit(collector);
    return std::move(collector.sites);
}

} // namespace qfv::sva
