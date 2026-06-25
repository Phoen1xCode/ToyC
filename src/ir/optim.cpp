#include "ir/optim.hpp"

#include <algorithm>
#include <cstdint>
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

bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }
int logPowerOfTwo(int v) {  // requires power of 2
  int r = 0;
  while ((1 << r) < v) ++r;
  return r;
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
    // value-numbering map: slot -> canonical key. For constants the key is
    // encoded as a high-bit-tagged integer so it cannot collide with raw slot
    // numbers; for non-constants it is just the slot id itself.
    std::unordered_map<int, int> known;  // slot -> constant value
    std::unordered_map<uint64_t, int> seen;  // key -> dest slot
    auto cur = itbeg;
    while (cur != fn.instructions.end() && !isControlBoundary(*cur)) {
      if (cur->op == Op::Label) break;
      auto &inst = *cur;
      if (inst.op == Op::Const) {
        known[inst.dest] = inst.value;
      }
      // Encode operand: if slot is known-const, use a tagged value-key
      // (high bit set is impossible for a non-negative slot id).
      auto enc = [&](int slot) -> int {
        auto it = known.find(slot);
        if (it == known.end()) return slot;  // raw slot id
        // mask to 24-bit constant value with top tag bit set
        return 0x40000000 | (it->second & 0x3FFFFF);
      };
      if (inst.op == Op::Binary) {
        auto op = static_cast<int>(inst.binary);
        int a = enc(inst.lhs);
        int b = enc(inst.rhs);
        bool commute = (inst.binary == ir::BinaryOp::Add) || (inst.binary == ir::BinaryOp::Mul);
        if (commute && a > b) std::swap(a, b);
        uint64_t key =
            (uint64_t)(op & 0xFFFF) << 48 | (uint64_t)(a & 0xFFFFFFFF) << 16 | (b & 0xFFFF);
        auto it = seen.find(key);
        if (it != seen.end()) {
          int prev = it->second;
          inst.op = Op::Move;
          inst.lhs = prev;
          inst.rhs = -1;
          inst.binary = ir::BinaryOp::Add;
          any = true;
        } else {
          seen[key] = inst.dest;
        }
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

void optimizeFunction(ir::Function &fn) {
  for (int iter = 0; iter < 16; ++iter) {
    bool any = false;
    any |= constFoldPass(fn);
    any |= copyPropPass(fn);
    any |= csePass(fn);
    any |= copyCoalescePass(fn);
    any |= dcePass(fn);
    if (!any) break;
  }
}

}  // namespace

void optimizeProgram(ir::Program &program) {
  for (auto &fn : program.functions) optimizeFunction(fn);
}

}  // namespace toycc::ir