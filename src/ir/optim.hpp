#pragma once

#include "ir/ir.hpp"

namespace toycc::ir {

// IR-level optimization passes. Mutates `program` in place.
//
// Program-level (cross-function):
//   - conservative function inlining of small, leaf, loop-free, non-recursive
//     callees (call-graph SCC guards termination); inlined constant-argument
//     bodies then fold via the local passes below.
//
// Per-function, iterated to a fixed point:
//   - basic-block local constant folding and propagation
//   - algebraic simplification (x+0, 0+x, x-0, 0-x, x*1, 1*x, x*0)
//   - local copy / copy propagation
//   - whole-function dead-code elimination for pure slot defs
//   - basic-block local common-subexpression elimination
//   - single-def/single-use copy coalescing
void optimizeProgram(ir::Program &program);

}  // namespace toycc::ir