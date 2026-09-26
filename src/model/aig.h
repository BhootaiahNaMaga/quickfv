// And-Inverter Graph with structural hashing. Every bit of the design becomes
// a literal in this graph; word-level operations are built from mkAnd.
//
// A literal is 2*var + negated. Variable 0 is the constant, so literal 0 is
// false and literal 1 is true.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace qfv {

using Lit = uint32_t;
constexpr Lit kFalse = 0;
constexpr Lit kTrue = 1;

inline Lit neg(Lit l) { return l ^ 1; }
inline uint32_t varOf(Lit l) { return l >> 1; }
inline bool isNeg(Lit l) { return l & 1; }
inline Lit mkLit(uint32_t var, bool negated = false) { return (var << 1) | Lit(negated); }

class Aig {
public:
    enum class Kind : uint8_t { Const, Input, And };

    Aig();

    Lit newInput();
    Lit mkAnd(Lit a, Lit b);
    Lit mkOr(Lit a, Lit b) { return neg(mkAnd(neg(a), neg(b))); }
    Lit mkXor(Lit a, Lit b);
    Lit mkXnor(Lit a, Lit b) { return neg(mkXor(a, b)); }
    Lit mkIte(Lit c, Lit t, Lit e);

    uint32_t numVars() const { return uint32_t(kind.size()); }
    Kind kindOf(uint32_t var) const { return kind[var]; }
    Lit fanin0(uint32_t var) const { return in0[var]; }
    Lit fanin1(uint32_t var) const { return in1[var]; }
    size_t numAnds() const { return nAnds; }

private:
    std::vector<Kind> kind;
    std::vector<Lit> in0, in1;
    std::unordered_map<uint64_t, uint32_t> strash;
    size_t nAnds = 0;
};

} // namespace qfv
