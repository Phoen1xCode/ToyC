#include "ir/optim.hpp"

#include "ir/cfg.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toycc::ir {
namespace {

using Inst = ir::Instruction;
using Op = ir::Instruction::Op;

bool definesSlot(const Inst &i) {
  switch (i.op) {
    case Op::Const:
    case Op::Move:
    case Op::LoadGlobal:
    case Op::Unary:
    case Op::Binary:
      return true;
    default:
      return false;
  }
}

// Returns slot IDs read by inst (may include duplicates).
std::vector<int> readsOf(const Inst &i) {
  std::vector<int> out;
  switch (i.op) {
    case Op::Move:
    case Op::StoreGlobal:
    case Op::Unary:
    case Op::Branch:
    case Op::Return:
      if (i.lhs != -1) out.push_back(i.lhs);
      break;
    case Op::Binary:
      if (i.lhs != -1) out.push_back(i.lhs);
      if (i.rhs != -1) out.push_back(i.rhs);
      break;
    case Op::Call:
      for (int a : i.args)
        if (a != -1) out.push_back(a);
      break;
    default:
      break;
  }
  return out;
}

bool isControlBoundary(const Inst &i) {
  return i.op == Op::Label || i.op == Op::Goto || i.op == Op::Branch ||
         i.op == Op::Return || i.op == Op::ReturnVoid;
}

// Whole-function use-count of slots.
std::unordered_map<int, int> computeUseCount(const ir::Function &fn) {
  std::unordered_map<int, int> uses;
  for (const auto &i : fn.instructions)
    for (int s : readsOf(i)) ++uses[s];
  return uses;
}

// Dead-code elimination of pure slot-defining instructions whose dest is never
// read anywhere in the function (or whose only reader was already removed).
bool dcePass(ir::Function &fn) {
  bool any = false;
  bool changed = true;
  while (changed) {
    changed = false;
    const auto uses = computeUseCount(fn);
    std::vector<Inst> kept;
    kept.reserve(fn.instructions.size());
    for (const auto &i : fn.instructions) {
      if (definesSlot(i)) {
        const auto it = uses.find(i.dest);
        if (it == uses.end() || it->second == 0) {
          changed = true;
          any = true;
          continue;  // drop
        }
      }
      kept.push_back(i);
    }
    fn.instructions = std::move(kept);
  }
  return any;
}

int foldConstUnary(ir::UnaryOp op, int v) {
  switch (op) {
    case ir::UnaryOp::Plus: return v;
    case ir::UnaryOp::Minus: return -v;
    case ir::UnaryOp::Not: return v == 0 ? 1 : 0;
  }
  return v;
}

int foldConstBinary(ir::BinaryOp op, int a, int b) {
  switch (op) {
    case ir::BinaryOp::Less: return a < b ? 1 : 0;
    case ir::BinaryOp::Greater: return a > b ? 1 : 0;
    case ir::BinaryOp::LessEqual: return a <= b ? 1 : 0;
    case ir::BinaryOp::GreaterEqual: return a >= b ? 1 : 0;
    case ir::BinaryOp::Equal: return a == b ? 1 : 0;
    case ir::BinaryOp::NotEqual: return a != b ? 1 : 0;
    case ir::BinaryOp::Add: return a + b;
    case ir::BinaryOp::Sub: return a - b;
    case ir::BinaryOp::Mul: return a * b;
    case ir::BinaryOp::Div: return b == 0 ? 0 : a / b;  // UB-guarded (judged programs avoid this)
    case ir::BinaryOp::Mod: return b == 0 ? 0 : a % b;
  }
  return 0;
}

// Basic-block local constant folding + algebraic simplification.
// Returns true if any IR instruction was rewritten.
bool constFoldPass(ir::Function &fn) {
  bool any = false;
  std::unordered_map<int, int> known;
  auto clearDef = [&](int slot) {
    if (slot == -1) return;
    auto it = known.find(slot);
    if (it != known.end()) known.erase(it);
  };
  for (auto &i : fn.instructions) {
    if (i.op == Op::Label) known.clear();

    switch (i.op) {
      case Op::Const:
        known[i.dest] = i.value;
        break;
      case Op::Move: {
        clearDef(i.dest);
        auto it = known.find(i.lhs);
        if (it != known.end()) {
          int v = it->second;
          i.op = Op::Const;
          i.value = v;
          i.lhs = -1;
          known[i.dest] = v;
          any = true;
        }
        break;
      }
      case Op::Unary: {
        clearDef(i.dest);
        auto it = known.find(i.lhs);
        if (it != known.end()) {
          int v = foldConstUnary(i.unary, it->second);
          i.op = Op::Const;
          i.value = v;
          i.unary = ir::UnaryOp::Plus;
          i.lhs = -1;
          known[i.dest] = v;
          any = true;
        }
        break;
      }
      case Op::Binary: {
        clearDef(i.dest);
        auto lk = known.find(i.lhs);
        auto rk = known.find(i.rhs);
        const bool lc = (lk != known.end());
        const bool rc = (rk != known.end());
        if (lc && rc) {
          int v = foldConstBinary(i.binary, lk->second, rk->second);
          i.op = Op::Const;
          i.value = v;
          i.binary = ir::BinaryOp::Add;
          i.lhs = i.rhs = -1;
          known[i.dest] = v;
          any = true;
          break;
        }
        // algebraic simplification using whichever operand is known const
        switch (i.binary) {
          case ir::BinaryOp::Add: {
            if (lc && lk->second == 0) {  // 0 + r -> r
              i.op = Op::Move; i.lhs = i.rhs; i.rhs = -1; any = true; break;
            }
            if (rc && rk->second == 0) {  // l + 0 -> l
              i.op = Op::Move; i.rhs = -1; any = true; break;
            }
            break;
          }
          case ir::BinaryOp::Sub: {
            if (rc && rk->second == 0) {  // l - 0 -> l
              i.op = Op::Move; i.rhs = -1; any = true; break;
            }
            if (lc && lk->second == 0) {  // 0 - r -> (-r)
              i.op = Op::Unary;
              i.unary = ir::UnaryOp::Minus;
              i.lhs = i.rhs;
              i.rhs = -1;
              any = true; break;
            }
            break;
          }
          case ir::BinaryOp::Mul: {
            if ((lc && lk->second == 0) || (rc && rk->second == 0)) {  // x*0 -> 0
              i.op = Op::Const; i.value = 0; i.binary = ir::BinaryOp::Add; i.lhs = i.rhs = -1; any = true; break;
            }
            if (lc && lk->second == 1) {  // 1 * r -> r
              i.op = Op::Move; i.lhs = i.rhs; i.rhs = -1; any = true; break;
            }
            if (rc && rk->second == 1) {  // l * 1 -> l
              i.op = Op::Move; i.rhs = -1; any = true; break;
            }
            break;
          }
          case ir::BinaryOp::Less:
          case ir::BinaryOp::LessEqual:
          case ir::BinaryOp::Greater:
          case ir::BinaryOp::GreaterEqual:
          case ir::BinaryOp::Equal:
          case ir::BinaryOp::NotEqual:
          case ir::BinaryOp::Div:
          case ir::BinaryOp::Mod:
            break;
        }
        break;
      }
      case Op::LoadGlobal:
        clearDef(i.dest);
        break;
      case Op::Call:
        clearDef(i.dest);
        break;
      case Op::Goto:
      case Op::Branch:
      case Op::Return:
      case Op::ReturnVoid:
        known.clear();
        break;
      default:
        break;
    }
  }
  return any;
}

// Fold `Branch lhs=known-const` into `Goto target` (or delete if fall-through).
// Tracks BB-local known constants the same way constFoldPass does; runs after
// constFoldPass so that `Move d <- s` (s const) has already been rewritten to
// `Const d=v`, making more Branch conditions locally-constant.
//
// Branch semantics (from backend emitInst): `bnez a0, label` then
// `beqz a0, falseLabel`. Either label may be empty (fall-through). For the
// while-loop pattern `emitBranch(cond, endLabel)` with onTrue=false, label is
// empty and falseLabel is endLabel — "if cond is false, jump to end; else
// fall through to body".
//
// When the condition is known:
//   v == 0 (false): take falseLabel; if falseLabel empty, fall through (delete).
//   v != 0 (true):  take label;       if label empty,       fall through (delete).
bool branchFoldPass(ir::Function &fn) {
  bool any = false;
  std::unordered_map<int, int> known;
  std::vector<Inst> kept;
  kept.reserve(fn.instructions.size());
  auto clearDef = [&](int slot) {
    if (slot != -1) known.erase(slot);
  };
  for (auto &i : fn.instructions) {
    if (i.op == Op::Label) {
      known.clear();
      kept.push_back(i);
      continue;
    }
    if (i.op == Op::Branch && i.lhs != -1) {
      auto it = known.find(i.lhs);
      if (it != known.end()) {
        int v = it->second;
        const std::string &target = (v == 0) ? i.falseLabel : i.label;
        if (!target.empty()) {
          Inst g;
          g.op = Op::Goto;
          g.label = target;
          kept.push_back(g);
        }
        // else: fall-through — drop the Branch entirely.
        any = true;
        known.clear();
        continue;
      }
      known.clear();
      kept.push_back(i);
      continue;
    }
    // Update known (mirrors constFoldPass; only the slots, not the rewrites).
    switch (i.op) {
      case Op::Const:
        known[i.dest] = i.value;
        break;
      case Op::Move: {
        clearDef(i.dest);
        auto it = known.find(i.lhs);
        if (it != known.end()) known[i.dest] = it->second;
        break;
      }
      case Op::Unary: {
        clearDef(i.dest);
        auto it = known.find(i.lhs);
        if (it != known.end())
          known[i.dest] = foldConstUnary(i.unary, it->second);
        break;
      }
      case Op::Binary: {
        clearDef(i.dest);
        auto lk = known.find(i.lhs);
        auto rk = known.find(i.rhs);
        if (lk != known.end() && rk != known.end())
          known[i.dest] = foldConstBinary(i.binary, lk->second, rk->second);
        break;
      }
      case Op::LoadGlobal:
      case Op::Call:
        clearDef(i.dest);
        break;
      case Op::Goto:
      case Op::Return:
      case Op::ReturnVoid:
        known.clear();
        break;
      default:
        break;
    }
    kept.push_back(i);
  }
  if (any) fn.instructions = std::move(kept);
  return any;
}

// Remove unreachable basic blocks via CFG reachability from the function entry.
// A block is reachable if it's the entry (instruction 0), the target of a
// Goto/Branch, OR reached by fall-through from a Branch with an empty side
// (or from any non-control-flow instruction). The prior implementation only
// counted Goto/Branch targets, which missed fall-through-reached blocks like
// `while_body` (the Branch's true-side is fall-through when label is empty),
// causing the entire loop body to be dropped.
bool deadBlockPass(ir::Function &fn) {
  if (fn.instructions.empty()) return false;
  std::unordered_map<std::string, std::size_t> labelPos;
  for (std::size_t i = 0; i < fn.instructions.size(); ++i)
    if (fn.instructions[i].op == Op::Label)
      labelPos[fn.instructions[i].label] = i;
  std::vector<bool> reachable(fn.instructions.size(), false);
  std::queue<std::size_t> worklist;
  worklist.push(0);
  while (!worklist.empty()) {
    std::size_t idx = worklist.front();
    worklist.pop();
    if (idx >= fn.instructions.size() || reachable[idx]) continue;
    reachable[idx] = true;
    const auto &inst = fn.instructions[idx];
    auto pushLabel = [&](const std::string &lbl) {
      auto it = labelPos.find(lbl);
      if (it != labelPos.end()) worklist.push(it->second);
    };
    switch (inst.op) {
      case Op::Goto:
        pushLabel(inst.label);
        break;
      case Op::Branch:
        // Each side either jumps to its label or falls through to idx+1.
        if (!inst.label.empty()) pushLabel(inst.label);
        else worklist.push(idx + 1);
        if (!inst.falseLabel.empty()) pushLabel(inst.falseLabel);
        else worklist.push(idx + 1);
        break;
      case Op::Return:
      case Op::ReturnVoid:
        break;
      default:
        worklist.push(idx + 1);
        break;
    }
  }
  std::vector<Inst> kept;
  bool any = false;
  for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
    if (reachable[i]) kept.push_back(fn.instructions[i]);
    else any = true;
  }
  if (any) fn.instructions = std::move(kept);
  return any;
}

// Drop `Goto X` immediately followed by `Label X` (jump to the next
// instruction is a no-op). Common after branchFoldPass converts a Branch to
// `Goto falseLabel` where falseLabel happens to be the next Label.
bool gotoCleanupPass(ir::Function &fn) {
  bool any = false;
  std::vector<Inst> kept;
  kept.reserve(fn.instructions.size());
  for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
    const auto &inst = fn.instructions[i];
    if (inst.op == Op::Goto && i + 1 < fn.instructions.size() &&
        fn.instructions[i + 1].op == Op::Label &&
        fn.instructions[i + 1].label == inst.label) {
      any = true;
      continue;
    }
    kept.push_back(inst);
  }
  if (any) fn.instructions = std::move(kept);
  return any;
}

// Basic-block local copy propagation. When `Move d <- s`, subsequent reads of d
// within the same basic block can substitute s. Aliases become stale when a
// affected slot is redefined. Returns true if any operand was rewritten.
bool copyPropPass(ir::Function &fn) {
  bool any = false;
  std::unordered_map<int, int> alias;  // slot d -> slot s (a substitute)
  auto subst = [&](int s) -> int {
    if (s == -1) return s;
    // follow chain
    int cur = s;
    while (true) {
      auto it = alias.find(cur);
      if (it == alias.end()) break;
      if (it->second == s) {  // cycle break (shouldn't happen)
        break;
      }
      cur = it->second;
      if (cur == s) break;
    }
    return cur == s ? s : cur;
  };
  // Strip any aliases mapping where value side points to a freshly redefined slot
  auto invalidate = [&](int slot) {
    if (slot == -1) return;
    for (auto it = alias.begin(); it != alias.end();) {
      if (it->second == slot) it = alias.erase(it);
      else ++it;
    }
    alias.erase(slot);
  };

  for (auto &i : fn.instructions) {
    if (i.op == Op::Label) alias.clear();

    switch (i.op) {
      case Op::Move: {
        int src = subst(i.lhs);
        if (src != i.lhs) any = true;
        i.lhs = src;
        invalidate(i.dest);
        if (i.dest != src) alias[i.dest] = src;
        break;
      }
      case Op::Unary: {
        int s = subst(i.lhs);
        if (s != i.lhs) any = true;
        i.lhs = s;
        invalidate(i.dest);
        break;
      }
      case Op::Binary: {
        int l = subst(i.lhs);
        int r = subst(i.rhs);
        if (l != i.lhs) any = true;
        if (r != i.rhs) any = true;
        i.lhs = l; i.rhs = r;
        invalidate(i.dest);
        break;
      }
      case Op::Const:
      case Op::LoadGlobal:
      case Op::Call:
        invalidate(i.dest);
        break;
      case Op::StoreGlobal:
      case Op::Branch:
      case Op::Return: {
        int s = subst(i.lhs);
        if (s != i.lhs) any = true;
        i.lhs = s;
        break;
      }
      case Op::Goto:
      case Op::ReturnVoid:
        alias.clear();
        break;
      default:
        break;
    }
    if (i.op == Op::Branch || i.op == Op::Return) alias.clear();
  }
  return any;
}

// Basic-block local common-subexpression elimination. Reuses a previous
// identical Binary's dest when same op & operands appear within a block.
// Operands are compared by either slot id OR (when the slot is known to hold
// a constant in this block) by constant value — so `i*3` and `i*3` match even
// when the two `3`s were materialized into different slots by IR construction.
bool csePass(ir::Function &fn) {
  bool any = false;
  for (auto itbeg = fn.instructions.begin(); itbeg != fn.instructions.end();) {
    std::unordered_map<int, int> known;  // slot -> constant value
    // seen: key -> {dest, opSlotA, opSlotB}. opSlotA/opSlotB are the raw slot
    // ids used as operands (-1 if the operand was a known constant at encode
    // time). When a slot is redefined we drop every seen entry that still
    // references it, so a later identical (op, operand) pattern is not wrongly
    // rewritten to a stale value.
    struct SeenEntry { int dest; int opA; int opB; };
    std::unordered_map<uint64_t, SeenEntry> seen;
    auto invalidate = [&](int slot) {
      if (slot == -1) return;
      for (auto it = seen.begin(); it != seen.end();)
        if (it->second.opA == slot || it->second.opB == slot) it = seen.erase(it);
        else ++it;
    };
    auto cur = itbeg;
    while (cur != fn.instructions.end() && !isControlBoundary(*cur)) {
      if (cur->op == Op::Label) break;
      auto &inst = *cur;
      // Const defines its dest as a known constant. If this slot was
      // previously used as a raw operand in any seen entry, those entries are
      // now stale (the slot's value changed from "unknown" to a constant).
      if (inst.op == Op::Const && inst.dest != -1) {
        invalidate(inst.dest);
        known[inst.dest] = inst.value;
      }
      auto enc = [&](int slot, int &rawSlot) -> int {
        rawSlot = -1;
        auto it = known.find(slot);
        if (it == known.end()) { rawSlot = slot; return slot; }
        return 0x40000000 | (it->second & 0x3FFFFF);
      };
      if (inst.op == Op::Binary) {
        // Self-update (dest == lhs or dest == rhs) cannot be CSE'd: the dest
        // redefines an operand, so identical keys yield different values.
        const bool selfUpdate = (inst.dest == inst.lhs) || (inst.dest == inst.rhs);
        if (!selfUpdate) {
          int ra, rb;
          auto op = static_cast<int>(inst.binary);
          int a = enc(inst.lhs, ra);
          int b = enc(inst.rhs, rb);
          bool commute = (inst.binary == ir::BinaryOp::Add) || (inst.binary == ir::BinaryOp::Mul);
          if (commute && a > b) { std::swap(a, b); std::swap(ra, rb); }
          uint64_t key =
              (uint64_t)(op & 0xFFFF) << 48 | (uint64_t)(a & 0xFFFFFFFF) << 16 | (b & 0xFFFF);
          auto it = seen.find(key);
          if (it != seen.end()) {
            int prev = it->second.dest;
            inst.op = Op::Move;
            inst.lhs = prev;
            inst.rhs = -1;
            inst.binary = ir::BinaryOp::Add;
            any = true;
          } else {
            seen[key] = {inst.dest, ra, rb};
          }
        }
        // The Binary defines inst.dest — invalidate any seen entry that used
        // the old value of inst.dest.
        invalidate(inst.dest);
      } else if (definesSlot(inst) && inst.dest != -1) {
        // Move/Unary/LoadGlobal redefine inst.dest.
        invalidate(inst.dest);
      }
      ++cur;
    }
    itbeg = (cur == fn.instructions.end()) ? cur : cur + 1;
  }
  return any;
}

// Copy-coalesce: when `Move d <- s` immediately follows a pure def of s, and s
// is used exactly once (this Move), re-target the def to write d directly.
//
// Soundness condition: the def must immediately precede the Move (no
// intervening instruction may read or write d, otherwise advancing the
// store-to-d corrupts the value an in-between read of d expected). Counter-
// example caught by p19_gcd_pairs: `t = a - (a/b)*b; a = b; b = t;` — here
// the binary defining t is two Moves before `Move b <- t`, with `Move a <- b`
// in between. Coalescing t into b would store t into b before `a = b` reads
// b, breaking the swap idiom.
bool copyCoalescePass(ir::Function &fn) {
  bool any = false;
  bool changed = true;
  while (changed) {
    changed = false;
    std::unordered_map<int,int> uses = computeUseCount(fn);
    // single def site (by index) for each slot, or size_t(-1) if multiple defs.
    std::unordered_map<int, std::size_t> singleDef;
    for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
      const auto &inst = fn.instructions[i];
      if (!definesSlot(inst)) continue;
      auto it = singleDef.find(inst.dest);
      if (it == singleDef.end()) singleDef.emplace(inst.dest, i);
      else it->second = static_cast<std::size_t>(-1);
    }
    // Pass 1: pick coalescable moves (s -> d). Record one re-target per s.
    std::unordered_map<int, int> retarget;  // s -> d
    for (std::size_t mi = 0; mi < fn.instructions.size(); ++mi) {
      const auto &inst = fn.instructions[mi];
      if (inst.op != Op::Move) continue;
      const int s = inst.lhs;
      const int d = inst.dest;
      if (s == -1 || d == s) continue;
      auto uit = uses.find(s);
      auto dit = singleDef.find(s);
      if (uit == uses.end() || uit->second != 1) continue;
      if (dit == singleDef.end() || dit->second == static_cast<std::size_t>(-1)) continue;
      // Adjacency requirement: the def must be the instruction right before
      // this Move, so nothing reads or writes d in between.
      if (dit->second + 1 != mi) continue;
      const Inst &def = fn.instructions[dit->second];
      if (def.op != Op::Const && def.op != Op::Unary &&
          def.op != Op::Binary && def.op != Op::LoadGlobal) continue;
      retarget[s] = d;
    }
    if (retarget.empty()) break;
    // Pass 2: rebuild. Defs of s become defs of d. Drop the Move.
    std::vector<Inst> kept;
    kept.reserve(fn.instructions.size());
    for (auto &inst : fn.instructions) {
      if (inst.op == Op::Move) {
        auto it = retarget.find(inst.lhs);
        if (it != retarget.end()) { changed = true; any = true; continue; }
      }
      if (definesSlot(inst)) {
        auto it = retarget.find(inst.dest);
        if (it != retarget.end()) inst.dest = it->second;
      }
      kept.push_back(inst);
    }
    fn.instructions = std::move(kept);
  }
  return any;
}

// Compute the set of "pure" function names in a program. A function is pure
// iff it has no StoreGlobal and calls no impure function (transitively). Pure
// functions don't modify global state, so a Call to a pure function doesn't
// invalidate loop-invariant LoadGlobals the way an impure Call might.
//
// Fixed-point iteration: start with all functions pure, then iteratively
// mark impure any function that has a StoreGlobal or calls an impure
// function. Recursion is handled implicitly — a self-recursive function that
// writes a global will be marked impure on the first sweep.
std::unordered_set<std::string> computePureFunctions(
    const std::vector<ir::Function> &fns) {
  std::unordered_set<std::string> pure;
  for (const auto &f : fns) pure.insert(f.name);
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &f : fns) {
      if (!pure.count(f.name)) continue;
      bool impure = false;
      for (const auto &inst : f.instructions) {
        if (inst.op == Op::StoreGlobal) { impure = true; break; }
        if (inst.op == Op::Call && !pure.count(inst.name)) {
          impure = true;
          break;
        }
      }
      if (impure) {
        pure.erase(f.name);
        changed = true;
      }
    }
  }
  return pure;
}

// ---------------------------------------------------------------------------
// Loop-invariant code motion (LICM) for `while` loops.
//
// The IR builder lowers `while` to a fixed label pattern:
//     .L_<fn>_while_cond_N:  <cond>; Branch cond, end
//     .L_<fn>_while_body_N:  <body>; Goto cond
//     .L_<fn>_while_end_N:
// We identify each loop by scanning for `while_cond` labels and pairing them
// with the matching `while_end` label carried in the Branch's falseLabel.
//
// A pure defining instruction inside the loop body is invariant and can be
// hoisted to just before the cond label iff:
//   (a) it is a pure slot def (Const/Move/Unary/Binary) — side-effecting ops
//       (Call/StoreGlobal/Branch/Goto/Return) stay put;
//   (b) every slot it reads is loop-invariant: defined zero times inside the
//       loop body (it comes from outside the loop, or is a function param);
//   (c) the slot it defines is defined exactly once inside the loop body
//       (this instruction) — so hoisting does not strand another in-loop def
//       that downstream reads expect.
// This excludes induction variables (`i = i + 1`: `i` is both read and
// re-defined in the loop) and accumulators (`s = s + inv`: `s` re-defined).
// It *does* hoist `inv = base*3 + base` (base from outside) and `t = a + b`
// where a, b are loop-invariant params — exactly the hoist-1 pattern.
//
// LoadGlobal is treated as invariant iff no StoreGlobal to the SAME global
// occurs in the loop body (per-global alias check — a StoreGlobal to a
// different global doesn't invalidate a LoadGlobal of this global).
// Calls inside the loop block hoisting of anything after an IMPURE call (a
// callee that may write globals). A pure callee (no StoreGlobal, no impure
// Call) doesn't write globals, so it doesn't block subsequent LoadGlobal
// hoisting. Purity is precomputed at the program level by
// `computePureFunctions` and passed in.
//
// We hoist one loop per outer iteration (positions shift after a rebuild),
// iterating to a fixed point.
bool licmPass(ir::Function &fn,
              const std::unordered_set<std::string> &pureFunctions) {
  bool any = false;
  bool changed = true;
  while (changed) {
    changed = false;

    for (std::size_t ci = 0; ci < fn.instructions.size(); ++ci) {
      if (fn.instructions[ci].op != Op::Label) continue;
      if (fn.instructions[ci].label.find("_while_cond_") == std::string::npos) continue;
      // Find the matching `_while_end_` Label by scanning forward with nesting
      // tracking. Robust to the while-condition Branch being deleted (e.g., by
      // branchFoldPass folding `while (1)` to fall-through). The prior approach
      // took the first Branch's falseLabel as endLab, which broke when that
      // Branch was removed — LICM would then find an if-condition Branch
      // inside the body and misidentify the loop range, hoisting non-invariant
      // instructions like `i >= N` to before the loop.
      std::size_t endIdx = std::size_t(-1);
      int nesting = 0;
      for (std::size_t j = ci + 1; j < fn.instructions.size(); ++j) {
        const auto &x = fn.instructions[j];
        if (x.op != Op::Label) continue;
        const std::string &lbl = x.label;
        if (lbl.find("_while_cond_") != std::string::npos) {
          ++nesting;
        } else if (lbl.find("_while_end_") != std::string::npos) {
          if (nesting == 0) { endIdx = j; break; }
          --nesting;
        }
      }
      if (endIdx == std::size_t(-1)) continue;
      if (endIdx <= ci + 1) continue;
      std::size_t bodyIdx = ci + 1;
      for (std::size_t j = ci + 1; j < endIdx; ++j) {
        if (fn.instructions[j].op == Op::Label &&
            fn.instructions[j].label.find("_while_body_") != std::string::npos) {
          bodyIdx = j; break;
        }
      }
      if (bodyIdx <= ci) continue;
      const std::size_t bodyStart = bodyIdx + 1;
      const std::size_t bodyEnd = endIdx;

      std::unordered_map<int, int> defCount;
      std::unordered_set<std::string> storedGlobals;
      for (std::size_t j = bodyStart; j < bodyEnd; ++j) {
        const auto &ins = fn.instructions[j];
        if (ins.op == Op::StoreGlobal) storedGlobals.insert(ins.name);
        // Count Call dests explicitly: `definesSlot` excludes Call (so dcePass
        // doesn't try to DCE Calls), but for LICM's invariant analysis a Call
        // DOES define its dest slot. Without this, a pure Call's dest would
        // have defCount=0 and be wrongly treated as invariant, causing
        // instructions that read it to be hoisted (the Call result changes
        // every iteration even if the args are invariant).
        if (ins.op == Op::Call) {
          if (ins.dest != -1) ++defCount[ins.dest];
        } else if (definesSlot(ins) && ins.dest != -1) {
          ++defCount[ins.dest];
        }
      }
      // Soundness guard: a hoisted store to slot D is only safe if the loop
      // is guaranteed to execute ≥1 iteration. Without trip-count analysis
      // we conservatively refuse to hoist any instruction whose dest is read
      // AFTER the loop (between endIdx and the function's end). Such a slot
      // is "live out" of the loop — hoisting a `Const D = ...` or `Binary D =`
      // would change D's value on zero-trip paths. Reads inside the loop are
      // fine (the hoisted def still dominates them); the bug is when the
      // hoisted value persists past `while (0) { D = 1; }` and a subsequent
      // read sees 1 instead of the pre-loop value.
      std::unordered_set<int> liveOutSlots;
      for (std::size_t j = endIdx + 1; j < fn.instructions.size(); ++j) {
        for (int s : readsOf(fn.instructions[j])) liveOutSlots.insert(s);
      }
      std::unordered_set<int> invariant;
      auto isInvariantSlot = [&](int s) {
        auto it = defCount.find(s);
        return it == defCount.end() || it->second == 0 || invariant.count(s);
      };

      std::vector<bool> hoist(fn.instructions.size(), false);
      bool sawImpureCall = false;
      for (std::size_t j = bodyStart; j < bodyEnd; ++j) {
        const auto &ins = fn.instructions[j];
        if (ins.op == Op::Call) {
          if (!pureFunctions.count(ins.name)) sawImpureCall = true;
          continue;
        }
        if (!definesSlot(ins) || ins.dest == -1) continue;
        if (sawImpureCall) continue;
        if (defCount[ins.dest] != 1) continue;
        if (ins.op == Op::LoadGlobal && storedGlobals.count(ins.name)) continue;
        // Refuse to hoist a def whose slot is read after the loop — hoisting
        // would corrupt the zero-trip path. (`Const`/`Binary`/`Unary`/`Move`
        // all write to dest; `LoadGlobal` too.) Pure-computation hoists where
        // the dest is dead after the loop remain safe and are still hoisted.
        if (liveOutSlots.count(ins.dest)) continue;
        bool ok = true;
        for (int s : readsOf(ins))
          if (!isInvariantSlot(s)) { ok = false; break; }
        if (!ok) continue;
        hoist[j] = true;
        defCount[ins.dest] = 0;
        invariant.insert(ins.dest);
        if (getenv("TOYCC_DUMP_LICM")) {
          fprintf(stderr, "  HOIST [%zu] op=%d dest=%d in %s (cond=%s)\n",
              j, (int)ins.op, ins.dest, fn.name.c_str(),
              fn.instructions[ci].label.c_str());
        }
      }

      bool hoistedAny = false;
      for (std::size_t j = bodyStart; j < bodyEnd; ++j)
        if (hoist[j]) { hoistedAny = true; break; }
      if (!hoistedAny) continue;

      std::vector<Inst> out;
      out.reserve(fn.instructions.size());
      for (std::size_t j = 0; j < ci; ++j) out.push_back(fn.instructions[j]);
      for (std::size_t j = bodyStart; j < bodyEnd; ++j)
        if (hoist[j]) out.push_back(fn.instructions[j]);
      for (std::size_t j = ci; j < bodyStart; ++j) out.push_back(fn.instructions[j]);
      for (std::size_t j = bodyStart; j < bodyEnd; ++j)
        if (!hoist[j]) out.push_back(fn.instructions[j]);
      for (std::size_t j = bodyEnd; j < fn.instructions.size(); ++j) out.push_back(fn.instructions[j]);
      fn.instructions = std::move(out);
      changed = true;
      any = true;
      break;
    }
  }
  return any;
}

// ---------------------------------------------------------------------------
// Loop summation elimination (closed-form induction-accumulator folding).
//
// Detects the canonical pattern
//     s = s_init;  i = i_init;
//     while (i < N) {            // or <=, >, >=
//         s = s + addend;         // addend affine in i:  a*i + c  (a,c const)
//         i = i + step;           // step nonzero const (typically 1)
//     }
// and replaces the whole loop with the closed-form
//     s = s_init + Σ_{k=0..T-1} (a*(i_init + k*step) + c)
//       = s_init + T*(a*i_init + c) + a*step*T*(T-1)/2
// where T is the compile-time trip count. This is the optimization gcc -O2
// uses to turn `for(i=0;i<N;i++) s+=c; return s%256;` into `return const;` —
// without it our O(N) loop runs 30-100× longer than gcc's O(1) code on real
// hardware, which is the root cause of p07 (3%) / p08 (1%) / p02 (9%).
//
// Soundness:
//  - ToyC `int` is 32-bit signed; test programs have no UB, so the iterative
//    sum never overflows 32-bit. Computing the closed form in int64 and
//    truncating to int32 therefore matches exactly.
//  - Guard: if T < 0, T > 3e9, or any int64 intermediate overflows → skip.
//  - The body must be exactly {optional addend-temp Binary, accumulator
//    Binary, induction Binary} — no Call/StoreGlobal/Branch/Goto/internal
//    Label. This rejects loops with if/break/continue/nested loops (p09,
//    lv7/lv8 functional tests) so they keep their original code.
//  - `s` must not be read in the body except as its own update lhs. `i` and
//    `s` each defined exactly once in the body.
//  - `i_init`, `step`, `N` must all resolve to compile-time constants.
//  - `s_init` must resolve to a compile-time constant (else skip — the common
//    perf cases all init `s = 0`).
//  - Zero-trip loops (i_init already past N) → T=0, s stays s_init, i stays
//    i_init; we still emit the (trivial) Consts and delete the loop.
// ---------------------------------------------------------------------------
namespace {
// Backward scan for the most recent def of `slot` before `beforeIdx` that is
// either a Const (returns its value) or a Move from a slot that itself resolves
// to a Const (one level of indirection). Returns {found, value}.
struct ConstRes { bool ok; long long v; };
ConstRes resolveInitConst(const ir::Function &fn, int slot, std::size_t beforeIdx) {
  for (std::size_t j = beforeIdx; j-- > 0;) {
    const auto &ins = fn.instructions[j];
    if (ins.dest != slot) continue;
    if (ins.op == Op::Const) return {true, ins.value};
    if (ins.op == Op::Move) {
      // one level of Move-from-Const
      for (std::size_t k = j; k-- > 0;) {
        const auto &m = fn.instructions[k];
        if (m.dest != ins.lhs) continue;
        if (m.op == Op::Const) return {true, m.value};
        break;
      }
    }
    return {false, 0};  // some other def kind, give up
  }
  return {false, 0};
}

// ceil(a/b) for b>0, a any integer. Returns 0 when a<=0.
long long ceilDivPos(long long a, long long b) {
  if (a <= 0) return 0;
  return (a + b - 1) / b;
}

// loopSumElimPass helpers. `bad` follows the pass's illFormed convention:
// bad=true means "overflowed / rejected"; bad=false means "still valid".
long long safeMulStat(long long x, long long y, bool &bad) {
  if (bad) return 0;
  if (x == 0 || y == 0) return 0;
  if (x > 0) {
    if (y > 0 ? x > (LLONG_MAX / y) : y < (LLONG_MIN / x)) bad = true;
  } else {
    if (y > 0 ? x < (LLONG_MIN / y) : y < (LLONG_MAX / x)) bad = true;
  }
  if (bad) return 0;
  return x * y;
}
long long trunc32Stat(long long x, bool &bad) {
  if (!bad && (x > INT32_MAX || x < INT32_MIN)) bad = true;
  if (bad) return 0;
  return static_cast<long long>(static_cast<std::uint32_t>(x));
}
}  // namespace

bool loopSumElimPass(ir::Function &fn) {
  bool any = false;
  bool changed = true;
  while (changed) {
    changed = false;

    // Single-def Const slots → known constant values (function-wide).
    std::unordered_map<int, int> defCount;
    std::unordered_map<int, long long> constVal;
    for (const auto &ins : fn.instructions) {
      if (definesSlot(ins) && ins.dest != -1) {
        ++defCount[ins.dest];
        if (ins.op == Op::Const) constVal[ins.dest] = ins.value;
        else constVal.erase(ins.dest);
      }
    }
    std::unordered_map<int, long long> globalConst;
    for (const auto &kv : defCount)
      if (kv.second == 1) {
        auto it = constVal.find(kv.first);
        if (it != constVal.end()) globalConst[kv.first] = it->second;
      }
    auto constOf = [&](int slot) -> ConstRes {
      auto it = globalConst.find(slot);
      if (it != globalConst.end()) return {true, it->second};
      return {false, 0};
    };

    // tryElim: attempt to close-form the loop whose `_while_cond_` label is at
    // `ci`. Returns true and mutates `fn` (replacing the loop with Consts) on
    // success; returns false (no change) on any rejection.
    auto tryElim = [&](std::size_t ci) -> bool {
      // Find matching _while_end_ with nesting (same logic as licmPass).
      std::size_t endIdx = std::size_t(-1);
      int nesting = 0;
      for (std::size_t j = ci + 1; j < fn.instructions.size(); ++j) {
        const auto &x = fn.instructions[j];
        if (x.op != Op::Label) continue;
        const std::string &lbl = x.label;
        if (lbl.find("_while_cond_") != std::string::npos) ++nesting;
        else if (lbl.find("_while_end_") != std::string::npos) {
          if (nesting == 0) { endIdx = j; break; }
          --nesting;
        }
      }
      if (endIdx == std::size_t(-1)) return false;
      std::size_t bodyIdx = std::size_t(-1);
      for (std::size_t j = ci + 1; j < endIdx; ++j) {
        if (fn.instructions[j].op == Op::Label &&
            fn.instructions[j].label.find("_while_body_") != std::string::npos) {
          bodyIdx = j; break;
        }
      }
      if (bodyIdx == std::size_t(-1)) return false;
      const std::size_t condStart = ci + 1;
      const std::size_t bodyStart = bodyIdx + 1;
      const std::size_t bodyEnd = endIdx;

      // ---- Parse cond block [condStart, bodyIdx): Const bound?, Binary cmp,
      // Branch. The Branch's lhs is the cmp result slot. ----
      std::size_t branchIdx = std::size_t(-1);
      for (std::size_t j = condStart; j < bodyIdx; ++j)
        if (fn.instructions[j].op == Op::Branch) { branchIdx = j; break; }
      if (branchIdx == std::size_t(-1)) return false;
      const int cmpSlot = fn.instructions[branchIdx].lhs;
      if (cmpSlot == -1) return false;
      std::size_t cmpIdx = std::size_t(-1);
      for (std::size_t j = condStart; j < branchIdx; ++j) {
        if (fn.instructions[j].op == Op::Binary && fn.instructions[j].dest == cmpSlot) {
          cmpIdx = j; break;
        }
      }
      if (cmpIdx == std::size_t(-1)) return false;
      const auto &cmpIns = fn.instructions[cmpIdx];
      using BOP = ir::BinaryOp;
      const BOP cmpOp = cmpIns.binary;

      // ---- Parse body [bodyStart, bodyEnd): collect non-Label insts. ----
      // The trailing back-edge `Goto _while_cond_` is part of the loop
      // structure, not body control flow — skip it rather than rejecting.
      const std::string &condLabel = fn.instructions[ci].label;
      std::vector<std::size_t> bodyInsts;
      for (std::size_t j = bodyStart; j < bodyEnd; ++j) {
        const auto &b = fn.instructions[j];
        if (b.op == Op::Label) continue;
        if (b.op == Op::Goto && b.label == condLabel) continue;  // back-edge
        if (b.op == Op::Goto || b.op == Op::Branch || b.op == Op::Return ||
            b.op == Op::ReturnVoid || b.op == Op::Call || b.op == Op::StoreGlobal)
          return false;  // body has control flow / side effects → reject
        bodyInsts.push_back(j);
      }
      if (bodyInsts.empty() || bodyInsts.size() > 48) return false;

      // Identify the induction update. Two shapes:
      //  (1) self-add/sub `i = i +/- step` (Binary dest==lhs, rhs const),
      //      where dest appears in the comparison;
      //  (2) after copy-propagation the self-update is aliased to a temp:
      //      `t = i + step` (body temp) then `Move i = t`. We detect this by
      //      scanning for a Move whose dest is a cmp operand and whose source
      //      temp is defined in the body as `t = i +/- K` (K const nonzero).
      int iSlot = -1;
      long long step = 0;
      std::size_t indIdx = std::size_t(-1);
      for (std::size_t bi : bodyInsts) {
        const auto &b = fn.instructions[bi];
        if (b.op != Op::Binary) continue;
        if (b.dest != b.lhs || b.dest == -1) continue;
        if (b.binary != BOP::Add && b.binary != BOP::Sub) continue;
        if (b.dest == cmpIns.lhs || b.dest == cmpIns.rhs) {
          if (iSlot != -1) return false;  // two induction vars
          auto r = constOf(b.rhs);
          if (!r.ok || r.v == 0) return false;
          iSlot = b.dest;
          step = (b.binary == BOP::Sub) ? -r.v : r.v;
          indIdx = bi;
        }
      }
      if (iSlot == -1) {
        // Shape (2): Move i <- t where t is a body temp `i +/- K`.
        for (std::size_t bi : bodyInsts) {
          const auto &b = fn.instructions[bi];
          if (b.op != Op::Move) continue;
          if (b.dest != cmpIns.lhs && b.dest != cmpIns.rhs) continue;
          if (b.dest == -1) continue;
          // find the body def of the source temp
          std::size_t tdef = std::size_t(-1);
          for (std::size_t bi2 : bodyInsts) {
            if (fn.instructions[bi2].dest == b.lhs) {
              if (tdef != std::size_t(-1)) { tdef = std::size_t(-1); break; }
              tdef = bi2;
            }
          }
          if (tdef == std::size_t(-1)) continue;
          const auto &t = fn.instructions[tdef];
          if (t.op != Op::Binary) continue;
          bool lIsI = (t.lhs == b.dest), rIsI = (t.rhs == b.dest);
          if (!lIsI && !rIsI) continue;
          int other = lIsI ? t.rhs : t.lhs;
          auto oc = constOf(other);
          if (!oc.ok || oc.v == 0) continue;
          if (t.binary == BOP::Add) step = oc.v;
          else if (t.binary == BOP::Sub && lIsI) step = -oc.v;
          else continue;
          if (iSlot != -1) return false;  // two induction vars
          iSlot = b.dest;
          indIdx = bi;
        }
      }
      if (iSlot == -1) return false;

      // Identify the accumulator `s`: the slot written in the body (excl. i)
      // that is read AFTER the loop (live-out). Chain temps are single-use
      // within the body and dead after, so the live-out slot is `s`. Require
      // exactly one.
      std::unordered_set<int> bodyWritten;
      for (std::size_t bi : bodyInsts) {
        const auto &b = fn.instructions[bi];
        if (b.op == Op::Binary && b.dest != -1 && b.dest != iSlot)
          bodyWritten.insert(b.dest);
      }
      std::unordered_set<int> liveOut;
      for (std::size_t j = endIdx + 1; j < fn.instructions.size(); ++j)
        for (int s : readsOf(fn.instructions[j])) liveOut.insert(s);
      int accSlot = -1;
      for (int w : bodyWritten) {
        if (liveOut.count(w)) {
          if (accSlot != -1) return false;  // ambiguous accumulator
          accSlot = w;
        }
      }
      if (accSlot == -1) return false;  // no live-out accumulator → leave to DCE

      // The Binary that writes back to accSlot (the chain tail).
      std::size_t accWriteIdx = std::size_t(-1);
      for (std::size_t bi : bodyInsts) {
        if (fn.instructions[bi].dest == accSlot) {
          if (accWriteIdx != std::size_t(-1)) return false;  // written twice
          accWriteIdx = bi;
        }
      }
      if (accWriteIdx == std::size_t(-1)) return false;

      // Helper: find the body Binary/Const index that defines `slot` (single def).
      // Polynomial in the induction i: qa*i^2 + a*i + c. We evaluate forward
      // over the body's pure Binary/Const/Move instructions, building a map
      // slot → Poly for each single-def body temp. This subsumes the old
      // backward affine-chain heuristic and additionally handles quadratic
      // reduction bodies like s += i*i, s += (i+1)*(i+1), s += a*i*i + b*i + c.
      struct Poly { long long qa = 0, a = 0, c = 0; };
      // Scalar × Poly (K const) → qa*K, a*K, c*K. K≠0.
      auto mulK = [](const Poly &p, long long K, bool &ok) -> Poly {
        return {safeMulStat(p.qa, K, ok), safeMulStat(p.a, K, ok),
                safeMulStat(p.c, K, ok)};
      };
      // Wrap a Poly into int32 range (two's complement): no-op if fits.
      auto wrap32Poly = [](Poly &p, bool &ok) {
        p.qa = trunc32Stat(p.qa, ok); p.a = trunc32Stat(p.a, ok); p.c = trunc32Stat(p.c, ok);
      };
      std::unordered_map<int, Poly> polyOf;   // slot → Poly (body-defined/const/i)
      polyOf[iSlot] = {0, 1, 0};
      // Seed constants seen anywhere (function-wide single-def Consts) so a
      // globalConst référent used by a Mul operand resolves even if the Const
      // sits above the loop.
      for (const auto &kv : globalConst) polyOf[kv.first] = {0, 0, kv.second};
      // Seed the accumulator as the zero polynomial. The body computes
      // `s = s + term` (an additive reduction); for the reduction we only need
      // the *increment* per iteration, so s's live-in value is irrelevant —
      // treat it as 0 and emit `s = s + Σterm` separately at the end.
      polyOf[accSlot] = {0, 0, 0};
      bool illFormed = false;
      auto constOf2 = [&](int s) -> long long* {
        auto it = polyOf.find(s);
        if (it == polyOf.end()) return nullptr;
        return (it->second.qa == 0 && it->second.a == 0) ? &it->second.c : nullptr;
      };
      auto evalBody = [&]() {
        for (std::size_t bi : bodyInsts) {
          const auto &b = fn.instructions[bi];
          if (b.op == Op::Const) { polyOf[b.dest] = {0, 0, b.value}; continue; }
          if (b.op == Op::Move) {
            auto it = polyOf.find(b.lhs);
            if (it == polyOf.end()) { illFormed = true; return; }
            polyOf[b.dest] = it->second; continue;
          }
          if (b.op != Op::Binary) { illFormed = true; return; }
          // operands must be known body temps / constants
          auto li = polyOf.find(b.lhs), ri = polyOf.find(b.rhs);
          if (li == polyOf.end() || ri == polyOf.end()) { illFormed = true; return; }
          const Poly &L = li->second, &R = ri->second;
          Poly out;
          long long *lcpt, *rcpt;
          bool Lc = (L.qa == 0 && L.a == 0) && (lcpt = constOf2(b.lhs));
          bool Rc = (R.qa == 0 && R.a == 0) && (rcpt = constOf2(b.rhs));
          if (b.binary == BOP::Add)        out = {L.qa + R.qa, L.a + R.a, L.c + R.c};
          else if (b.binary == BOP::Sub)   out = {L.qa - R.qa, L.a - R.a, L.c - R.c};
          else if (b.binary == BOP::Mul) {
            if (Lc && Rc) { out = {0, 0, safeMulStat(*lcpt, *rcpt, illFormed)}; }
            else if (Lc)  { out = mulK(R, *lcpt, illFormed); }  // R is the poly, K=*lcpt
            else if (Rc)  { out = mulK(L, *rcpt, illFormed); }  // L is the poly, K=*rcpt
            else {
              // affine × affine (both qa==0): (a1*i+c1)(a2*i+c2) = a1*a2*i² +
              // (a1*c2 + a2*c1)*i + c1*c2. If either operand has qa≠0 the
              // product is degree ≥3 — out of scope, reject.
              if (L.qa != 0 || R.qa != 0) { illFormed = true; return; }
              long long qa = safeMulStat(L.a, R.a, illFormed);
              long long a = safeMulStat(L.a, R.c, illFormed) + safeMulStat(R.a, L.c, illFormed);
              long long c = safeMulStat(L.c, R.c, illFormed);
              out = {qa, a, c};
            }
          } else { illFormed = true; return; }
          wrap32Poly(out, illFormed);
          if (illFormed) return;
          polyOf[b.dest] = out;
        }
      };
      evalBody();
      if (illFormed) return false;

      // The accumulator was seeded as the zero polynomial, and evalBody forwards
//      it through the body's `s = s + ...` / `s = term` (where term reads the
//      prior s) writes. So polyOf[accSlot] after evalBody IS the per-iteration
//      increment, regardless of whether the final write is `s = s + term`,
//      `Move s = term`, or an irreducible `s = X + Y` where s was consumed
//      earlier (e.g. s = (s + i*i + 7*i) + 5).
      auto accIt = polyOf.find(accSlot);
      if (accIt == polyOf.end()) return false;
      long long qaCoef = accIt->second.qa;
      long long aCoef  = accIt->second.a;
      long long cConst = accIt->second.c;
      // Reject a zero increment (no reduction happening) — leave to DCE.
      if (qaCoef == 0 && aCoef == 0 && cConst == 0) return false;
      (void)accWriteIdx;
      (void)indIdx;

      // ---- Resolve i_init, N, s_init as constants; normalize cmp to "i OP bound". ----
      int boundSlot = -1;
      BOP normOp;
      if (cmpIns.lhs == iSlot) { boundSlot = cmpIns.rhs; normOp = cmpOp; }
      else if (cmpIns.rhs == iSlot) {
        boundSlot = cmpIns.lhs;
        switch (cmpOp) {
          case BOP::Less: normOp = BOP::Greater; break;
          case BOP::LessEqual: normOp = BOP::GreaterEqual; break;
          case BOP::Greater: normOp = BOP::Less; break;
          case BOP::GreaterEqual: normOp = BOP::LessEqual; break;
          case BOP::NotEqual: normOp = BOP::NotEqual; break;  // symmetric
          default: return false;  // Equal is not a loop condition we handle
        }
      } else {
        return false;
      }
      if (boundSlot == -1) return false;
      auto boundRes = constOf(boundSlot);
      if (!boundRes.ok) return false;
      long long N = boundRes.v;
      auto initRes = resolveInitConst(fn, iSlot, ci);
      if (!initRes.ok) return false;
      long long iInit = initRes.v;
      // Note: we do NOT require s_init to be a known constant — the additive
      // form `s = s + sum` emitted below is sound regardless of s's entry
      // value (which matters for nested loops where s carries over).

      // ---- Trip count T. ----
      long long T = -1;
      if (normOp == BOP::NotEqual) {
        // `while (i != N)`: runs until i hits N. Zero trips if iInit == N.
        // Otherwise requires step to move i exactly toward N (same sign) and
        // (N - iInit) divisible by step, else the loop is infinite → reject.
        long long diff = N - iInit;
        if (diff == 0) T = 0;
        else {
          if ((diff > 0) != (step > 0)) return false;  // moving away → infinite
          if (diff % step != 0) return false;           // never hits N exactly
          T = diff / step;
          if (T <= 0) return false;
        }
      } else if (step > 0) {
        if (normOp == BOP::Less) T = ceilDivPos(N - iInit, step);
        else if (normOp == BOP::LessEqual) {
          if (N < iInit) T = 0;
          else T = (N - iInit) / step + 1;
        } else return false;
      } else if (step < 0) {
        long long ns = -step;
        if (normOp == BOP::Greater) T = ceilDivPos(iInit - N, ns);
        else if (normOp == BOP::GreaterEqual) {
          if (iInit < N) T = 0;
          else T = (iInit - N) / ns + 1;
        } else return false;
      } else {
        return false;
      }
      if (T < 0) return false;
      if (T > 3000000000LL) return false;  // int64-overflow guard

      // ---- Closed-form sum in int64, with overflow checks. ----
      // sum = Σ_{k=0..T-1} (a*(iInit + k*step) + c)
      //     = T*(a*iInit + c) + a*step*T*(T-1)/2
      // We emit the ADDITIVE form `s = s + sum` (not `s = s_init + sum`),
      // which is sound whether the loop runs once, is nested inside another
      // loop (s carries over between outer iterations), or runs sequentially
      // after prior updates. constFold/globalConstProp later fold it to a
      // single Const when s_init is a known constant.
      auto safeMul = [](long long x, long long y, bool &ok) -> long long {
        if (!ok) return 0;
        if (x == 0 || y == 0) return 0;
        if (x > 0) {
          if (y > 0 ? x > (LLONG_MAX / y) : y < (LLONG_MIN / x)) { ok = false; return 0; }
        } else {
          if (y > 0 ? x < (LLONG_MIN / y) : y < (LLONG_MAX / x)) { ok = false; return 0; }
        }
        return x * y;
      };
      bool ok = true;
      long long aI = safeMul(aCoef, iInit, ok);
      long long base = aI + cConst;
      if (ok && ((aI > 0 && cConst > 0 && base < aI) ||
                 (aI < 0 && cConst < 0 && base > aI))) ok = false;
      long long term1 = safeMul(T, base, ok);                 // a*Σi + c*T part
      long long Tm1 = T - 1;
      long long tri = safeMul(T, Tm1, ok);  // T*(T-1), always even
      bool triNeg = tri < 0;
      tri = triNeg ? -tri : tri;
      tri /= 2;
      if (triNeg) tri = -tri;
      long long astep = safeMul(aCoef, step, ok);
      long long term2 = safeMul(astep, tri, ok);               // a*step*tri
      // Quadratic part: Σi² = T*iInit² + 2*iInit*step*tri + step² * T*(T-1)*(2T-1)/6.
      // Cubic C = T*(T-1)*(2T-1)/6, term3 = qa*(T*i0² + 2*i0*step*tri + step²*C).
      long long qaSum = 0;
      if (qaCoef != 0) {
        long long i0sq = safeMul(iInit, iInit, ok);
        long long tq = safeMul(T, i0sq, ok);                  // T*iInit²
        long long two = safeMul(2, iInit, ok);
        long long mid = safeMul(two, step, ok);
        mid = safeMul(mid, tri, ok);                          // 2*i0*step*tri
        long long stepsq = safeMul(step, step, ok);
        long long twotm1 = 2 * T - 1;                             // 2T-1
        long long cube = safeMul(tri, twotm1, ok);                // T*(T-1)/2 * (2T-1)
        if (ok && cube % 3 != 0) ok = false;                      // /6 = /2/3 → must be divisible
        cube /= 3;                                                // = T*(T-1)*(2T-1)/6
        long long last = safeMul(stepsq, cube, ok);               // step²*C
        long long sumsq = tq + mid;
        if (ok && ((tq > 0 && mid > 0 && sumsq < tq) ||
                   (tq < 0 && mid < 0 && sumsq > tq))) ok = false;
        sumsq += last;
        if (ok && ((sumsq - last > 0 && last > 0 && sumsq < (sumsq - last)) ||
                   (sumsq - last < 0 && last < 0 && sumsq > (sumsq - last)))) ok = false;
        qaSum = safeMul(qaCoef, sumsq, ok);                  // qa*Σi²
      }
      if (!ok) return false;
      long long sum = term1 + term2 + qaSum;
      if (getenv("TOYCC_DEBUG_LOSELIM")) fprintf(stderr, "LOSELIM qa=%lld a=%lld c=%lld T=%lld i0=%lld step=%lld sum=%lld\n", qaCoef, aCoef, cConst, T, iInit, step, sum);
      auto ovl = [](long long r, long long s, long long t) -> bool {
        return (r > 0 && s > 0 && t < r) || (r < 0 && s < 0 && t > r);
      };
      if (ovl(term1, term2, term1 + term2)) return false;
      long long s12 = term1 + term2;
      if (ovl(s12, qaSum, sum)) return false;
      long long iFinalLL = iInit + safeMul(T, step, ok);
      if (!ok) return false;

      // trunc32 matches two's-complement wraparound (no-op when no overflow,
      // which is the guaranteed case for no-UB programs; correct otherwise
      // because 32-bit addition is associative mod 2^32).
      auto trunc32 = [](long long x) -> int {
        return static_cast<int>(static_cast<std::uint32_t>(x));
      };
      int sum32 = trunc32(sum);
      int iFinal32 = trunc32(iFinalLL);

      // ---- Rewrite: replace [ci, endIdx) with:
      //   Const sum → newTmp; Binary s = s Add newTmp; Const i_final → i
      // The additive `s = s + sum` is sound for nested/sequential loops.
      // constFold collapses it to a single Const when s_init is known. ----
      const int newTmp = fn.slotCount++;
      std::vector<Inst> out;
      out.reserve(fn.instructions.size() - (endIdx - ci) + 6);
      for (std::size_t j = 0; j < ci; ++j) out.push_back(fn.instructions[j]);
      Inst sc; sc.op = Op::Const; sc.dest = newTmp; sc.value = sum32;
      out.push_back(sc);
      Inst add; add.op = Op::Binary; add.dest = accSlot; add.lhs = accSlot;
      add.rhs = newTmp; add.binary = ir::BinaryOp::Add;
      out.push_back(add);
      Inst ic; ic.op = Op::Const; ic.dest = iSlot; ic.value = iFinal32;
      out.push_back(ic);
      for (std::size_t j = endIdx; j < fn.instructions.size(); ++j)
        out.push_back(fn.instructions[j]);
      fn.instructions = std::move(out);
      return true;
    };

    for (std::size_t ci = 0; ci < fn.instructions.size(); ++ci) {
      if (fn.instructions[ci].op != Op::Label) continue;
      if (fn.instructions[ci].label.find("_while_cond_") == std::string::npos) continue;
      if (tryElim(ci)) { changed = true; any = true; break; }
    }
  }
  return any;
}

// slot. After copyCoalesce, `input = input + 1; input = input + 1; ...` looks
// like a run of `Binary input = input Add constSlot` where constSlot is a
// single-def Const. We merge a run into one `Binary input = input Add (sum)`
// by bumping the first Const's value and dropping the later Binarys.
//
// Soundness: a merge of `x = x + c1` (at b1) and `x = x + c2` (at b2) into
// `x = x + (c1+c2)` requires that NOTHING between b1+1 and b2-1 reads or
// writes x — otherwise the intermediate value of x would be observed and
// folding the two adds would change it. The second Binary's own read of x
// (its lhs) is the merge point and is allowed. The first Binary's rhs Const
// slot must be single-use (only the first Binary reads it) so changing its
// value is safe; the dropped second Binary's rhs Const becomes dead and is
// removed by DCE.
// ---------------------------------------------------------------------------
// Instruction combining: collapse chained constant add/sub on a temp chain.
//
// After copyProp, a run of `input = input + 1; input = input + 1; ...` looks
// like a temp chain in the IR:
//     Const c1=1; Binary t1 = input + c1; Move input <- t1;
//     Const c2=1; Binary t2 = t1 + c2;    Move input <- t2;
//     Const c3=1; Binary t3 = t2 + c3;    Move input <- t3; ...
// (copyProp has replaced the `input` operand of each Binary with the previous
// temp, since `Move input <- t1` makes input alias t1.)
//
// We track a "chain tip" — the temp slot whose value is `base + accum` — and
// when the next Binary reads the tip and adds a constant, we:
//   - bump the FIRST Binary's rhs Const by the new constant (so tip now holds
//     base + accum + c), and
//   - rewrite that next Binary to `Move tnext <- tip` (tnext is now a copy of
//     tip), and advance the tip to tnext.
// After the pass, copyProp replaces reads of tnext with tip, DCE drops the
// dead Moves and now-unused Consts, and copyCoalesce folds the final
// `Move input <- tip` into the first Binary. The net result is a single
// `Binary input = input + (sum of all constants)`.
//
// Soundness: the first Binary's rhs Const must be single-use (only that Binary
// reads it) so bumping its value is safe — verified at chain start. Later
// Binarys' rhs Consts are simply orphaned (their reader is rewritten to a
// Move) and removed by DCE. The chain breaks at any control-flow boundary or
// when the tip slot is redefined.
bool instCombinePass(ir::Function &fn) {
  bool any = false;
  const auto uses = computeUseCount(fn);
  // defCount[slot] = number of pure defs in the function; constDefIndex[slot]
  // = index of the single Const def, valid only when defCount==1.
  std::unordered_map<int, int> defCount;
  std::unordered_map<int, std::size_t> constDefIndex;
  for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
    const auto &ins = fn.instructions[i];
    if (definesSlot(ins) && ins.dest != -1) {
      ++defCount[ins.dest];
      if (ins.op == Op::Const) constDefIndex[ins.dest] = i;
      else constDefIndex.erase(ins.dest);
    }
  }
  for (auto it = constDefIndex.begin(); it != constDefIndex.end();)
    if (defCount[it->first] != 1) it = constDefIndex.erase(it); else ++it;

  std::unordered_map<int, int> knownConst;  // slot -> const value (within BB)
  // Global const map: single-def Consts have a fixed value across the whole
  // function, so we can look them up even after a Label resets the per-BB
  // knownConst. Without this, a loop body Binary like `t = acc + 81` (where
  // the 81 Const lives at the function top, above several Labels) would fail
  // to start a chain — the per-BB map is empty after the Labels.
  std::unordered_map<int, int> globalConst;
  for (const auto &kv : constDefIndex)
    globalConst[kv.first] = fn.instructions[kv.second].value;
  int chainTip = -1;                         // active chain tip slot, or -1
  std::size_t chainConstIdx = 0;             // index of the chain's first Const
  int chainAccum = 0;                        // accumulated constant (in Add sense)
  bool chainFirstAdd = true;                 // first Binary's op is Add (vs Sub)
  auto bbReset = [&]() { knownConst.clear(); chainTip = -1; };

  for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
    auto &ins = fn.instructions[i];
    if (ins.op == Op::Label) { bbReset(); continue; }

    if (ins.op == Op::Const && ins.dest != -1 && constDefIndex.count(ins.dest)) {
      knownConst[ins.dest] = ins.value;
    }

    const bool isAddSub =
        ins.binary == ir::BinaryOp::Add || ins.binary == ir::BinaryOp::Sub;

    // Chain extension: Binary t = chainTip +/- c  → bump first Const, rewrite to Move.
    //
    // Soundness: bumping `chainConstIdx`'s Const changes the value the
    // chain-start Binary computes, which propagates to EVERY reader of the
    // chain tip — not just the next chain link. If an external reader (e.g.,
    // a `Move i = a` produced by copyProp/CSE aliasing the induction
    // `i = i + 1` to the chain start `a = i + 1`) reads the tip, bumping
    // corrupts that reader. So we may only extend while the current tip is
    // read by exactly this extending Binary (uses == 1). The final tip's
    // single reader is the accumulator feed (`s = s + tip`), which is the
    // intended consumer — that's fine. Any extra reader breaks the chain.
    if (ins.op == Op::Binary && chainTip != -1 && ins.lhs == chainTip &&
        ins.dest != -1 && ins.dest != chainTip && ins.rhs != -1 && isAddSub) {
      const int tipUses = uses.count(chainTip) ? uses.at(chainTip) : 0;
      if (tipUses != 1) { chainTip = -1; }  // external reader → chain unsound
    }
    if (ins.op == Op::Binary && chainTip != -1 && ins.lhs == chainTip &&
        ins.dest != -1 && ins.dest != chainTip && ins.rhs != -1 && isAddSub) {
      // Look up rhs as a const: prefer per-BB knownConst (handles locals that
      // are redefined per BB), fall back to globalConst (single-def Consts
      // whose value is fixed across the whole function).
      auto rk = knownConst.find(ins.rhs);
      bool found = (rk != knownConst.end());
      int cv = 0;
      if (!found) {
        auto gk = globalConst.find(ins.rhs);
        if (gk != globalConst.end()) { found = true; cv = gk->second; }
      } else {
        cv = rk->second;
      }
      if (found) {
        const int c = (ins.binary == ir::BinaryOp::Add) ? cv : -cv;
        chainAccum += c;
        // The first Binary is `tip = base Add/Sub Const`. Set Const so that
        // base op Const == base + chainAccum. For Add: Const = chainAccum.
        // For Sub: Const = -chainAccum (since base - Const = base + chainAccum).
        fn.instructions[chainConstIdx].value =
            chainFirstAdd ? chainAccum : -chainAccum;
        ins.op = Op::Move;
        ins.lhs = chainTip;
        ins.rhs = -1;
        ins.binary = ir::BinaryOp::Add;
        chainTip = ins.dest;  // t is now a copy of the old tip
        any = true;
        continue;
      }
    }

    // New chain start: Binary t = base +/- c, where c is a single-def single-use
    // Const (so bumping it later is safe).
    if (ins.op == Op::Binary && ins.dest != -1 && ins.lhs != -1 &&
        ins.dest != ins.lhs && ins.rhs != -1 && isAddSub) {
      auto rk = knownConst.find(ins.rhs);
      bool found = (rk != knownConst.end());
      int cv = 0;
      if (!found) {
        auto gk = globalConst.find(ins.rhs);
        if (gk != globalConst.end()) { found = true; cv = gk->second; }
      } else {
        cv = rk->second;
      }
      auto cit = constDefIndex.find(ins.rhs);
      const int rhsUses = uses.count(ins.rhs) ? uses.at(ins.rhs) : 0;
      if (found && cit != constDefIndex.end() && rhsUses == 1) {
        chainTip = ins.dest;
        chainConstIdx = cit->second;
        chainFirstAdd = (ins.binary == ir::BinaryOp::Add);
        chainAccum = chainFirstAdd ? cv : -cv;
        continue;
      }
    }

    // The chain breaks if the tip is redefined, or at a control boundary.
    if (definesSlot(ins) && ins.dest != -1 && ins.dest == chainTip) chainTip = -1;
    if (isControlBoundary(ins)) bbReset();
  }
  return any;
}

// ---------------------------------------------------------------------------
// Tail-recursion to loop. When a function `f` ends a path with
// `Call t = f(args); Return t` (self-recursion in tail position), replace it
// with `Move param[k] <- args[k]; Goto f_entry`. The entry label sits just
// after the backend prologue, so the prologue (frame setup + param move-in)
// runs once; subsequent "iterations" reuse the same frame and just overwrite
// the param slots with the new args. This converts tail recursion into
// iteration — no stack growth, no per-call frame setup.
bool tailCallPass(ir::Function &fn) {
  if (fn.name.empty()) return false;
  bool any = false;
  std::vector<Inst> out;
  out.reserve(fn.instructions.size() + 4);
  const std::string entry = ".L_" + fn.name + "_tail_entry";
  Inst entryLab;
  entryLab.op = Op::Label;
  entryLab.label = entry;
  out.push_back(entryLab);
  for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
    const auto &ins = fn.instructions[i];
    if (ins.op == Op::Call && ins.name == fn.name && i + 1 < fn.instructions.size()) {
      const auto &nxt = fn.instructions[i + 1];
      if (nxt.op == Op::Return && nxt.lhs == ins.dest) {
        const std::size_t nargs = std::min(ins.args.size(), fn.paramSlots.size());
        for (std::size_t k = 0; k < nargs; ++k) {
          Inst mv;
          mv.op = Op::Move;
          mv.dest = fn.paramSlots[k];
          mv.lhs = ins.args[k];
          out.push_back(mv);
        }
        Inst g;
        g.op = Op::Goto;
        g.label = entry;
        out.push_back(g);
        ++i;  // consume the Return
        any = true;
        continue;
      }
    }
    out.push_back(ins);
  }
  if (any) fn.instructions = std::move(out);
  return any;
}

// Global constant propagation across basic blocks.
//
// Runs a forward dataflow on the CFG: IN[BB] = intersection of OUT[preds]
// (a slot is const at IN only if ALL preds agree on its value), OUT[BB] =
// transfer(IN[BB]) walking the BB's instructions. Iterates to fixed point.
// Then rewrites each BB seeded with IN[BB] — same rewrites as constFoldPass
// (Move<-const → Const, fold Unary/Binary, algebraic simplification) but with
// cross-BB constants.
//
// Soundness:
// - "All-preds-agree" merge: at a loop header, the pre-header says `i=0` and
//   the back-edge says `i=i+1` (not const) → intersection is empty → i is not
//   const at the header. Correct.
// - Call clears inst.dest (return value unknown) but does NOT kill local
//   consts — they're in slots, not globals. LoadGlobal already doesn't set
//   known (global values aren't compile-time constants), so Call can't
//   invalidate them.
// - StoreGlobal doesn't affect local slots' const-ness.
//
// Runs ONCE at the start of optimizeFunction; the local passes then iterate
// to fixed point. Running the dataflow 16× (once per fixed-point iter) would
// be wasteful.
bool globalConstPropPass(ir::Function &fn) {
  CFG cfg = buildCFG(fn);
  if (cfg.blocks.empty()) return false;

  // Transfer: walk a BB's instructions starting from `known`, return the
  // final known map. Pure (no mutation of fn).
  auto transferFn = [&](std::unordered_map<int, int> known,
                        const BasicBlock &bb)
      -> std::unordered_map<int, int> {
    for (std::size_t i = bb.startIdx; i <= bb.endIdx; ++i) {
      const auto &inst = fn.instructions[i];
      switch (inst.op) {
        case Op::Const:
          if (inst.dest != -1) known[inst.dest] = inst.value;
          break;
        case Op::Move: {
          if (inst.dest != -1) known.erase(inst.dest);
          auto it = known.find(inst.lhs);
          if (it != known.end() && inst.dest != -1) known[inst.dest] = it->second;
          break;
        }
        case Op::Unary: {
          if (inst.dest != -1) known.erase(inst.dest);
          auto it = known.find(inst.lhs);
          if (it != known.end() && inst.dest != -1)
            known[inst.dest] = foldConstUnary(inst.unary, it->second);
          break;
        }
        case Op::Binary: {
          if (inst.dest != -1) known.erase(inst.dest);
          auto lk = known.find(inst.lhs);
          auto rk = known.find(inst.rhs);
          if (lk != known.end() && rk != known.end() && inst.dest != -1)
            known[inst.dest] = foldConstBinary(inst.binary, lk->second, rk->second);
          break;
        }
        case Op::LoadGlobal:
          if (inst.dest != -1) known.erase(inst.dest);
          break;
        case Op::Call:
          if (inst.dest != -1) known.erase(inst.dest);
          break;
        case Op::StoreGlobal:
          break;
        default:
          break;
      }
    }
    return known;
  };

  std::vector<std::unordered_map<int, int>> in(cfg.blocks.size());
  std::vector<std::unordered_map<int, int>> out(cfg.blocks.size());

  bool dfChanged = true;
  int dfIter = 0;
  while (dfChanged && dfIter < 20) {
    dfChanged = false;
    ++dfIter;
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
      // IN[b] = intersection of OUT[preds] (all-preds-agree).
      // The entry block's IN is the boundary condition: {} (no constants come
      // from outside the function). Without this, a loop-header entry (e.g.,
      // tail_entry after tailCallPass) would inherit constants from the
      // back-edge, wrongly concluding that params are const.
      std::unordered_map<int, int> newIn;
      if (b == cfg.entryBlock) {
        newIn = {};
      } else if (!cfg.blocks[b].preds.empty()) {
        newIn = out[cfg.blocks[b].preds[0]];
        for (std::size_t k = 1; k < cfg.blocks[b].preds.size(); ++k) {
          const auto &predOut = out[cfg.blocks[b].preds[k]];
          for (auto it = newIn.begin(); it != newIn.end();) {
            auto jt = predOut.find(it->first);
            if (jt == predOut.end() || jt->second != it->second)
              it = newIn.erase(it);
            else
              ++it;
          }
        }
      }
      auto newOut = transferFn(newIn, cfg.blocks[b]);
      if (newIn != in[b] || newOut != out[b]) {
        in[b] = std::move(newIn);
        out[b] = std::move(newOut);
        dfChanged = true;
      }
    }
  }
  if (getenv("TOYCC_DUMP_GCP")) {
    fprintf(stderr, "=== GCP Function %s (dfIters=%d) ===\n", fn.name.c_str(), dfIter);
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
      fprintf(stderr, "  BB%zu [%zu..%zu] label=%s preds=", b, cfg.blocks[b].startIdx, cfg.blocks[b].endIdx, cfg.blocks[b].label.c_str());
      for (auto p : cfg.blocks[b].preds) fprintf(stderr, "%zu ", p);
      fprintf(stderr, " succs=");
      for (auto s : cfg.blocks[b].succs) fprintf(stderr, "%zu ", s);
      fprintf(stderr, "\n    IN={");
      for (auto &kv : in[b]) fprintf(stderr, "%d:%d ", kv.first, kv.second);
      fprintf(stderr, "} OUT={");
      for (auto &kv : out[b]) fprintf(stderr, "%d:%d ", kv.first, kv.second);
      fprintf(stderr, "}\n");
    }
  }

  // Rewrite phase: walk each BB seeded with IN[b], apply constFold rewrites.
  bool any = false;
  for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
    std::unordered_map<int, int> known = in[b];
    auto clearDef = [&](int slot) {
      if (slot != -1) known.erase(slot);
    };
    for (std::size_t i = cfg.blocks[b].startIdx; i <= cfg.blocks[b].endIdx; ++i) {
      auto &inst = fn.instructions[i];
      switch (inst.op) {
        case Op::Const:
          if (inst.dest != -1) known[inst.dest] = inst.value;
          break;
        case Op::Move: {
          clearDef(inst.dest);
          auto it = known.find(inst.lhs);
          if (it != known.end()) {
            int v = it->second;
            inst.op = Op::Const;
            inst.value = v;
            inst.lhs = -1;
            known[inst.dest] = v;
            any = true;
          }
          break;
        }
        case Op::Unary: {
          clearDef(inst.dest);
          auto it = known.find(inst.lhs);
          if (it != known.end()) {
            int v = foldConstUnary(inst.unary, it->second);
            inst.op = Op::Const;
            inst.value = v;
            inst.unary = ir::UnaryOp::Plus;
            inst.lhs = -1;
            known[inst.dest] = v;
            any = true;
          }
          break;
        }
        case Op::Binary: {
          clearDef(inst.dest);
          auto lk = known.find(inst.lhs);
          auto rk = known.find(inst.rhs);
          const bool lc = (lk != known.end());
          const bool rc = (rk != known.end());
          if (lc && rc) {
            int v = foldConstBinary(inst.binary, lk->second, rk->second);
            inst.op = Op::Const;
            inst.value = v;
            inst.binary = ir::BinaryOp::Add;
            inst.lhs = inst.rhs = -1;
            known[inst.dest] = v;
            any = true;
            break;
          }
          switch (inst.binary) {
            case ir::BinaryOp::Add:
              if (lc && lk->second == 0) {
                inst.op = Op::Move; inst.lhs = inst.rhs; inst.rhs = -1; any = true;
              } else if (rc && rk->second == 0) {
                inst.op = Op::Move; inst.rhs = -1; any = true;
              }
              break;
            case ir::BinaryOp::Sub:
              if (rc && rk->second == 0) {
                inst.op = Op::Move; inst.rhs = -1; any = true;
              } else if (lc && lk->second == 0) {
                inst.op = Op::Unary;
                inst.unary = ir::UnaryOp::Minus;
                inst.lhs = inst.rhs;
                inst.rhs = -1;
                any = true;
              }
              break;
            case ir::BinaryOp::Mul:
              if ((lc && lk->second == 0) || (rc && rk->second == 0)) {
                inst.op = Op::Const; inst.value = 0;
                inst.binary = ir::BinaryOp::Add; inst.lhs = inst.rhs = -1;
                any = true;
              } else if (lc && lk->second == 1) {
                inst.op = Op::Move; inst.lhs = inst.rhs; inst.rhs = -1; any = true;
              } else if (rc && rk->second == 1) {
                inst.op = Op::Move; inst.rhs = -1; any = true;
              }
              break;
            default:
              break;
          }
          break;
        }
        case Op::LoadGlobal:
          clearDef(inst.dest);
          break;
        case Op::Call:
          clearDef(inst.dest);
          break;
        case Op::StoreGlobal:
          break;
        default:
          break;
      }
    }
  }
  return any;
}

// Global copy propagation across basic blocks.
//
// Tracks `Move d <- s` aliases: when d is read, s can be substituted. Runs a
// forward dataflow on the CFG: IN[BB] = INTERSECTION of OUT[preds] (a copy is
// substitutable at IN only if ALL preds agree on the same d→s alias), OUT[BB]
// = transfer(IN[BB]) walking the BB's instructions. Iterates to fixed point.
// Then rewrites each BB seeded with IN[BB] — same substitutions as
// copyPropPass but with cross-BB aliases.
//
// Soundness — why intersection (not union):
//   A copy (d, s) is substitutable at a BB only if it holds on EVERY path to
//   that BB. If one pred has (d, s) and another doesn't (because d or s was
//   redefined on that path), substituting would read a stale s on the path
//   where the copy is invalid. Union would be unsound; intersection is
//   conservative but correct.
//   At a loop header, a copy from the pre-header is NOT in the back-edge's
//   OUT if d or s is redefined in the body → intersection drops it → no
//   substitution at the header. Correct.
//
// Kill rules:
//   - Any def of slot X kills every alias (d, s) where d == X or s == X
//     (redefining either side invalidates the copy).
//   - Call: conservatively kill all aliases whose d or s is NOT a param slot
//     or named slot — actually, since Call only writes globals (not local
//     slots) and inst.dest, aliases between local slots survive. But the
//     return value (inst.dest) is a new def, killing aliases involving it.
//   - LoadGlobal: def of inst.dest, kills aliases involving inst.dest.
//   - StoreGlobal: no effect on local-slot aliases.
bool globalCopyPropPass(ir::Function &fn) {
  CFG cfg = buildCFG(fn);
  if (cfg.blocks.empty()) return false;

  // Alias map: slot d -> slot s (reads of d can be substituted with s).
  using AliasMap = std::unordered_map<int, int>;

  auto transferFn = [&](AliasMap known, const BasicBlock &bb) -> AliasMap {
    auto invalidate = [&](int slot) {
      if (slot == -1) return;
      for (auto it = known.begin(); it != known.end();) {
        if (it->second == slot || it->first == slot) it = known.erase(it);
        else ++it;
      }
    };
    for (std::size_t i = bb.startIdx; i <= bb.endIdx; ++i) {
      const auto &inst = fn.instructions[i];
      switch (inst.op) {
        case Op::Const:
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        case Op::Move: {
          // Subst the source first (chain follow happens at rewrite time).
          int src = inst.lhs;
          auto it = known.find(src);
          if (it != known.end()) src = it->second;
          invalidate(inst.dest);
          if (inst.dest != -1 && inst.lhs != -1 && inst.dest != inst.lhs)
            known[inst.dest] = inst.lhs;
          break;
        }
        case Op::Unary: {
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        }
        case Op::Binary: {
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        }
        case Op::LoadGlobal:
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        case Op::Call:
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        case Op::StoreGlobal:
          break;
        default:
          break;
      }
    }
    return known;
  };

  std::vector<AliasMap> in(cfg.blocks.size());
  std::vector<AliasMap> out(cfg.blocks.size());

  bool dfChanged = true;
  int dfIter = 0;
  while (dfChanged && dfIter < 20) {
    dfChanged = false;
    ++dfIter;
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
      AliasMap newIn;
      if (b == cfg.entryBlock) {
        newIn = {};
      } else if (!cfg.blocks[b].preds.empty()) {
        newIn = out[cfg.blocks[b].preds[0]];
        for (std::size_t k = 1; k < cfg.blocks[b].preds.size(); ++k) {
          const auto &predOut = out[cfg.blocks[b].preds[k]];
          for (auto it = newIn.begin(); it != newIn.end();) {
            auto jt = predOut.find(it->first);
            if (jt == predOut.end() || jt->second != it->second)
              it = newIn.erase(it);
            else
              ++it;
          }
        }
      }
      auto newOut = transferFn(newIn, cfg.blocks[b]);
      if (newIn != in[b] || newOut != out[b]) {
        in[b] = std::move(newIn);
        out[b] = std::move(newOut);
        dfChanged = true;
      }
    }
  }

  // Rewrite phase: walk each BB seeded with IN[b], substitute operands.
  bool any = false;
  for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
    AliasMap known = in[b];
    auto invalidate = [&](int slot) {
      if (slot == -1) return;
      for (auto it = known.begin(); it != known.end();) {
        if (it->second == slot || it->first == slot) it = known.erase(it);
        else ++it;
      }
    };
    auto subst = [&](int s) -> int {
      if (s == -1) return s;
      int cur = s;
      int hops = 0;
      while (hops < 4) {
        auto it = known.find(cur);
        if (it == known.end()) break;
        cur = it->second;
        ++hops;
      }
      return cur;
    };
    for (std::size_t i = cfg.blocks[b].startIdx; i <= cfg.blocks[b].endIdx; ++i) {
      auto &inst = fn.instructions[i];
      switch (inst.op) {
        case Op::Const:
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        case Op::Move: {
          int src = subst(inst.lhs);
          if (src != inst.lhs) any = true;
          inst.lhs = src;
          invalidate(inst.dest);
          if (inst.dest != -1 && inst.lhs != -1 && inst.dest != inst.lhs)
            known[inst.dest] = inst.lhs;
          break;
        }
        case Op::Unary: {
          int s = subst(inst.lhs);
          if (s != inst.lhs) any = true;
          inst.lhs = s;
          invalidate(inst.dest);
          break;
        }
        case Op::Binary: {
          int l = subst(inst.lhs);
          int r = subst(inst.rhs);
          if (l != inst.lhs) any = true;
          if (r != inst.rhs) any = true;
          inst.lhs = l; inst.rhs = r;
          invalidate(inst.dest);
          break;
        }
        case Op::LoadGlobal:
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        case Op::Call:
          if (inst.dest != -1) invalidate(inst.dest);
          break;
        case Op::StoreGlobal: {
          int s = subst(inst.lhs);
          if (s != inst.lhs) any = true;
          inst.lhs = s;
          break;
        }
        case Op::Branch: {
          int s = subst(inst.lhs);
          if (s != inst.lhs) any = true;
          inst.lhs = s;
          break;
        }
        case Op::Return: {
          int s = subst(inst.lhs);
          if (s != inst.lhs) any = true;
          inst.lhs = s;
          break;
        }
        default:
          break;
      }
    }
  }
  return any;
}

// Global common-subexpression elimination via available-expressions dataflow.
//
// An expression (BinaryOp, lhsSlot, rhsSlot) is "available" at a program
// point if it has been computed and none of its operands or its destination
// has been redefined since. Runs a forward dataflow on the CFG:
//   IN[BB] = INTERSECTION of OUT[preds] (an expr is available at IN only if
//            ALL preds computed it and didn't kill it)
//   OUT[BB] = transfer(IN[BB]) walking the BB's instructions
// Iterates to fixed point. Then rewrites each BB seeded with IN[BB] — when a
// Binary's expr is in `avail` and the prior def's slot is still live, replace
// the Binary with `Move dest <- priorDef`.
//
// Soundness:
// - Intersection: a loop-only expr (computed in the body, not the pre-header)
//   is NOT available at the header (pre-header's OUT doesn't have it) → not
//   rewritten at the header. Correct.
// - Kill on def: any def of slot X kills every expr using X as operand (the
//   operand's value changed) AND every expr whose defSlot is X (the stored
//   value changed).
// - Self-update (dest == lhs or dest == rhs): the expr is computed with OLD
//   operands and stored to a slot that's also an operand. After the store,
//   the expr with NEW operands is NOT what was computed. We kill dest but do
//   NOT add the expr to avail. We also do NOT rewrite self-updates (the
//   prior expr used the old operand, rewriting would lose the update).
//
// Expression key encoding: (op << 40) | (a << 20) | b, where (a, b) is the
// commutative-normalized operand pair (smaller first for Add/Mul). 20 bits
// per operand (1M slots), 24 bits for op. No const encoding — const-fold
// already handled constant operands.
bool globalCsePass(ir::Function &fn) {
  CFG cfg = buildCFG(fn);
  if (cfg.blocks.empty()) return false;

  // Const-aware operand encoding: a slot that's known-const is encoded as
  // 0x40000000 | (value & 0x3FFFFF) so that two different Const slots holding
  // the same value produce the same expr key. Without this, `i * 2` computed
  // twice (with two different `Const 2` slots) wouldn't match.
  auto enc = [](int slot, const std::unordered_map<int, int> &known) -> int {
    auto it = known.find(slot);
    if (it != known.end()) return 0x40000000 | (it->second & 0x3FFFFF);
    return slot;
  };
  auto encodeExpr = [&](ir::BinaryOp op, int lhs, int rhs,
                        const std::unordered_map<int, int> &known) -> uint64_t {
    int a = enc(lhs, known), b = enc(rhs, known);
    if ((op == ir::BinaryOp::Add || op == ir::BinaryOp::Mul) && a > b)
      std::swap(a, b);
    return ((uint64_t)static_cast<int>(op) << 40) |
           ((uint64_t)(a & 0xFFFFF) << 20) |
           (uint64_t)(b & 0xFFFFF);
  };
  auto exprA = [](uint64_t k) { return (int)((k >> 20) & 0xFFFFF); };
  auto exprB = [](uint64_t k) { return (int)(k & 0xFFFFF); };

  using AvailMap = std::unordered_map<uint64_t, int>;  // exprKey -> defSlot

  // Track both `avail` (exprKey -> defSlot) and `known` (slot -> const value)
  // in the transfer so we can encode const operands. known is updated like
  // constFoldPass: Const sets, Move<-const propagates, other defs clear.
  struct TransferState {
    AvailMap avail;
    std::unordered_map<int, int> known;
  };
  using StateMap = TransferState;

  auto transferFn = [&](StateMap st, const BasicBlock &bb) -> StateMap {
    auto &avail = st.avail;
    auto &known = st.known;
    auto killSlot = [&](int slot) {
      if (slot == -1) return;
      known.erase(slot);
      for (auto it = avail.begin(); it != avail.end();) {
        if (it->second == slot || exprA(it->first) == slot ||
            exprB(it->first) == slot)
          it = avail.erase(it);
        else
          ++it;
      }
    };
    for (std::size_t i = bb.startIdx; i <= bb.endIdx; ++i) {
      const auto &inst = fn.instructions[i];
      switch (inst.op) {
        case Op::Const:
          if (inst.dest != -1) { known[inst.dest] = inst.value; killSlot(inst.dest); known[inst.dest] = inst.value; }
          break;
        case Op::Move: {
          if (inst.dest != -1) killSlot(inst.dest);
          auto it = known.find(inst.lhs);
          if (it != known.end() && inst.dest != -1) known[inst.dest] = it->second;
          break;
        }
        case Op::Unary: {
          if (inst.dest != -1) killSlot(inst.dest);
          break;
        }
        case Op::Binary: {
          if (inst.dest != -1) killSlot(inst.dest);
          const bool selfUpdate =
              (inst.dest == inst.lhs) || (inst.dest == inst.rhs);
          if (!selfUpdate && inst.dest != -1 && inst.lhs != -1 &&
              inst.rhs != -1) {
            uint64_t key = encodeExpr(inst.binary, inst.lhs, inst.rhs, known);
            avail[key] = inst.dest;
          }
          // Folded-const dest: if both operands known, dest is known too.
          auto lk = known.find(inst.lhs);
          auto rk = known.find(inst.rhs);
          if (lk != known.end() && rk != known.end() && inst.dest != -1)
            known[inst.dest] = foldConstBinary(inst.binary, lk->second, rk->second);
          break;
        }
        case Op::LoadGlobal:
        case Op::Call:
          if (inst.dest != -1) killSlot(inst.dest);
          break;
        default:
          break;
      }
    }
    return st;
  };

  // Dataflow: IN[BB] = intersection of OUT[preds]. For avail, require same
  // defSlot. For known, require same value (intersection). They're coupled:
  // an expr's encoding depends on known, so we carry both together.
  std::vector<StateMap> in(cfg.blocks.size());
  std::vector<StateMap> out(cfg.blocks.size());

  auto mergeState = [](const StateMap &a, const StateMap &b) -> StateMap {
    StateMap r;
    // Merge avail: keep entries where both agree on defSlot.
    for (const auto &kv : a.avail) {
      auto it = b.avail.find(kv.first);
      if (it != b.avail.end() && it->second == kv.second)
        r.avail[kv.first] = kv.second;
    }
    // Merge known: keep entries where both agree on value.
    for (const auto &kv : a.known) {
      auto it = b.known.find(kv.first);
      if (it != b.known.end() && it->second == kv.second)
        r.known[kv.first] = kv.second;
    }
    return r;
  };

  bool dfChanged = true;
  int dfIter = 0;
  while (dfChanged && dfIter < 20) {
    dfChanged = false;
    ++dfIter;
    for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
      StateMap newIn;
      if (b == cfg.entryBlock) {
        // entry: empty
      } else if (!cfg.blocks[b].preds.empty()) {
        newIn = out[cfg.blocks[b].preds[0]];
        for (std::size_t k = 1; k < cfg.blocks[b].preds.size(); ++k) {
          newIn = mergeState(newIn, out[cfg.blocks[b].preds[k]]);
        }
      }
      auto newOut = transferFn(newIn, cfg.blocks[b]);
      if (newIn.avail != in[b].avail || newOut.avail != out[b].avail ||
          newIn.known != in[b].known || newOut.known != out[b].known) {
        in[b] = std::move(newIn);
        out[b] = std::move(newOut);
        dfChanged = true;
      }
    }
  }

  // Rewrite phase: walk each BB seeded with IN[b]. When a Binary's expr is in
  // `avail` and the prior def is still live (not killed since), rewrite to
  // Move dest <- priorDef.
  bool any = false;
  for (std::size_t b = 0; b < cfg.blocks.size(); ++b) {
    StateMap st = in[b];
    auto &avail = st.avail;
    auto &known = st.known;
    auto killSlot = [&](int slot) {
      if (slot == -1) return;
      known.erase(slot);
      for (auto it = avail.begin(); it != avail.end();) {
        if (it->second == slot || exprA(it->first) == slot ||
            exprB(it->first) == slot)
          it = avail.erase(it);
        else
          ++it;
      }
    };
    for (std::size_t i = cfg.blocks[b].startIdx; i <= cfg.blocks[b].endIdx; ++i) {
      auto &inst = fn.instructions[i];
      switch (inst.op) {
        case Op::Const:
          if (inst.dest != -1) { killSlot(inst.dest); known[inst.dest] = inst.value; }
          break;
        case Op::Move: {
          if (inst.dest != -1) killSlot(inst.dest);
          auto it = known.find(inst.lhs);
          if (it != known.end() && inst.dest != -1) known[inst.dest] = it->second;
          break;
        }
        case Op::Unary: {
          if (inst.dest != -1) killSlot(inst.dest);
          break;
        }
        case Op::Binary: {
          const bool selfUpdate =
              (inst.dest == inst.lhs) || (inst.dest == inst.rhs);
          if (!selfUpdate && inst.lhs != -1 && inst.rhs != -1) {
            uint64_t key = encodeExpr(inst.binary, inst.lhs, inst.rhs, known);
            auto it = avail.find(key);
            if (it != avail.end() && it->second != inst.dest &&
                it->second != inst.lhs && it->second != inst.rhs) {
              inst.op = Op::Move;
              inst.lhs = it->second;
              inst.rhs = -1;
              inst.binary = ir::BinaryOp::Add;
              any = true;
              if (inst.dest != -1) killSlot(inst.dest);
              break;
            }
          }
          if (inst.dest != -1) killSlot(inst.dest);
          if (!selfUpdate && inst.dest != -1 && inst.lhs != -1 &&
              inst.rhs != -1) {
            uint64_t key = encodeExpr(inst.binary, inst.lhs, inst.rhs, known);
            avail[key] = inst.dest;
          }
          auto lk = known.find(inst.lhs);
          auto rk = known.find(inst.rhs);
          if (lk != known.end() && rk != known.end() && inst.dest != -1)
            known[inst.dest] = foldConstBinary(inst.binary, lk->second, rk->second);
          break;
        }
        case Op::LoadGlobal:
        case Op::Call:
          if (inst.dest != -1) killSlot(inst.dest);
          break;
        default:
          break;
      }
    }
  }
  return any;
}

void optimizeFunction(ir::Function &fn,
                      const std::unordered_set<std::string> &pureFunctions) {
  tailCallPass(fn);  // one-shot restructure before the fixed-point loop
  // Global dataflow passes run once up front (cross-BB const/copy/CSE), then
  // the local passes iterate to a fixed point. Running them inside the
  // 16-iter loop would be wasteful.
  globalConstPropPass(fn);
  globalCopyPropPass(fn);
  globalCsePass(fn);
  auto dumpPass = [&](const char *name) {
    if (!getenv("TOYCC_DUMP_PASS")) return;
    fprintf(stderr, "--- %s : %s ---\n", fn.name.c_str(), name);
    for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
      const auto &ins = fn.instructions[i];
      fprintf(stderr, "  [%zu] op=%d dest=%d lhs=%d rhs=%d v=%d bin=%d lab=%s\n",
          i, (int)ins.op, ins.dest, ins.lhs, ins.rhs, ins.value,
          (int)ins.binary, ins.label.c_str());
    }
  };
  for (int iter = 0; iter < 16; ++iter) {
    bool any = false;
    bool changed = false;
    changed = constFoldPass(fn); any |= changed; dumpPass("constFold");
    changed = branchFoldPass(fn); any |= changed; dumpPass("branchFold");
    changed = deadBlockPass(fn); any |= changed; dumpPass("deadBlock");
    changed = gotoCleanupPass(fn); any |= changed; dumpPass("gotoCleanup");
    changed = copyPropPass(fn); any |= changed; dumpPass("copyProp");
    changed = csePass(fn); any |= changed; dumpPass("cse");
    changed = copyCoalescePass(fn); any |= changed; dumpPass("copyCoalesce");
    changed = instCombinePass(fn); any |= changed; dumpPass("instCombine");
    changed = licmPass(fn, pureFunctions); any |= changed; dumpPass("licm");
    changed = loopSumElimPass(fn); any |= changed; dumpPass("loopSumElim");
    changed = dcePass(fn); any |= changed; dumpPass("dce");
    if (getenv("TOYCC_DUMP_IR")) {
      fprintf(stderr, "=== %s after iter %d ===\n", fn.name.c_str(), iter);
      for (std::size_t i = 0; i < fn.instructions.size(); ++i) {
        const auto &ins = fn.instructions[i];
        fprintf(stderr, "  [%zu] op=%d dest=%d lhs=%d rhs=%d v=%d bin=%d lab=%s\n",
            i, (int)ins.op, ins.dest, ins.lhs, ins.rhs, ins.value,
            (int)ins.binary, ins.label.c_str());
      }
    }
    if (!any) break;
  }
}

// ---------------------------------------------------------------------------
// Function inlining.
//
// Operates on the flat per-function instruction list. A Call to a small,
// loop-free, non-recursive, side-effect-free callee is expanded in place:
//   - callee body instructions are copied with every slot shifted by a
//     fresh base offset so they cannot collide with the caller's slots;
//   - reads of a callee paramSlot are rewritten to the corresponding
//     caller-side argument slot (direct argument substitution — no Move);
//   - callee `Return lhs` becomes `Move callDest <- retSlot; Goto next`;
//     `ReturnVoid` becomes `Goto next`.
// After inlining, the existing local passes (constFold/copyProp/dce) clean
// up: when all arguments are constants, the callee body folds to a single
// Const — this is how `compute(5,7,11)` collapses to `return 122`.
//
// Recursion is handled with a call graph + Tarjan SCC: any function in a
// cycle (including direct self-recursion) is never inlined, so expansion
// always terminates. Purity: a function with a StoreGlobal is still inlinable
// (moving the store into the caller is semantically equivalent), but a
// function that itself contains a Call is only inlined if that nested callee
// is also inlinable-and-already-inlined — to keep this pass simple and
// terminating we refuse to inline any function that contains a Call to a
// function not yet confirmed leaf. In practice we just refuse functions
// containing any Call at all (leaf-only inlining), which covers the perf
// cases (compute, adj) and stays safe.

// Collect the set of callee names referenced by `fn`.
std::unordered_set<std::string> calleesOf(const ir::Function &fn) {
  std::unordered_set<std::string> out;
  for (const auto &i : fn.instructions)
    if (i.op == Op::Call) out.insert(i.name);
  return out;
}

// Does `fn` contain a loop? Delegates to the shared CFG helper: a back-edge
// (a successor block index ≤ the current block index) is a reliable loop
// signal. The earlier implementation scanned for Goto-to-earlier-Label, which
// is equivalent but duplicated the CFG logic.
bool hasLoop(const ir::Function &fn) {
  return hasBackEdge(buildCFG(fn));
}

// Is `fn` a leaf (contains no Call)? Leaf-only inlining keeps termination
// trivial and covers the hot cases (compute, adj).
bool isLeaf(const ir::Function &fn) {
  for (const auto &i : fn.instructions)
    if (i.op == Op::Call) return false;
  return true;
}

// Tarjan SCC over the call graph to find functions involved in any cycle.
// Returns the set of function names that are *not* inlinable due to being
// in a recursive cycle (self-recursion or mutual). A function that calls
// itself directly is its own cycle.
std::unordered_set<std::string> findRecursive(const std::vector<ir::Function> &fns) {
  std::unordered_map<std::string, int> id;
  std::vector<std::string> names;
  for (const auto &f : fns) {
    if (id.count(f.name)) continue;
    id[f.name] = static_cast<int>(names.size());
    names.push_back(f.name);
  }
  const int n = static_cast<int>(names.size());
  std::vector<std::vector<int>> adj(n);
  for (const auto &f : fns) {
    int u = id[f.name];
    for (const auto &c : calleesOf(f)) {
      auto it = id.find(c);
      if (it != id.end()) adj[u].push_back(it->second);
    }
  }
  // Tarjan
  std::vector<int> idx(n, -1), low(n, 0), onStack(n, 0);
  std::vector<int> stack;
  int index = 0;
  std::unordered_set<std::string> inCycle;
  // iterative Tarjan to avoid deep recursion on large call graphs
  for (int s = 0; s < n; ++s) {
    if (idx[s] != -1) continue;
    std::vector<std::pair<int, int>> work;  // (node, nextNeighborIndex)
    work.push_back({s, 0});
    while (!work.empty()) {
      auto &top = work.back();
      int v = top.first;
      if (idx[v] == -1) {
        idx[v] = low[v] = index++;
        stack.push_back(v);
        onStack[v] = 1;
      }
      if (top.second < static_cast<int>(adj[v].size())) {
        int w = adj[v][top.second++];
        if (idx[w] == -1) {
          work.push_back({w, 0});
          continue;
        }
        if (onStack[w]) low[v] = std::min(low[v], idx[w]);
      } else {
        if (low[v] == idx[v]) {
          // pop SCC rooted at v
          std::vector<int> comp;
          int w;
          do {
            w = stack.back();
            stack.pop_back();
            onStack[w] = 0;
            comp.push_back(w);
          } while (w != v);
          if (comp.size() > 1) {
            for (int c : comp) inCycle.insert(names[c]);
          } else {
            // singleton — still recursive if it calls itself
            int c = comp[0];
            for (int nb : adj[c])
              if (nb == c) { inCycle.insert(names[c]); break; }
          }
        }
        work.pop_back();
        if (!work.empty()) {
          int parent = work.back().first;
          low[parent] = std::min(low[parent], low[v]);
        }
      }
    }
  }
  return inCycle;
}

// Rewrite every slot operand of `inst` that is a callee paramSlot to the
// corresponding caller argument slot, using `paramToArg`. Non-param slots are
// shifted by `base`.
void remapInstSlots(Inst &inst, int base,
                    const std::unordered_map<int, int> &paramToArg) {
  auto xform = [&](int s) -> int {
    if (s == -1) return s;
    auto it = paramToArg.find(s);
    if (it != paramToArg.end()) return it->second;
    return s + base;
  };
  if (inst.op == Op::StoreGlobal) {
    inst.lhs = xform(inst.lhs);
    return;
  }
  if (inst.op == Op::Branch) {
    inst.lhs = xform(inst.lhs);
    return;
  }
  if (inst.op == Op::Return) {
    inst.lhs = xform(inst.lhs);
    return;
  }
  if (inst.op == Op::Move || inst.op == Op::Unary) {
    inst.lhs = xform(inst.lhs);
    inst.dest = xform(inst.dest);
    return;
  }
  if (inst.op == Op::Binary) {
    inst.lhs = xform(inst.lhs);
    inst.rhs = xform(inst.rhs);
    inst.dest = xform(inst.dest);
    return;
  }
  if (inst.op == Op::Const) {
    inst.dest = xform(inst.dest);
    return;
  }
  if (inst.op == Op::LoadGlobal) {
    inst.dest = xform(inst.dest);
    return;
  }
  if (inst.op == Op::Call) {
    for (auto &a : inst.args) a = xform(a);
    inst.dest = xform(inst.dest);
    return;
  }
  // Label/Goto/ReturnVoid have no slots.
}

// Inline every inlinable Call inside `caller` using the function table
// `byName`. Returns true if any Call was inlined. Mutates `caller` in place.
// `inlinable` precomputed per callee name.
bool inlineCallsInFunction(
    ir::Function &caller,
    const std::unordered_map<std::string, const ir::Function *> &byName,
    const std::unordered_set<std::string> &inlinable) {
  bool any = false;
  // iterate to fixed point: inlining may expose new calls? No — we only
  // inline leaf callees, so inlining a leaf cannot introduce new Calls.
  // But a single pass may have multiple call sites; rebuild once.
  std::vector<Inst> out;
  out.reserve(caller.instructions.size());
  for (std::size_t i = 0; i < caller.instructions.size(); ++i) {
    const auto &inst = caller.instructions[i];
    if (inst.op != Op::Call) {
      out.push_back(inst);
      continue;
    }
    auto it = byName.find(inst.name);
    if (it == byName.end() || !inlinable.count(inst.name)) {
      out.push_back(inst);
      continue;
    }
    const ir::Function &callee = *it->second;
    // Build paramSlot -> caller arg slot map (direct substitution).
    std::unordered_map<int, int> paramToArg;
    const std::size_t nargs = std::min(callee.paramSlots.size(), inst.args.size());
    for (std::size_t k = 0; k < nargs; ++k)
      paramToArg[callee.paramSlots[k]] = inst.args[k];
    // Fresh slot base for callee temps/locals.
    const int base = caller.slotCount;
    caller.slotCount += callee.slotCount;
    // Drop callee paramSlots from the shifted range: their reads are
    // substituted, but a callee may also *write* its paramSlot (e.g.
    // `i1 = i1 / 3`). That write must go to a fresh slot, not back to the
    // caller's arg slot. So if a paramSlot is ever defined in the callee,
    // we cannot substitute its reads after the def. Handle by making the
    // param slot map to a fresh slot, then emit a `Move fresh <- arg` at
    // the entry so the initial value is the argument.
    std::unordered_map<int, int> effectiveParam = paramToArg;
    // Detect which paramSlots are reassigned inside the callee.
    for (int p : callee.paramSlots) {
      bool reassigned = false;
      for (const auto &ci : callee.instructions) {
        if (definesSlot(ci) && ci.dest == p) { reassigned = true; break; }
      }
      if (reassigned) {
        int fresh = caller.slotCount++;
        effectiveParam[p] = fresh;
        // emit Move fresh <- arg (so the initial value equals the argument)
        Inst mv;
        mv.op = Op::Move;
        mv.dest = fresh;
        mv.lhs = paramToArg[p];
        out.push_back(mv);
      }
    }
    // Fresh label for the continuation after the inlined body. Use a
    // program-unique counter so that when THIS caller is itself inlined
    // elsewhere later, the continuation label (and every label inside the
    // callee body) cannot collide with labels already in the caller or in
    // other functions.
    static std::size_t globalInlineId = 0;
    const std::size_t siteId = globalInlineId++;
    const std::string cont = ".L_inl" + std::to_string(siteId) + "_cont";
    // Build a rename map for every label defined in the callee body. Each
    // gets a fresh unique name so that copying the body into the caller
    // cannot duplicate a label that the callee still owns (the callee remains
    // emitted as a standalone function), nor collide with labels from other
    // inline sites.
    std::unordered_map<std::string, std::string> labelRename;
    {
      std::size_t labelIdx = 0;
      for (const auto &ci : callee.instructions)
        if (ci.op == Op::Label && !ci.label.empty()) {
          labelRename.emplace(ci.label,
              ".L_inl" + std::to_string(siteId) + "_l" + std::to_string(labelIdx++));
        }
    }
    auto renameLabel = [&](std::string &s) {
      if (s.empty()) return;
      auto it = labelRename.find(s);
      if (it != labelRename.end()) s = it->second;
    };
    // Copy callee body, remap slots, rename labels, convert Return/ReturnVoid.
    for (const auto &ci : callee.instructions) {
      Inst copy = ci;
      remapInstSlots(copy, base, effectiveParam);
      if (copy.op == Op::Label) renameLabel(copy.label);
      else if (copy.op == Op::Goto) renameLabel(copy.label);
      else if (copy.op == Op::Branch) { renameLabel(copy.label); renameLabel(copy.falseLabel); }
      if (copy.op == Op::Return) {
        Inst mv;
        mv.op = Op::Move;
        mv.dest = inst.dest;  // the Call's dest slot in the caller
        mv.lhs = copy.lhs;
        out.push_back(mv);
        Inst g;
        g.op = Op::Goto;
        g.label = cont;
        out.push_back(g);
      } else if (copy.op == Op::ReturnVoid) {
        Inst g;
        g.op = Op::Goto;
        g.label = cont;
        out.push_back(g);
      } else {
        out.push_back(copy);
      }
    }
    out.push_back({});  // Label cont
    out.back().op = Op::Label;
    out.back().label = cont;
    any = true;
  }
  if (any) caller.instructions = std::move(out);
  return any;
}

}  // namespace

// Program-level inlining driver. First runs local passes on every function
// so dead args / dead locals shrink callee bodies (a DCE-heavy callee like
// `void func(i){ int x1=1;...; global=i; }` collapses to one store and
// becomes cheap to inline). Then marks leaf, loop-free, non-recursive,
// small callees as inlinable, inlines their call sites, and re-runs local
// passes so folded constants / dead defs from inlining are cleaned up.
// The outer loop repeats: inlining can turn a caller into a leaf that is
// now itself inlinable elsewhere.
void inlineProgram(ir::Program &program,
                   const std::unordered_set<std::string> &pureFunctions) {
  constexpr int kMaxBody = 400;  // callee body instruction budget (post-cleanup)
  const auto recursive = findRecursive(program.functions);
  std::unordered_map<std::string, const ir::Function *> byName;
  for (const auto &f : program.functions) byName[f.name] = &f;

  // Pre-shrink every function with local passes so inlinability is judged on
  // the optimized body, not the raw IR (DCE removes dead locals, constFold
  // folds constant arg-driven computations, etc.).
  for (auto &f : program.functions) optimizeFunction(f, pureFunctions);
  byName.clear();
  for (auto &f : program.functions) byName[f.name] = &f;

  for (int outer = 0; outer < 6; ++outer) {
    // Mark inlinable callees based on current (post-cleanup) bodies.
    std::unordered_set<std::string> inlinable;
    for (const auto &f : program.functions) {
      if (recursive.count(f.name)) continue;
      if (!isLeaf(f)) continue;
      if (hasLoop(f)) continue;
      if (f.instructions.size() > kMaxBody) continue;
      inlinable.insert(f.name);
    }
    // Inline in callers; rebuild each caller once per sweep.
    bool any = false;
    for (auto &f : program.functions) any |= inlineCallsInFunction(f, byName, inlinable);
    if (!any) break;
    // Cleanup so the next outer iteration judges shrunken bodies and so
    // inlined constants fold.
    for (auto &f : program.functions) optimizeFunction(f, pureFunctions);
    byName.clear();
    for (auto &f : program.functions) byName[f.name] = &f;
  }
}

// Dead function elimination. After inlining, some callees are no longer
// referenced from `main` (or anywhere reachable). ToyC has no function
// pointers, so a function is reachable iff transitively called from `main`.
// Removing dead functions shrinks the emitted .text section — important for
// platform startup cost on small test binaries.
void deadFunctionElimination(ir::Program &program) {
  std::unordered_map<std::string, const ir::Function *> byName;
  for (const auto &f : program.functions) byName[f.name] = &f;
  if (!byName.count("main")) return;
  std::unordered_set<std::string> alive;
  std::vector<std::string> stack{"main"};
  while (!stack.empty()) {
    std::string name = stack.back();
    stack.pop_back();
    if (alive.count(name)) continue;
    alive.insert(name);
    auto it = byName.find(name);
    if (it == byName.end()) continue;
    for (const auto &inst : it->second->instructions)
      if (inst.op == Op::Call) stack.push_back(inst.name);
  }
  std::vector<ir::Function> kept;
  kept.reserve(program.functions.size());
  for (auto &f : program.functions)
    if (alive.count(f.name)) kept.push_back(std::move(f));
  program.functions = std::move(kept);
}

void optimizeProgram(ir::Program &program) {
  // Compute purity once at the start. Purity is stable across inlining: if a
  // callee has StoreGlobal, inlining copies that StoreGlobal into the caller,
  // so the caller's purity doesn't change. Computing once avoids re-running
  // the fixed-point analysis every optimization round.
  const auto pureFunctions = computePureFunctions(program.functions);
  inlineProgram(program, pureFunctions);
  // Local passes already ran inside inlineProgram; one final sweep ensures a
  // fixed point.
  for (auto &fn : program.functions) optimizeFunction(fn, pureFunctions);
  // Drop functions that are no longer reachable from `main` after inlining.
  deadFunctionElimination(program);
}

}  // namespace toycc::ir