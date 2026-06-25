#include "backend/riscv.hpp"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "support/diagnostic.hpp"

namespace toycc::backend {
namespace {

// Magic-number signed division (Hacker's Delight, Fig. 10-1). For a divisor
// d (d != 0, d != ±1, d != ±2^k) compute (M, s, addMarker) so that
//     x / d  ==  (signed_high32(x * M) [+ x if addMarker]) >>arith s  +  (x<0?1:0)
// where signed_high32 is the upper 32 bits of the signed 64-bit product.
// addMarker is true when |M| does not fit in 32 signed bits and we need to
// add x back after the high-multiply to recover the right magnitude.
//
// `d` must satisfy d != 0 && d != 1 && d != -1 && d is not a power of two
// (or negative power of two). Callers handle those cases separately.
struct DivMagic {
  std::int32_t M;
  int s;
  bool addMarker;   // need an extra `add a0, hi, x` step
};
DivMagic computeDivMagic(int d) {
  const std::uint32_t twoP31 = 0x80000000u;
  const std::uint32_t ad = static_cast<std::uint32_t>(d < 0 ? -static_cast<long long>(d) : d);
  const std::uint32_t t = twoP31 + (static_cast<std::uint32_t>(d) >> 31);  // 2^31 + (d<0?1:0)
  const std::uint32_t anc = t - 1 - t % ad;  // |nc|
  int p = 31;
  std::uint32_t q1 = twoP31 / anc;
  std::uint32_t r1 = twoP31 - q1 * anc;
  std::uint32_t q2 = twoP31 / ad;
  std::uint32_t r2 = twoP31 - q2 * ad;
  std::uint32_t delta;
  do {
    ++p;
    q1 *= 2; r1 *= 2;
    if (r1 >= anc) { ++q1; r1 -= anc; }
    q2 *= 2; r2 *= 2;
    if (r2 >= ad) { ++q2; r2 -= ad; }
    delta = ad - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  std::int32_t M = static_cast<std::int32_t>(q2 + 1);
  if (d < 0) M = -M;
  DivMagic out;
  out.M = M;
  out.s = p - 32;
  out.addMarker = (d > 0 && M < 0) || (d < 0 && M > 0);
  return out;
}

int slotOffset(int slot, int savedSRegs) {
  // 局部社于 s-reg 保存区之下：ra/s0 在 s0-4..s0-8，随后被保存的
  // s2..s11（占 savedSRegs 个），再向下才是社区。未分配时 savedSRegs=0。
  return -12 - savedSRegs * 4 - slot * 4;
}

// 返回指令读取的社位号（用于寄存器分配的使用频率统计）。
std::vector<int> readsOf(const ir::Instruction &i) {
  std::vector<int> out;
  using Op = ir::Instruction::Op;
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


class FunctionEmitter {
 public:
  FunctionEmitter(const ir::Function &func, std::ostream &out) : func_(func), out_(out) {}

  void emit() {
    allocateRegisters();
    computeConstSlots();
    frameSize_ = computeFrameSize();
    collectBranchTargets();
    out_ << "  .text\n"
         << "  .globl " << func_.name << "\n"
         << func_.name << ":\n";
    emitAddSp(-frameSize_);
    emitSwReg("ra", frameSize_ - 4, "sp");
    emitSwReg("s0", frameSize_ - 8, "sp");
    for (std::size_t i = 0; i < savedSRegs_.size(); ++i)
      emitSwReg(savedSRegs_[i], frameSize_ - 12 - static_cast<int>(i) * 4, "sp");
    emitAddiReg("s0", "sp", frameSize_);

    for (std::size_t i = 0; i < func_.paramSlots.size(); ++i) {
      const int slot = func_.paramSlots[i];
      // 参数若被分配到 s-reg，prologue 把实参搬进 s-reg（callee-saved，跨
      // 函数内部调用安全）。前 8 个参数在 a-reg：`mv sX, aY`；溢出参数在调用
      // 方栈上（相对 s0 偏移 (i-8)*4）：`lw sX, (i-8)*4(s0)`。否则落栈到栈槽。
      auto rm = regMap_.find(slot);
      if (rm != regMap_.end()) {
        if (i < 8) {
          out_ << "  mv " << rm->second << ", a" << i << "\n";
        } else {
          emitLwReg(rm->second, (static_cast<int>(i) - 8) * 4, "s0");
        }
        continue;
      }
      const int offset = slotOffset(slot);
      if (i < 8) {
        emitSwReg("a" + std::to_string(i), offset, "s0");
      } else {
        emitLwReg("t0", (static_cast<int>(i) - 8) * 4, "s0");
        emitSwReg("t0", offset, "s0");
      }
    }

    for (const auto &inst : func_.instructions) emitInst(inst);
    out_ << returnLabel() << ":\n";
    // Restore callee-saved s-regs FIRST (while s0 still points at this frame),
    // then ra, then s0 last — otherwise reloading s0 first invalidates the
    // base register used for the rest of the lw's.
    for (std::size_t i = 0; i < savedSRegs_.size(); ++i)
      emitLwReg(savedSRegs_[i], -12 - static_cast<int>(i) * 4, "s0");
    emitLwReg("ra", -4, "s0");
    emitLwReg("s0", -8, "s0");
    emitAddSp(frameSize_);
    out_ << "  ret\n";
  }

 private:
  std::string returnLabel() const { return ".L_" + func_.name + "_return"; }

  // 逆扫描一遇反过来：报Branch/Goto 的标号标为“汇合点”，需要在 Label 处
  // 清空 cache；未被任何分支跳转的 Label（单附从 fallback）可以沿用 cache，
  // 对于循环的 body_label 恰好保留循环体进入载荷后的负载区间。
  std::unordered_set<std::string> branchTargets_;
  void collectBranchTargets() {
    for (const auto &inst : func_.instructions) {
      if (inst.op == ir::Instruction::Op::Goto) branchTargets_.insert(inst.label);
      else if (inst.op == ir::Instruction::Op::Branch) {
        if (!inst.label.empty()) branchTargets_.insert(inst.label);
        if (!inst.falseLabel.empty()) branchTargets_.insert(inst.falseLabel);
      }
    }
    branchTargets_.insert(returnLabel());
  }

  // --- big-immediate helpers ------------------------------------------------
  // emit "addi rd, rs, imm" splitting if |imm| > 2047
  void emitAddiReg(const std::string &rd, const std::string &rs, int imm) {
    if (imm >= -2048 && imm <= 2047) {
      out_ << "  addi " << rd << ", " << rs << ", " << imm << "\n";
      return;
    }
    out_ << "  li t6, " << imm << "\n"
         << "  add " << rd << ", " << rs << ", t6\n";
  }

  // emit "addi sp, sp, imm" splitting if needed
  void emitAddSp(int imm) { emitAddiReg("sp", "sp", imm); }

  // emit "sw reg, offset(base)" splitting large offsets through t6
  void emitSwReg(const std::string &reg, int offset, const std::string &base) {
    if (offset >= -2048 && offset <= 2047) {
      out_ << "  sw " << reg << ", " << offset << "(" << base << ")\n";
      return;
    }
    out_ << "  li t6, " << offset << "\n"
         << "  add t6, " << base << ", t6\n"
         << "  sw " << reg << ", 0(t6)\n";
  }

  // emit "lw reg, offset(base)" splitting large offsets through t6
  void emitLwReg(const std::string &reg, int offset, const std::string &base) {
    if (offset >= -2048 && offset <= 2047) {
      out_ << "  lw " << reg << ", " << offset << "(" << base << ")\n";
      return;
    }
    out_ << "  li t6, " << offset << "\n"
         << "  add t6, " << base << ", t6\n"
         << "  lw " << reg << ", 0(t6)\n";
  }

  // 多槽 LRU 寄存器缓存窥孔：同时保存最近写入最多 8 个 slot 的“在本寄存
  // 器里有一份最新值”的事实。load 命中时直接 mv，避免冗余 lw；store 后
  // 在 reg 上登记 slot。任何跨越控制流都会全部失效。
  // reg -> slot 单向不重复；slot -> reg 也唯一。
  struct CacheRow { int slot; std::string reg; };
  std::vector<CacheRow> cache_;
  // 反方向跟踪：t0 / a0 / 等"工作寄存器"上次被 mv 自哪个 s-reg。下次
  // loadSlot 同一 regAllocated slot 时若 reg 仍持有相同 s-reg 值（即中间
  // 没有写过这个 reg），跳过 mv。invalidateCache 同步清空。
  std::unordered_map<std::string, std::string> sRegHeld_;
  static constexpr int kCacheCap = 16;

  void dropReg(const std::string &reg) {
    cache_.erase(std::remove_if(cache_.begin(), cache_.end(),
        [&](const CacheRow &c) { return c.reg == reg; }), cache_.end());
  }
  void dropSlot(int slot) {
    cache_.erase(std::remove_if(cache_.begin(), cache_.end(),
        [&](const CacheRow &c) { return c.slot == slot; }), cache_.end());
  }
  const CacheRow *findCached(int slot) const {
    for (const auto &c : cache_) if (c.slot == slot) return &c;
    return nullptr;
  }
  void promote(int slot) {
    auto it = std::find_if(cache_.begin(), cache_.end(),
        [&](const CacheRow &c) { return c.slot == slot; });
    if (it != cache_.end() && it + 1 != cache_.end()) {
      CacheRow row = *it; cache_.erase(it); cache_.push_back(row);
    }
  }
  void addCache(int slot, const std::string &reg) {
    dropSlot(slot);
    cache_.push_back({slot, reg});
    while (static_cast<int>(cache_.size()) > kCacheCap) cache_.erase(cache_.begin());
  }
  void invalidateCache() { cache_.clear(); sRegHeld_.clear(); }

  // 写入工作寄存器时通知 cache：该 reg 上所有旧 slot 映射失效。
  void killReg(const std::string &reg) { dropReg(reg); sRegHeld_.erase(reg); }

  // 选出使用频率最高的若干用户命名社位（VarDecl + 形参）映射到 s2..s11。
  // 这些通常是循环中贯穿的归纳变量/累加器/形参，入 s-reg 后避免每次 lw/sw。
  // 形参与 VarDecl 一视同仁参与分配：被调方 prologue 用 `mv sX, aY` 把实参
  // 从 a-reg 搬到分配的 s-reg（callee-saved，跨函数内部调用安全）。
  std::unordered_map<int, std::string> regMap_;
  std::vector<std::string> savedSRegs_;
  static constexpr int kMaxRegAlloc = 10;  // s2..s11
  void allocateRegisters() {
    if (func_.namedSlots.empty()) return;
    std::unordered_map<int, int> uses;
    for (const auto &inst : func_.instructions)
      for (int s : readsOf(inst)) ++uses[s];
    // VarDecl 与形参都参与分配。
    std::vector<std::pair<int,int>> ranked;
    for (int s : func_.namedSlots) {
      int u = uses.count(s) ? uses[s] : 0;
      if (u > 0) ranked.emplace_back(u, s);
    }
    auto cmp = [](const std::pair<int,int>&a, const std::pair<int,int>&b){
      if (a.first != b.first) return a.first > b.first;
      return a.second < b.second;
    };
    std::sort(ranked.begin(), ranked.end(), cmp);
    int pick = std::min<int>(kMaxRegAlloc, static_cast<int>(ranked.size()));
    for (int i = 0; i < pick; ++i) {
      int s = ranked[i].second;
      const std::string r = "s" + std::to_string(2 + i);
      regMap_[s] = r;
      savedSRegs_.push_back(r);
    }
  }

  int slotOffset(int slot) const {
    return ::toycc::backend::slotOffset(slot, static_cast<int>(savedSRegs_.size()));
  }

  int computeFrameSize() const {
    const int localBytes =
        8 + static_cast<int>(savedSRegs_.size()) * 4 + func_.slotCount * 4;
    return std::max(16, ((localBytes + 15) / 16) * 16);
  }

  bool isRegAllocated(int slot) const { return regMap_.find(slot) != regMap_.end(); }

  // 把 slot 当前在 cache 中的 reg 名称返回；若不在 cache 但是 regAllocated
  // 则返回 s-reg；否则返回空字符串。仅在严格的"立即可用"路径上调用。
  std::string slotInReg(int slot) const {
    auto m = regMap_.find(slot);
    if (m != regMap_.end()) return m->second;
    for (const auto &c : cache_) if (c.slot == slot) return c.reg;
    return {};
  }

  // 把"只被定义为 Op::Const 一次且永不被其他指令重写"的 slot 编入常量表。
  // 后端在 emitBinary 中据此把 mul/div/rem 常量优化为 shift/and/addi 等等，
  // 也可以把常量短直接 li 到工作寄存器，省略 sw/lw 的中间往返。
  std::unordered_map<int, int> slotConst_;
  // 单次使用的临时 slot 集合（用于跳过 sw 的窥孔）。一个 slot 进入此集合
  // 当且仅当：① 仅有一次 IR-读取，② 不是 namedSlot（不是用户变量），
  // ③ 不是 paramSlot（参数有自己的栈位约束）。命中时 storeSlot 跳过 sw、
  // 仅写入 cache。下一条读取由 cache hit 用 mv 完成。
  // 计算"可省略 sw 的 slot 集"。条件：
  //   ① 该 slot 只有一次读 (reads==1)
  //   ② 唯一的读取指令在 def 之后、第一个控制流屏障 / a0-overwrite 之前
  //   ③ 该读取指令本身只通过 loadSlot 来访问
  // 实现：精准地标"def-index"，向前扫描到 reader-index，期间不能跨越
  // Label/Goto/Branch/Return/Call。读取指令必须就是下一条非 Label 指令。
  std::unordered_set<int> skipStore_;
  void computeConstSlots() {
    std::unordered_map<int, int> defs;  // slot -> def count
    std::unordered_map<int, int> values;
    std::unordered_map<int, int> reads;
    using Op = ir::Instruction::Op;
    for (const auto &i : func_.instructions) {
      for (int s : readsOf(i)) ++reads[s];
      switch (i.op) {
        case Op::Const:
          if (i.dest != -1) {
            defs[i.dest] += 1;
            if (defs[i.dest] == 1) values[i.dest] = i.value;
          }
          break;
        case Op::Move:
        case Op::LoadGlobal:
        case Op::Unary:
        case Op::Binary:
        case Op::Call:
          if (i.dest != -1) defs[i.dest] += 2;  // mark non-const def
          break;
        default:
          break;
      }
    }
    for (const auto &kv : defs)
      if (kv.second == 1) slotConst_[kv.first] = values[kv.first];

    // 单读 + 单定义、非参数；用户命名的局部变量也算（短生命期临时常常
    // 被用户用 int x = ...; ... x 单次消费的形式写出来）。寄存器分配的
    // 槽位由其它通路处理，这里允许通过——storeSlot 看到 regMap_ 命中
    // 会先一步发出 mv 不走 sw 路径。
    std::unordered_set<int> paramSet(func_.paramSlots.begin(), func_.paramSlots.end());
    std::unordered_set<int> oneReadOneDefTemps;
    for (const auto &kv : defs) {
      const int slot = kv.first;
      if (paramSet.count(slot)) continue;
      auto rit = reads.find(slot);
      if (rit == reads.end() || rit->second != 1) continue;
      oneReadOneDefTemps.insert(slot);
    }

    // skipStore_: 严格条件——定义 slot 的指令之后，紧接着（允许跨越纯
    // fall-through 标签，不允许跨越任何写 a0 的指令）的下一条指令就是
    // 唯一的读者。读者必须是不会篡改 a0 的 loadSlot 调用者（Branch/
    // Return/Move/StoreGlobal 等）。
    // 简化判据：紧邻的下一条非 Label 指令必须读 slot 作为其首个/唯一操作数，
    // 且读取后立即被消费（Branch/Return）。
    for (std::size_t i = 0; i < func_.instructions.size(); ++i) {
      const auto &inst = func_.instructions[i];
      if (inst.dest == -1 || !oneReadOneDefTemps.count(inst.dest)) continue;
      if (inst.op != ir::Instruction::Op::Binary &&
          inst.op != ir::Instruction::Op::Unary &&
          inst.op != ir::Instruction::Op::Const) continue;
      const int defSlot = inst.dest;
      // 向后跳过 Label 与"会被 backend 完全省略"的 Const 指令（它们的
      // dest 已在 slotConst_ 且非 regAllocated；loadSlot 后续直接 li，不
      // 会侵犯 cache）。
      std::size_t j = i + 1;
      while (j < func_.instructions.size()) {
        const auto &mid = func_.instructions[j];
        if (mid.op == ir::Instruction::Op::Label) { ++j; continue; }
        if (mid.op == ir::Instruction::Op::Const) {
          if (mid.dest != -1 && slotConst_.count(mid.dest) &&
              slotConst_[mid.dest] == mid.value &&
              !regMap_.count(mid.dest)) {
            ++j; continue;  // 这个 Const 不会发射任何指令，可越过
          }
        }
        break;
      }
      if (j == func_.instructions.size()) continue;
      const auto &nx = func_.instructions[j];
      bool ok = false;
      switch (nx.op) {
        case ir::Instruction::Op::Branch:
        case ir::Instruction::Op::Return:
        case ir::Instruction::Op::Unary:
        case ir::Instruction::Op::Move:
        case ir::Instruction::Op::StoreGlobal:
          ok = (nx.lhs == defSlot);
          break;
        case ir::Instruction::Op::Binary:
          ok = (nx.lhs == defSlot) || (nx.rhs == defSlot);
          break;
        default:
          ok = false;
          break;
      }
      if (ok) skipStore_.insert(defSlot);
    }
  }
  bool isConstSlot(int slot, int &outVal) const {
    auto it = slotConst_.find(slot);
    if (it == slotConst_.end()) return false;
    outVal = it->second;
    return true;
  }

  void loadSlot(const std::string &reg, int slot) {
    auto m = regMap_.find(slot);
    if (m != regMap_.end()) {
      // 跟踪 t0 / a0 上一次 mv 进来的 s-reg 值，避免每次都重复 mv。
      auto it = sRegHeld_.find(reg);
      if (it != sRegHeld_.end() && it->second == m->second) return;
      out_ << "  mv " << reg << ", " << m->second << "\n";
      sRegHeld_[reg] = m->second;
      // 普通 cache 中以 reg 为 key 的临时 slot 被覆盖
      dropReg(reg);
      return;
    }
    // 一旦从其他来源写 reg，sRegHeld_ 该 reg 标记失效
    sRegHeld_.erase(reg);
    int cv = 0;
    if (isConstSlot(slot, cv)) {
      std::string regs(reg);
      dropReg(regs);
      out_ << "  li " << reg << ", " << cv << "\n";
      return;
    }
    if (const CacheRow *c = findCached(slot)) {
      if (reg != c->reg) {
        out_ << "  mv " << reg << ", " << c->reg << "\n";
        std::string regs(reg);
        dropReg(regs);
        addCache(slot, regs);
      } else {
        promote(slot);
      }
      return;
    }
    std::string regs(reg);
    dropReg(regs);
    emitLwReg(reg, slotOffset(slot), "s0");
  }

  void storeSlot(const char *reg, int slot) {
    std::string regs(reg);
    auto m = regMap_.find(slot);
    if (m != regMap_.end()) {
      out_ << "  mv " << m->second << ", " << reg << "\n";
      return;
    }
    // 严格 peephole：slot 是"下一条指令就是它唯一读者，且读者只用 mv/
    // load 形式消费 a0"——直接跳过 sw、保持 cache 中即可。
    if (skipStore_.count(slot)) {
      dropSlot(slot);
      cache_.push_back({slot, regs});
      while (static_cast<int>(cache_.size()) > kCacheCap) cache_.erase(cache_.begin());
      return;
    }
    emitSwReg(reg, slotOffset(slot), "s0");
    addCache(slot, regs);
  }

  void emitInst(const ir::Instruction &inst) {
    switch (inst.op) {
      case ir::Instruction::Op::Label:
        if (branchTargets_.find(inst.label) != branchTargets_.end())
          invalidateCache();
        out_ << inst.label << ":\n";
        return;
      case ir::Instruction::Op::Goto:
        invalidateCache();
        out_ << "  j " << inst.label << "\n";
        return;
      case ir::Instruction::Op::Branch:
        loadSlot("a0", inst.lhs);
        if (!inst.label.empty()) out_ << "  bnez a0, " << inst.label << "\n";
        if (!inst.falseLabel.empty()) out_ << "  beqz a0, " << inst.falseLabel << "\n";
        // Branch 有跳转 falseLabel/Label 的可能，都会被 Label 重复 invalidate。
        // 这里不清 cache，从而 fall-through 的下一亲 Label 沿用 cache。
        return;
      case ir::Instruction::Op::Const:
        // 若 slot 是常量表中的成员，所有读取都会通过 loadSlot 直接 li，
        // 这里就完全省略 li+sw 的发射。但若 slot 是寄存器分配的，s-reg
        // 仍然需要初始化（loadSlot 的 regAllocated 通路不会查常量表）。
        {
          int cv = 0;
          if (isConstSlot(inst.dest, cv) && cv == inst.value &&
              !isRegAllocated(inst.dest)) {
            return;
          }
        }
        killReg("a0");
        out_ << "  li a0, " << inst.value << "\n";
        storeSlot("a0", inst.dest);
        return;
      case ir::Instruction::Op::Move:
        loadSlot("a0", inst.lhs);
        storeSlot("a0", inst.dest);
        return;
      case ir::Instruction::Op::LoadGlobal:
        killReg("t0"); killReg("a0");
        out_ << "  la t0, " << inst.name << "\n"
             << "  lw a0, 0(t0)\n";
        storeSlot("a0", inst.dest);
        return;
      case ir::Instruction::Op::StoreGlobal:
        killReg("t0");
        loadSlot("a0", inst.lhs);
        out_ << "  la t0, " << inst.name << "\n"
             << "  sw a0, 0(t0)\n";
        return;
      case ir::Instruction::Op::Unary:
        emitUnary(inst);
        return;
      case ir::Instruction::Op::Binary:
        emitBinary(inst);
        return;
      case ir::Instruction::Op::Call:
        emitCall(inst);
        return;
      case ir::Instruction::Op::Return:
        loadSlot("a0", inst.lhs);
        out_ << "  j " << returnLabel() << "\n";
        invalidateCache();
        return;
      case ir::Instruction::Op::ReturnVoid:
        invalidateCache();
        killReg("a0");
        out_ << "  li a0, 0\n"
             << "  j " << returnLabel() << "\n";
        return;
    }
  }

  void emitUnary(const ir::Instruction &inst) {
    loadSlot("a0", inst.lhs);
    killReg("a0");  // unary op overwrites a0
    switch (inst.unary) {
      case ir::UnaryOp::Plus:
        break;
      case ir::UnaryOp::Minus:
        out_ << "  neg a0, a0\n";
        break;
      case ir::UnaryOp::Not:
        out_ << "  seqz a0, a0\n";
        break;
    }
    storeSlot("a0", inst.dest);
  }

  // 计算 log2(v)，前提 v 是 2 的幂且 v >= 1
  static int log2pow2(int v) {
    int r = 0;
    while ((1 << r) < v) ++r;
    return r;
  }

  // 用 magic multiplier 实现 `x / d` 或 `x % d`（d 不为 0/±1/±2^k）。
  // 算法来自 Hacker's Delight Fig. 10-1：
  //   q = high32_signed(x * M)
  //   if (d > 0 && M < 0) q = q + x;     // addMarker
  //   if (d < 0 && M > 0) q = q - x;     // 用同一个 addMarker 标记并取反 src
  //   q = q >>arith s
  //   q = q + (x >>logical 31)            // 加上符号位（负数时 +1）
  //                                       //   d<0 时改为 (-x) >>logical 31
  // 对 d<0 的修正：先令 ad = |d|，按 ad 计算，再 neg 结果。我们采用更直接
  // 的形式：让 magic 自带符号（M 的符号已在 computeDivMagic 中处理）。
  void emitDivByConst(const ir::Instruction &inst, int d, bool isMod) {
    const auto m = computeDivMagic(d);
    // 把 src 放到 t0（不能是 a0，整个序列中要重复读 src）。
    const std::string lhsReg = slotInReg(inst.lhs);
    const std::string src = (lhsReg.empty() || lhsReg == "a0")
        ? (loadSlot("t0", inst.lhs), std::string("t0"))
        : lhsReg;
    killReg("a0");
    // li t1, M
    out_ << "  li t1, " << m.M << "\n";
    // mulh a0, src, t1 — signed × signed -> high 32
    out_ << "  mulh a0, " << src << ", t1\n";
    if (m.addMarker) {
      if (d > 0)
        out_ << "  add a0, a0, " << src << "\n";
      else
        out_ << "  sub a0, a0, " << src << "\n";
    }
    if (m.s > 0) out_ << "  srai a0, a0, " << m.s << "\n";
    // q = q + (sign-bit of dividend). For negative d, the dividend's sign
    // contribution flips: standard form is q = q + (d<0 ? -x : x) >>u 31.
    if (d > 0) {
      out_ << "  srli t1, " << src << ", 31\n"
           << "  add a0, a0, t1\n";
    } else {
      // For negative d we want q = q + (-x >>u 31), i.e. add 1 when x > 0.
      // After computing M with negated sign, the standard fix is the same
      // formula but reading the sign of the (signed) high product 'a0'.
      // Hacker's Delight gives: q += (q >>u 31). Use that.
      out_ << "  srli t1, a0, 31\n"
           << "  add a0, a0, t1\n";
    }
    if (!isMod) {
      storeSlot("a0", inst.dest);
      return;
    }
    // x % d = x - (x/d)*d
    out_ << "  li t1, " << d << "\n"
         << "  mul a0, a0, t1\n"
         << "  sub a0, " << src << ", a0\n";
    storeSlot("a0", inst.dest);
  }

  void emitBinary(const ir::Instruction &inst) {
    int kr = 0, kl = 0;
    const bool rConst = isConstSlot(inst.rhs, kr);
    const bool lConst = isConstSlot(inst.lhs, kl);

    // -------- 强度削减 / addi 折叠：rhs 是已知常量 ----------
    if (rConst) {
      const std::string lhsReg = slotInReg(inst.lhs);
      auto loadLhs = [&](const char *target) {
        if (!lhsReg.empty()) {
          if (lhsReg == target) return std::string(target);
          out_ << "  mv " << target << ", " << lhsReg << "\n";
          return std::string(target);
        }
        loadSlot(target, inst.lhs);
        return std::string(target);
      };
      switch (inst.binary) {
        case ir::BinaryOp::Add:
          if (kr >= -2048 && kr <= 2047) {
            const std::string src = lhsReg.empty() ? (loadSlot("t0", inst.lhs), std::string("t0")) : lhsReg;
            killReg("a0");
            out_ << "  addi a0, " << src << ", " << kr << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        case ir::BinaryOp::Sub:
          if (-kr >= -2048 && -kr <= 2047) {
            const std::string src = lhsReg.empty() ? (loadSlot("t0", inst.lhs), std::string("t0")) : lhsReg;
            killReg("a0");
            out_ << "  addi a0, " << src << ", " << (-kr) << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        case ir::BinaryOp::Mul:
          if (kr == 0) {
            killReg("a0");
            out_ << "  li a0, 0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr == 1) {
            loadSlot("a0", inst.lhs);
            killReg("a0");
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr == -1) {
            const std::string src = lhsReg.empty() ? (loadSlot("t0", inst.lhs), std::string("t0")) : lhsReg;
            killReg("a0");
            out_ << "  neg a0, " << src << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr > 0 && (kr & (kr - 1)) == 0) {
            const std::string src = lhsReg.empty() ? (loadSlot("t0", inst.lhs), std::string("t0")) : lhsReg;
            killReg("a0");
            out_ << "  slli a0, " << src << ", " << log2pow2(kr) << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        case ir::BinaryOp::Div:
          if (kr == 1) {
            loadSlot("a0", inst.lhs);
            killReg("a0");
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr == -1) {
            const std::string src = lhsReg.empty() ? (loadSlot("t0", inst.lhs), std::string("t0")) : lhsReg;
            killReg("a0");
            out_ << "  neg a0, " << src << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr > 1 && (kr & (kr - 1)) == 0) {
            // 把 src 放到 t0（必须留住源），结果计算在 a0。
            const std::string src = lhsReg.empty() || lhsReg == "a0"
                ? (loadSlot("t0", inst.lhs), std::string("t0"))
                : lhsReg;
            const int k = log2pow2(kr);
            killReg("a0");
            out_ << "  srai a0, " << src << ", 31\n"
                 << "  srli a0, a0, " << (32 - k) << "\n"
                 << "  add a0, " << src << ", a0\n"
                 << "  srai a0, a0, " << k << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          // 非 2 幂常数除法 → magic multiplier。
          if (kr != 0 && kr != 1 && kr != -1) {
            emitDivByConst(inst, kr, /*isMod=*/false);
            return;
          }
          break;
        case ir::BinaryOp::Mod:
          if (kr == 1 || kr == -1) {
            killReg("a0");
            out_ << "  li a0, 0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr > 1 && (kr & (kr - 1)) == 0) {
            const std::string src = lhsReg.empty() || lhsReg == "a0"
                ? (loadSlot("t0", inst.lhs), std::string("t0"))
                : lhsReg;
            const int k = log2pow2(kr);
            killReg("a0");
            out_ << "  srai a0, " << src << ", 31\n"
                 << "  srli a0, a0, " << (32 - k) << "\n"
                 << "  add a0, " << src << ", a0\n"
                 << "  srai a0, a0, " << k << "\n"
                 << "  slli a0, a0, " << k << "\n"
                 << "  sub a0, " << src << ", a0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          // 非 2 幂常数取模 → x - (x/c)*c。
          if (kr != 0 && kr != 1 && kr != -1) {
            emitDivByConst(inst, kr, /*isMod=*/true);
            return;
          }
          break;
        default:
          break;
      }
      (void)loadLhs;
    }
    // -------- 强度削减：lhs 是已知常量（仅交换可行的 op）---------
    if (lConst) {
      switch (inst.binary) {
        case ir::BinaryOp::Add:
          if (kl >= -2048 && kl <= 2047) {
            loadSlot("t0", inst.rhs);
            killReg("a0");
            out_ << "  addi a0, t0, " << kl << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        case ir::BinaryOp::Mul:
          if (kl == 0) {
            killReg("a0");
            out_ << "  li a0, 0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kl == 1) {
            loadSlot("a0", inst.rhs);
            killReg("a0");
            storeSlot("a0", inst.dest);
            return;
          }
          if (kl > 0 && (kl & (kl - 1)) == 0) {
            loadSlot("t0", inst.rhs);
            killReg("a0");
            out_ << "  slli a0, t0, " << log2pow2(kl) << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        default:
          break;
      }
    }

    // -------- 通用回退 ----------
    // 决定操作数最终所在的寄存器。优先复用 slot 当前的寄存器位置；若必须
    // 加载，安排到不会冲突的临时寄存器。
    const std::string lhsHere = slotInReg(inst.lhs);
    const std::string rhsHere = slotInReg(inst.rhs);
    std::string t0Src;  // lhs operand reg
    std::string a0Src;  // rhs operand reg

    if (!lhsHere.empty() && !rhsHere.empty()) {
      // 两个操作数都已在寄存器中。t0Src 不能等于 a0（killReg("a0") 会丢值）
      // 之外，直接用之即可。注意 lhsHere == rhsHere（同一 s-reg 来源）也安全
      // ——op 允许两个 source 相同。
      if (lhsHere == "a0") {
        // 即将写 a0；为避免覆盖输入，搬到 t0。
        out_ << "  mv t0, a0\n";
        t0Src = "t0";
      } else {
        t0Src = lhsHere;
      }
      a0Src = rhsHere;
    } else if (!lhsHere.empty()) {
      // lhs 在 reg；rhs 需 load 到 a0（结果也写 a0 不冲突，因 rhs 是输入端
      // 先消费才被覆盖——但 RISC-V 三操作数允许）。
      if (lhsHere == "a0") {
        out_ << "  mv t0, a0\n";
        t0Src = "t0";
      } else {
        t0Src = lhsHere;
      }
      loadSlot("a0", inst.rhs);
      a0Src = "a0";
    } else if (!rhsHere.empty()) {
      // rhs 在 reg；lhs 需 load。把 lhs 放 t0。要小心 load 不要破坏 rhsHere：
      // loadSlot("t0", ...) 只 dropReg("t0")。但若 rhsHere == "t0"，load 会
      // 覆盖它——这种情况先把 rhs 搬到 a0 再 load lhs。
      if (rhsHere == "t0") {
        out_ << "  mv a0, t0\n";
        a0Src = "a0";
      } else {
        a0Src = rhsHere;
      }
      loadSlot("t0", inst.lhs);
      t0Src = "t0";
    } else {
      loadSlot("t0", inst.lhs);
      loadSlot("a0", inst.rhs);
      t0Src = "t0"; a0Src = "a0";
    }
    killReg("a0");  // binary op overwrites a0
    switch (inst.binary) {
      case ir::BinaryOp::Less:
        out_ << "  slt a0, " << t0Src << ", " << a0Src << "\n";
        break;
      case ir::BinaryOp::Greater:
        out_ << "  slt a0, " << a0Src << ", " << t0Src << "\n";
        break;
      case ir::BinaryOp::LessEqual:
        out_ << "  slt a0, " << a0Src << ", " << t0Src << "\n  xori a0, a0, 1\n";
        break;
      case ir::BinaryOp::GreaterEqual:
        out_ << "  slt a0, " << t0Src << ", " << a0Src << "\n  xori a0, a0, 1\n";
        break;
      case ir::BinaryOp::Equal:
        out_ << "  sub a0, " << t0Src << ", " << a0Src << "\n  seqz a0, a0\n";
        break;
      case ir::BinaryOp::NotEqual:
        out_ << "  sub a0, " << t0Src << ", " << a0Src << "\n  snez a0, a0\n";
        break;
      case ir::BinaryOp::Add:
        out_ << "  add a0, " << t0Src << ", " << a0Src << "\n";
        break;
      case ir::BinaryOp::Sub:
        out_ << "  sub a0, " << t0Src << ", " << a0Src << "\n";
        break;
      case ir::BinaryOp::Mul:
        out_ << "  mul a0, " << t0Src << ", " << a0Src << "\n";
        break;
      case ir::BinaryOp::Div:
        out_ << "  div a0, " << t0Src << ", " << a0Src << "\n";
        break;
      case ir::BinaryOp::Mod:
        out_ << "  rem a0, " << t0Src << ", " << a0Src << "\n";
        break;
    }
    storeSlot("a0", inst.dest);
  }

  void pushA0() { out_ << "  addi sp, sp, -4\n  sw a0, 0(sp)\n"; }

  void emitCall(const ir::Instruction &inst) {
    const std::size_t count = inst.args.size();
    const std::size_t registerCount = std::min<std::size_t>(count, 8);
    const std::size_t overflowCount = count > 8 ? count - 8 : 0;

    // Overflow args (index 8..) go on the caller stack at [0 .. overflowCount*4).
    // Push them first (in reverse so the lowest-index overflow arg ends up at
    // the lowest address, matching the standard ABI where arg8 is at 0(sp)).
    if (overflowCount > 0) {
      emitAddSp(-static_cast<int>(overflowCount) * 4);
      for (std::size_t i = 0; i < overflowCount; ++i) {
        const std::size_t argIndex = 8 + i;
        loadSlot("a0", inst.args[argIndex]);
        emitSwReg("a0", static_cast<int>(i) * 4, "sp");
      }
    }

    // Register args a0..a7. Load in reverse (a7 first, a0 last) so that if any
    // loadSlot path temporarily reuses an a-reg as a scratch, the final a0
    // write is not clobbered by a later load. loadSlot("aK", slot) only writes
    // aK (it never touches other a-regs directly), so forward order would also
    // work; reverse is a defensive belt-and-suspenders.
    for (std::size_t k = registerCount; k-- > 0;) {
      loadSlot("a" + std::to_string(k), inst.args[k]);
    }

    out_ << "  jal " << inst.name << "\n";
    if (overflowCount > 0) emitAddSp(static_cast<int>(overflowCount) * 4);
    invalidateCache();
    storeSlot("a0", inst.dest);
  }

  const ir::Function &func_;
  std::ostream &out_;
  int frameSize_ = 16;
};

void emitData(const ir::Program &program, std::ostream &out) {
  if (program.globals.empty()) return;
  out << "  .data\n";
  for (const auto &global : program.globals) {
    out << "  .globl " << global.label << "\n"
        << global.label << ":\n"
        << "  .word " << global.value << "\n";
  }
}

}  // namespace

void emitRiscv(const ir::Program &program, std::ostream &out) {
  emitData(program, out);
  for (const auto &func : program.functions) FunctionEmitter(func, out).emit();
}

}  // namespace toycc::backend