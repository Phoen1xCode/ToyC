#include "backend/riscv.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "support/diagnostic.hpp"

namespace toycc::backend {
namespace {

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
      // 参数一律落栈，不录入 s-reg。避免调用方在 emitCall 中复用 a-i 与被调
      // 用方映射到同一 s-reg 的冲突路径。
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
  static constexpr int kCacheCap = 8;

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
  void invalidateCache() { cache_.clear(); }

  // 写入工作寄存器时通知 cache：该 reg 上所有旧 slot 映射失效。
  void killReg(const std::string &reg) { dropReg(reg); }

  // 选出使用频率最高的若干用户命名社位（VarDecl，不含参数）映射到 s2..s11。
  // 这些通常是循环中贯穿的归纳变量/累加器，入 s-reg 后避免每次 lw/sw。
  // 参数一律落栈不参与映射，避免调用约定复杂交互。
  std::unordered_map<int, std::string> regMap_;
  std::vector<std::string> savedSRegs_;
  static constexpr int kMaxRegAlloc = 10;  // s2..s11
  void allocateRegisters() {
    if (func_.namedSlots.empty()) return;
    std::unordered_map<int, int> uses;
    for (const auto &inst : func_.instructions)
      for (int s : readsOf(inst)) ++uses[s];
    // 只考虑 VarDecl 产生的 namedSlot；参数已设为落栈，这里排除 paramSlots。
    std::unordered_set<int> paramSet(func_.paramSlots.begin(), func_.paramSlots.end());
    std::vector<std::pair<int,int>> ranked;
    for (int s : func_.namedSlots) {
      if (paramSet.count(s)) continue;
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

  // 把"只被定义为 Op::Const 一次且永不被其他指令重写"的 slot 编入常量表。
  // 后端在 emitBinary 中据此把 mul/div/rem 常量优化为 shift/and/addi 等等，
  // 也可以把常量短直接 li 到工作寄存器，省略 sw/lw 的中间往返。
  std::unordered_map<int, int> slotConst_;
  void computeConstSlots() {
    std::unordered_map<int, int> defs;  // slot -> def count
    std::unordered_map<int, int> values;
    using Op = ir::Instruction::Op;
    for (const auto &i : func_.instructions) {
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
  }
  bool isConstSlot(int slot, int &outVal) const {
    auto it = slotConst_.find(slot);
    if (it == slotConst_.end()) return false;
    outVal = it->second;
    return true;
  }

  void loadSlot(const char *reg, int slot) {
    // 被寄存器分配的 slot 其家在 s-reg；不走 cache，也不可被缓存命中
    // （cache 中临时 reg 早已被覆盖）。
    auto m = regMap_.find(slot);
    if (m != regMap_.end()) {
      out_ << "  mv " << reg << ", " << m->second << "\n";
      return;
    }
    // 常量 slot：直接 li，不去内存找。这要求 Const 指令本身可以省略 sw。
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

  void emitBinary(const ir::Instruction &inst) {
    int kr = 0, kl = 0;
    const bool rConst = isConstSlot(inst.rhs, kr);
    const bool lConst = isConstSlot(inst.lhs, kl);

    // -------- 强度削减 / addi 折叠：rhs 是已知常量 ----------
    if (rConst) {
      switch (inst.binary) {
        case ir::BinaryOp::Add:
          if (kr >= -2048 && kr <= 2047) {
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  addi a0, t0, " << kr << "\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        case ir::BinaryOp::Sub:
          if (-kr >= -2048 && -kr <= 2047) {
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  addi a0, t0, " << (-kr) << "\n";
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
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  neg a0, t0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr > 0 && (kr & (kr - 1)) == 0) {
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  slli a0, t0, " << log2pow2(kr) << "\n";
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
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  neg a0, t0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          if (kr > 1 && (kr & (kr - 1)) == 0) {
            // Signed division by 2^k with round-towards-zero (C semantics):
            //   t1 = x >> 31           (all 1s if x<0, else 0)
            //   t1 = t1 >>u (32-k)      (k 1s in low bits if x<0 — bias)
            //   t0 = (x + t1) >>s k
            const int k = log2pow2(kr);
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  srai a0, t0, 31\n"
                 << "  srli a0, a0, " << (32 - k) << "\n"
                 << "  add a0, t0, a0\n"
                 << "  srai a0, a0, " << k << "\n";
            storeSlot("a0", inst.dest);
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
            // Signed rem by 2^k. Compute (x mod 2^k) preserving sign of x:
            //   t1 = x >> 31; t1 = t1 >>u (32-k);    bias = (x<0 ? 2^k-1 : 0)
            //   q = (x + bias) >>s k
            //   r = x - q*2^k = x - (q << k)
            const int k = log2pow2(kr);
            loadSlot("t0", inst.lhs);
            killReg("a0");
            out_ << "  srai a0, t0, 31\n"
                 << "  srli a0, a0, " << (32 - k) << "\n"
                 << "  add a0, t0, a0\n"
                 << "  srai a0, a0, " << k << "\n"
                 << "  slli a0, a0, " << k << "\n"
                 << "  sub a0, t0, a0\n";
            storeSlot("a0", inst.dest);
            return;
          }
          break;
        default:
          break;
      }
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
    loadSlot("t0", inst.lhs);
    loadSlot("a0", inst.rhs);
    killReg("a0");  // binary op overwrites a0
    switch (inst.binary) {
      case ir::BinaryOp::Less:
        out_ << "  slt a0, t0, a0\n";
        break;
      case ir::BinaryOp::Greater:
        out_ << "  slt a0, a0, t0\n";
        break;
      case ir::BinaryOp::LessEqual:
        out_ << "  slt a0, a0, t0\n  xori a0, a0, 1\n";
        break;
      case ir::BinaryOp::GreaterEqual:
        out_ << "  slt a0, t0, a0\n  xori a0, a0, 1\n";
        break;
      case ir::BinaryOp::Equal:
        out_ << "  sub a0, t0, a0\n  seqz a0, a0\n";
        break;
      case ir::BinaryOp::NotEqual:
        out_ << "  sub a0, t0, a0\n  snez a0, a0\n";
        break;
      case ir::BinaryOp::Add:
        out_ << "  add a0, t0, a0\n";
        break;
      case ir::BinaryOp::Sub:
        out_ << "  sub a0, t0, a0\n";
        break;
      case ir::BinaryOp::Mul:
        out_ << "  mul a0, t0, a0\n";
        break;
      case ir::BinaryOp::Div:
        out_ << "  div a0, t0, a0\n";
        break;
      case ir::BinaryOp::Mod:
        out_ << "  rem a0, t0, a0\n";
        break;
    }
    storeSlot("a0", inst.dest);
  }

  void pushA0() { out_ << "  addi sp, sp, -4\n  sw a0, 0(sp)\n"; }

  void emitCall(const ir::Instruction &inst) {
    const std::size_t count = inst.args.size();
    for (int arg : inst.args) {
      loadSlot("a0", arg);
      pushA0();
    }

    const std::size_t registerCount = std::min<std::size_t>(count, 8);
    const std::size_t overflowCount = count > 8 ? count - 8 : 0;
    if (overflowCount > 0) {
      const int bytes = static_cast<int>(overflowCount) * 4;
      emitAddSp(-bytes);
    }

    // 寄存器参数从接近栈顶的位置取回。偏移可能很大，使用大偏移 helper。
    for (std::size_t i = 0; i < registerCount; ++i) {
      const int offset = static_cast<int>(overflowCount) * 4 + 4 * (static_cast<int>(count) - 1 - static_cast<int>(i));
      emitLwReg("a" + std::to_string(i), offset, "sp");
    }
    for (std::size_t i = 0; i < overflowCount; ++i) {
      const std::size_t argIndex = 8 + i;
      const int from = static_cast<int>(overflowCount) * 4 + 4 * (static_cast<int>(count) - 1 - static_cast<int>(argIndex));
      emitLwReg("t0", from, "sp");
      emitSwReg("t0", static_cast<int>(i) * 4, "sp");
    }

    out_ << "  jal " << inst.name << "\n";
    const int bytesToPop = static_cast<int>((count + overflowCount) * 4);
    if (bytesToPop > 0) emitAddSp(bytesToPop);
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