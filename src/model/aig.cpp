#include "model/aig.h"

#include <utility>

namespace qfv {

Aig::Aig() {
    kind.push_back(Kind::Const);
    in0.push_back(0);
    in1.push_back(0);
}

Lit Aig::newInput() {
    kind.push_back(Kind::Input);
    in0.push_back(0);
    in1.push_back(0);
    return mkLit(uint32_t(kind.size() - 1));
}

Lit Aig::mkAnd(Lit a, Lit b) {
    // Constant and trivial simplifications.
    if (a == kFalse || b == kFalse || a == neg(b))
        return kFalse;
    if (a == kTrue)
        return b;
    if (b == kTrue || a == b)
        return a;
    if (a > b)
        std::swap(a, b);
    uint64_t key = (uint64_t(a) << 32) | b;
    if (auto it = strash.find(key); it != strash.end())
        return mkLit(it->second);
    uint32_t v = uint32_t(kind.size());
    kind.push_back(Kind::And);
    in0.push_back(a);
    in1.push_back(b);
    strash.emplace(key, v);
    nAnds++;
    return mkLit(v);
}

Lit Aig::mkXor(Lit a, Lit b) {
    if (a == kFalse)
        return b;
    if (b == kFalse)
        return a;
    if (a == kTrue)
        return neg(b);
    if (b == kTrue)
        return neg(a);
    if (a == b)
        return kFalse;
    if (a == neg(b))
        return kTrue;
    return mkOr(mkAnd(a, neg(b)), mkAnd(neg(a), b));
}

Lit Aig::mkIte(Lit c, Lit t, Lit e) {
    if (c == kTrue)
        return t;
    if (c == kFalse)
        return e;
    if (t == e)
        return t;
    return mkOr(mkAnd(c, t), mkAnd(neg(c), e));
}

} // namespace qfv
