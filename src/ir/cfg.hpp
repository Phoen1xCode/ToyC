#pragma once

#include "ir/ir.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toycc::ir {

// A basic block: a maximal run of instructions with no internal control flow.
// Starts at a Label (or index 0 for the entry block) and ends at (and
// includes) the next control-flow instruction (Goto/Branch/Return/ReturnVoid)
// or the instruction before the next Label.
struct BasicBlock {
  std::size_t startIdx = 0;   // index into Function::instructions
  std::size_t endIdx = 0;     // inclusive: last instruction of this block
  std::string label;          // empty if the entry block has no leading Label
  std::vector<std::size_t> preds;
  std::vector<std::size_t> succs;
};

// Control-flow graph for a single function.
struct CFG {
  std::vector<BasicBlock> blocks;
  std::unordered_map<std::string, std::size_t> labelToBlock;
  std::size_t entryBlock = 0;
};

// Build the CFG for `fn`. Every Label starts a new block (conservative —
// fall-through-only Labels are not merged with the previous block). This
// matches the dataflow-analysis convention where IN/OUT sets are computed
// per-Label. Successors are derived from the block's terminating instruction:
//   Goto X           → {X}
//   Branch l, f      → {l (if non-empty), f (if non-empty), next-block
//                       (for each empty side, as fall-through)}
//   Return/ReturnVoid → {}
//   other            → {next-block (fall-through)}
// Predecessors are the reverse.
CFG buildCFG(const Function &fn);

// Return the set of block indices reachable from the entry via BFS.
// Useful for dead-block elimination.
std::unordered_set<std::size_t> reachableBlocks(const CFG &cfg);

// True if the CFG has a back-edge (a successor index ≤ block index), which is
// a conservative loop indicator. Used by the inliner to skip functions with
// loops.
bool hasBackEdge(const CFG &cfg);

}  // namespace toycc::ir
