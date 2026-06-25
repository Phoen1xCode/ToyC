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

int slotOffset(int slot) { return -12 - slot * 4; }

int frameSize(const ir::Function &func) {
  const int localBytes = 8 + func.slotCount * 4;
  return std::max(16, ((localBytes + 15) / 16) * 16);
}

class FunctionEmitter {
 public:
  FunctionEmitter(const ir::Function &func, std::ostream &out) : func_(func), out_(out) {}

  void emit() {
    frameSize_ = frameSize(func_);
    collectBranchTargets();
    out_ << "  .text\n"
         << "  .globl " << func_.name << "\n"
         << func_.name << ":\n";
    emitAddSp(-frameSize_);
    emitSwReg("ra", frameSize_ - 4, "sp");
    emitSwReg("s0", frameSize_ - 8, "sp");
    emitAddiReg("s0", "sp", frameSize_);

    for (std::size_t i = 0; i < func_.paramSlots.size(); ++i) {
      const int offset = slotOffset(func_.paramSlots[i]);
      if (i < 8) {
        emitSwReg("a" + std::to_string(i), offset, "s0");
      } else {
        // 参数 8+ 由调用方放在调用者栈上（位于 sp 之前）；
        // 我们约定调用方按溢出顺序写入以 s0 为基址的正偏移，
        // 即 (i-8)*4。为 i*4 偏移过大时同样需要大偏移 helper。
        emitLwReg("t0", (static_cast<int>(i) - 8) * 4, "s0");
        emitSwReg("t0", offset, "s0");
      }
    }

    for (const auto &inst : func_.instructions) emitInst(inst);
    out_ << returnLabel() << ":\n";
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
    dropSlot(slot); dropReg(reg);
    cache_.push_back({slot, reg});
    while (static_cast<int>(cache_.size()) > kCacheCap) cache_.erase(cache_.begin());
  }
  void invalidateCache() { cache_.clear(); }

  void loadSlot(const char *reg, int slot) {
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
    emitSwReg(reg, slotOffset(slot), "s0");
    std::string regs(reg);
    // sw 不修改源 reg；reg 上原本缓存的别的 slot 仍与内存一致（但内存
    // 中该 slot 仍是旧值，不需要修改）。但是我们登记新 cache (slot, reg)，
    // 而 reg 此前若已在别条 cache 中（reg 仍是別的 slot 的有效缓存）则
    // 该 slot 的实际内存值仍是旧值，与 reg 内容不同；敝 斧 避 产生 例 错。
    // 为安全起见，先以 dropReg 清除任何 “reg 是另外一套 slot 的缓存” 的
    // 陈腐语义。
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
        out_ << "  li a0, " << inst.value << "\n";
        storeSlot("a0", inst.dest);
        return;
      case ir::Instruction::Op::Move:
        loadSlot("a0", inst.lhs);
        storeSlot("a0", inst.dest);
        return;
      case ir::Instruction::Op::LoadGlobal:
        out_ << "  la t0, " << inst.name << "\n"
             << "  lw a0, 0(t0)\n";
        storeSlot("a0", inst.dest);
        return;
      case ir::Instruction::Op::StoreGlobal:
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
        out_ << "  li a0, 0\n"
             << "  j " << returnLabel() << "\n";
        return;
    }
  }

  void emitUnary(const ir::Instruction &inst) {
    loadSlot("a0", inst.lhs);
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

  void emitBinary(const ir::Instruction &inst) {
    loadSlot("t0", inst.lhs);
    loadSlot("a0", inst.rhs);
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