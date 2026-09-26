#include "sva/seq_ir.h"

namespace qfv::sva {

uint32_t Chain::maxLength() const {
    uint32_t total = 0;
    for (auto& e : elems)
        total += e.hi;
    return total;
}

static std::string delayText(uint32_t lo, uint32_t hi) {
    if (lo == hi)
        return "##" + std::to_string(lo);
    return "##[" + std::to_string(lo) + ":" + std::to_string(hi) + "]";
}

std::string Chain::toString() const {
    std::string s;
    for (size_t i = 0; i < elems.size(); i++) {
        auto& e = elems[i];
        if (i > 0 || e.hi > 0)
            s += (i ? " " : "") + delayText(e.lo, e.hi) + " ";
        s += "(" + e.leaf.text + ")";
    }
    return s;
}

std::string AssertionIR::toString() const {
    const char* k = kind == DirectiveKind::Assert   ? "assert"
                    : kind == DirectiveKind::Assume ? "assume"
                                                    : "cover";
    std::string s = std::string(k) + " @(" + clockEvent + ")";
    if (!disableText.empty())
        s += " disable iff (" + disableText + ")";
    if (kind == DirectiveKind::Cover)
        return s + " " + ante.toString();
    if (!ante.elems.empty())
        s += " " + ante.toString() + " |->";
    return s + " " + cons.toString();
}

} // namespace qfv::sva
