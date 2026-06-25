#include "backend/riscv.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <unordered_map>
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
    out_ << "  .text\n"
         << "  .globl " << func_.name << "\n"
         << func_.name << ":\n"
         << "  addi sp, sp, -" << frameSize_ << "\n"
         << "  sw ra, " << frameSize_ - 4 << "(sp)\n"
         << "  sw s0, " << frameSize_ - 8 << "(sp)\n"
         << "  addi s0, sp, " << frameSize_ << "\n";

    for (std::size_t i = 0; i < func_.paramSlots.size(); ++i) {
      const int offset = slotOffset(func_.paramSlots[i]);
      if (i < 8) {
        out_ << "  sw a" << i << ", " << offset << "(s0)\n";
      } else {
        out_ << "  lw t0, " << (static_cast<int>(i) - 8) * 4 << "(s0)\n"
             << "  sw t0, " << offset << "(s0)\n";
      }
    }

    for (const auto &inst : func_.instructions) emitInst(inst);
    out_ << returnLabel() << ":\n"
         << "  lw ra, -4(s0)\n"
         << "  lw s0, -8(s0)\n"
         << "  addi sp, sp, " << frameSize_ << "\n"
         << "  ret\n";
  }

 private:
  std::string returnLabel() const { return ".L_" + func_.name + "_return"; }

  void loadSlot(const char *reg, int slot) { out_ << "  lw " << reg << ", " << slotOffset(slot) << "(s0)\n"; }

  void storeSlot(const char *reg, int slot) { out_ << "  sw " << reg << ", " << slotOffset(slot) << "(s0)\n"; }

  void emitInst(const ir::Instruction &inst) {
    switch (inst.op) {
      case ir::Instruction::Op::Label:
        out_ << inst.label << ":\n";
        return;
      case ir::Instruction::Op::Goto:
        out_ << "  j " << inst.label << "\n";
        return;
      case ir::Instruction::Op::Branch:
        loadSlot("a0", inst.lhs);
        if (!inst.label.empty()) out_ << "  bnez a0, " << inst.label << "\n";
        if (!inst.falseLabel.empty()) out_ << "  beqz a0, " << inst.falseLabel << "\n";
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
        return;
      case ir::Instruction::Op::ReturnVoid:
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
    if (overflowCount > 0) out_ << "  addi sp, sp, -" << overflowCount * 4 << "\n";

    for (std::size_t i = 0; i < registerCount; ++i) {
      const std::size_t offset = overflowCount * 4 + 4 * (count - 1 - i);
      out_ << "  lw a" << i << ", " << offset << "(sp)\n";
    }
    for (std::size_t i = 0; i < overflowCount; ++i) {
      const std::size_t argIndex = 8 + i;
      const std::size_t from = overflowCount * 4 + 4 * (count - 1 - argIndex);
      out_ << "  lw t0, " << from << "(sp)\n"
           << "  sw t0, " << i * 4 << "(sp)\n";
    }

    out_ << "  jal " << inst.name << "\n";
    const std::size_t bytesToPop = (count + overflowCount) * 4;
    if (bytesToPop > 0) out_ << "  addi sp, sp, " << bytesToPop << "\n";
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
