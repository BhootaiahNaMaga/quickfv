// The reset environment: JasperGold-style `reset -expression E`, generated as a
// small SV module bound into the top (its assume becomes a design constraint).
#pragma once

#include <string>

namespace qfv {

/// Reset `resetExpr` is active for the first `cycles` clock cycles (so that
/// registers without a reset, e.g. a checker's delay pipeline, settle as they
/// do in JasperGold's multi-cycle reset analysis). Afterwards it is held
/// inactive, unless `freeAfter` (then unconstrained, as in the M0 flow).
std::string resetEnvironment(const std::string& top, const std::string& clock, const std::string& resetExpr,
                             int cycles, bool freeAfter);

} // namespace qfv
