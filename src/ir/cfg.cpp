#include "ir/cfg.hpp"

#include <queue>

namespace toycc::ir {

CFG buildCFG(const Function &fn) {
  CFG cfg;
  if (fn.instructions.empty()) return cfg;

  // Pass 1: identify block starts. A new block starts at:
  //   - index 0 (the entry)
  //   - any Label
  //   - the instruction AFTER a control-flow instruction (Goto/Branch/Return/
  //     ReturnVoid), since control flow ends the current block
  // Without the control-flow split, a block could contain a Return in the
  // middle followed by an unreachable Goto, and the CFG would treat the Goto
  // as the block's terminator — masking the Return and producing wrong
  // fall-through edges. This bit `globalConstPropPass` on p15_ackermann: the
  // block containing `Move n <- 1` (from tailCallPass) was treated as
  // falling through to the ifend_3 block, wrongly propagating n=1.
  std::vector<std::size_t> starts;
  starts.push_back(0);
  for (std::size_t i = 1; i < fn.instructions.size(); ++i) {
    using Op = Instruction::Op;
    const auto &prev = fn.instructions[i - 1];
    if (fn.instructions[i].op == Op::Label ||
        prev.op == Op::Goto || prev.op == Op::Branch ||
        prev.op == Op::Return || prev.op == Op::ReturnVoid) {
      starts.push_back(i);
    }
  }

  // Build blocks.
  cfg.blocks.reserve(starts.size());
  for (std::size_t b = 0; b < starts.size(); ++b) {
    BasicBlock bb;
    bb.startIdx = starts[b];
    bb.endIdx = (b + 1 < starts.size()) ? starts[b + 1] - 1
                                        : fn.instructions.size() - 1;
    if (fn.instructions[bb.startIdx].op == Instruction::Op::Label) {
      bb.label = fn.instructions[bb.startIdx].label;
      cfg.labelToBlock[bb.label] = b;
    }
    cfg.blocks.push_back(bb);
  }

  // Pass 2: compute successors from each block's terminating instruction.
  auto addSucc = [&](BasicBlock &bb, const std::string &lbl) {
    auto it = cfg.labelToBlock.find(lbl);
    if (it != cfg.labelToBlock.end()) bb.succs.push_back(it->second);
  };
  for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
    BasicBlock &bb = cfg.blocks[b];
    const auto &last = fn.instructions[bb.endIdx];
    using Op = Instruction::Op;
    switch (last.op) {
      case Op::Goto:
        addSucc(bb, last.label);
        break;
      case Op::Branch:
        // Each side either jumps to its label or falls through to the next
        // block (when that side's label is empty).
        if (!last.label.empty()) addSucc(bb, last.label);
        else if (b + 1 < cfg.blocks.size()) bb.succs.push_back(b + 1);
        if (!last.falseLabel.empty()) addSucc(bb, last.falseLabel);
        else if (b + 1 < cfg.blocks.size()) bb.succs.push_back(b + 1);
        break;
      case Op::Return:
      case Op::ReturnVoid:
        break;  // no successor
      default:
        // Non-control-flow instruction at end of block: fall through.
        if (b + 1 < cfg.blocks.size()) bb.succs.push_back(b + 1);
        break;
    }
  }

  // Pass 3: compute predecessors from successors.
  for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
    for (std::size_t s : cfg.blocks[b].succs) {
      if (s < cfg.blocks.size()) cfg.blocks[s].preds.push_back(b);
    }
  }

  cfg.entryBlock = 0;
  return cfg;
}

std::unordered_set<std::size_t> reachableBlocks(const CFG &cfg) {
  std::unordered_set<std::size_t> seen;
  if (cfg.blocks.empty()) return seen;
  std::queue<std::size_t> work;
  work.push(cfg.entryBlock);
  while (!work.empty()) {
    std::size_t b = work.front();
    work.pop();
    if (b >= cfg.blocks.size() || seen.count(b)) continue;
    seen.insert(b);
    for (std::size_t s : cfg.blocks[b].succs) work.push(s);
  }
  return seen;
}

bool hasBackEdge(const CFG &cfg) {
  for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
    for (std::size_t s : cfg.blocks[b].succs) {
      if (s <= b) return true;  // back-edge to an earlier-or-same block
    }
  }
  return false;
}

}  // namespace toycc::ir
