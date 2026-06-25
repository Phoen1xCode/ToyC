#pragma once

#include "ir/ir.hpp"

namespace toycc::ir {

// IR-level optimization passes. Mutates `program` in place.
// Currently runs (iteratively to a fixed point):
//   - basic-block local constant folding and propagation
//   - algebraic simplification (x+0, 0+x, x-0, 0-x, x*1, 1*x, x*0)
//   - local copy / copy propagation
//   - whole-function dead-code elimination for pure slot defs
void optimizeProgram(ir::Program &program);

}  // namespace toycc::ir