#include "ir/ir_builder.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "support/diagnostic.hpp"

namespace toycc::ir {
namespace {

using ast::AssignStmt;
using ast::BinaryExpr;
using ast::BlockStmt;
using ast::BreakStmt;
using ast::CallExpr;
using ast::CompUnit;
using ast::ConstDecl;
using ast::ContinueStmt;
using ast::Decl;
using ast::DeclStmt;
using ast::EmptyStmt;
using ast::Expr;
using ast::ExprStmt;
using ast::FuncDef;
using ast::IfStmt;
using ast::NameExpr;
using ast::NumberExpr;
using ast::ReturnStmt;
using ast::Stmt;
using ast::UnaryExpr;
using ast::VarDecl;
using ast::WhileStmt;

struct Binding {
  enum class Kind { Slot, Global, Constant };

  Kind kind = Kind::Slot;
  int slot = -1;
  int value = 0;
  std::string label;
};

std::string globalLabel(const std::string &name) { return ".G_" + name; }

int truthy(int value) { return value != 0 ? 1 : 0; }

UnaryOp mapUnary(UnaryExpr::Op op) {
  switch (op) {
    case UnaryExpr::Op::Plus:
      return UnaryOp::Plus;
    case UnaryExpr::Op::Minus:
      return UnaryOp::Minus;
    case UnaryExpr::Op::Not:
      return UnaryOp::Not;
  }
}

BinaryOp mapBinary(BinaryExpr::Op op) {
  switch (op) {
    case BinaryExpr::Op::Less:
      return BinaryOp::Less;
    case BinaryExpr::Op::Greater:
      return BinaryOp::Greater;
    case BinaryExpr::Op::LessEqual:
      return BinaryOp::LessEqual;
    case BinaryExpr::Op::GreaterEqual:
      return BinaryOp::GreaterEqual;
    case BinaryExpr::Op::Equal:
      return BinaryOp::Equal;
    case BinaryExpr::Op::NotEqual:
      return BinaryOp::NotEqual;
    case BinaryExpr::Op::Add:
      return BinaryOp::Add;
    case BinaryExpr::Op::Sub:
      return BinaryOp::Sub;
    case BinaryExpr::Op::Mul:
      return BinaryOp::Mul;
    case BinaryExpr::Op::Div:
      return BinaryOp::Div;
    case BinaryExpr::Op::Mod:
      return BinaryOp::Mod;
    case BinaryExpr::Op::LogicalAnd:
    case BinaryExpr::Op::LogicalOr:
      break;
  }
  fatal("logical operator must be lowered before binary IR");
}

class Builder {
 public:
  explicit Builder(const CompUnit &program) : ast_(program) {}

  Program build() {
    collectGlobals();
    for (const auto &decl : ast_.decls) {
      if (const auto *func = dynamic_cast<const FuncDef *>(decl.get())) program_.functions.push_back(buildFunction(*func));
    }
    return std::move(program_);
  }

 private:
  void collectGlobals() {
    pushScope();
    for (const auto &decl : ast_.decls) {
      if (const auto *constant = dynamic_cast<const ConstDecl *>(decl.get())) {
        const int value = evalConst(*constant->initializer);
        scopes_.back()[constant->name] = Binding{Binding::Kind::Constant, -1, value, {}};
      } else if (const auto *var = dynamic_cast<const VarDecl *>(decl.get())) {
        const int value = evalConst(*var->initializer);
        const std::string label = globalLabel(var->name);
        scopes_.back()[var->name] = Binding{Binding::Kind::Global, -1, 0, label};
        program_.globals.push_back(Global{var->name, label, value});
      }
    }
  }

  Function buildFunction(const FuncDef &func) {
    current_ = Function{};
    current_.name = func.name;
    nextLabel_ = 0;
    loopStack_.clear();
    pushScope();
    for (const auto &param : func.params) {
      const int slot = newSlot();
      current_.paramSlots.push_back(slot);
      scopes_.back()[param.name] = Binding{Binding::Kind::Slot, slot, 0, {}};
    }
    emitStmt(*func.body);
    if (func.returnType == FuncDef::ReturnType::Void) emit({Instruction::Op::ReturnVoid});
    popScope();
    Function result = std::move(current_);
    current_ = Function{};
    return result;
  }

  int evalConst(const Expr &expr) const {
    if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) return number->value;
    if (const auto *name = dynamic_cast<const NameExpr *>(&expr)) {
      const Binding *binding = lookup(name->name);
      if (binding->kind != Binding::Kind::Constant) fatal("non-constant in constant expression");
      return binding->value;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      const int v = evalConst(*unary->operand);
      switch (unary->op) {
        case UnaryExpr::Op::Plus:
          return v;
        case UnaryExpr::Op::Minus:
          return -v;
        case UnaryExpr::Op::Not:
          return truthy(!v);
      }
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
      if (binary->op == BinaryExpr::Op::LogicalAnd) {
        const int left = evalConst(*binary->left);
        return left ? truthy(evalConst(*binary->right)) : 0;
      }
      if (binary->op == BinaryExpr::Op::LogicalOr) {
        const int left = evalConst(*binary->left);
        return left ? 1 : truthy(evalConst(*binary->right));
      }
      const int left = evalConst(*binary->left);
      const int right = evalConst(*binary->right);
      switch (binary->op) {
        case BinaryExpr::Op::Less:
          return left < right;
        case BinaryExpr::Op::Greater:
          return left > right;
        case BinaryExpr::Op::LessEqual:
          return left <= right;
        case BinaryExpr::Op::GreaterEqual:
          return left >= right;
        case BinaryExpr::Op::Equal:
          return left == right;
        case BinaryExpr::Op::NotEqual:
          return left != right;
        case BinaryExpr::Op::Add:
          return left + right;
        case BinaryExpr::Op::Sub:
          return left - right;
        case BinaryExpr::Op::Mul:
          return left * right;
        case BinaryExpr::Op::Div:
          return left / right;
        case BinaryExpr::Op::Mod:
          return left % right;
        case BinaryExpr::Op::LogicalAnd:
        case BinaryExpr::Op::LogicalOr:
          break;
      }
    }
    fatal("unsupported constant expression");
  }


  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }

  const Binding *lookup(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) return &found->second;
    }
    fatal("unknown IR binding: " + name);
  }

  int newSlot() { return current_.slotCount++; }

  std::string newLabel(const std::string &prefix) {
    return ".L_" + current_.name + "_" + prefix + "_" + std::to_string(nextLabel_++);
  }

  int newTemp() { return newSlot(); }

  void emit(Instruction inst) { current_.instructions.push_back(std::move(inst)); }

  void emitLabel(const std::string &label) {
    Instruction inst;
    inst.op = Instruction::Op::Label;
    inst.label = label;
    emit(std::move(inst));
  }

  void emitGoto(const std::string &label) {
    Instruction inst;
    inst.op = Instruction::Op::Goto;
    inst.label = label;
    emit(std::move(inst));
  }

  void emitStmt(const Stmt &stmt) {
    if (dynamic_cast<const EmptyStmt *>(&stmt)) return;
    if (const auto *block = dynamic_cast<const BlockStmt *>(&stmt)) {
      pushScope();
      for (const auto &child : block->statements) emitStmt(*child);
      popScope();
      return;
    }
    if (const auto *exprStmt = dynamic_cast<const ExprStmt *>(&stmt)) {
      emitExpr(*exprStmt->expr);
      return;
    }
    if (const auto *assign = dynamic_cast<const AssignStmt *>(&stmt)) {
      const int value = emitExpr(*assign->value);
      storeName(assign->name, value);
      return;
    }
    if (const auto *declStmt = dynamic_cast<const DeclStmt *>(&stmt)) {
      emitDecl(*declStmt->decl);
      return;
    }
    if (const auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
      const std::string elseLabel = newLabel("else");
      const std::string endLabel = newLabel("ifend");
      const int cond = emitExpr(*ifStmt->condition);
      emitBranch(cond, ifStmt->elseBranch ? elseLabel : endLabel);
      emitStmt(*ifStmt->thenBranch);
      emitGoto(endLabel);
      if (ifStmt->elseBranch) {
        emitLabel(elseLabel);
        emitStmt(*ifStmt->elseBranch);
      }
      emitLabel(endLabel);
      return;
    }
    if (const auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
      const std::string condLabel = newLabel("while_cond");
      const std::string bodyLabel = newLabel("while_body");
      const std::string endLabel = newLabel("while_end");
      loopStack_.push_back({endLabel, condLabel});
      emitLabel(condLabel);
      const int cond = emitExpr(*whileStmt->condition);
      emitBranch(cond, endLabel);
      emitLabel(bodyLabel);
      emitStmt(*whileStmt->body);
      emitGoto(condLabel);
      emitLabel(endLabel);
      loopStack_.pop_back();
      return;
    }
    if (dynamic_cast<const BreakStmt *>(&stmt)) {
      emitGoto(loopStack_.back().breakLabel);
      return;
    }
    if (dynamic_cast<const ContinueStmt *>(&stmt)) {
      emitGoto(loopStack_.back().continueLabel);
      return;
    }
    if (const auto *ret = dynamic_cast<const ReturnStmt *>(&stmt)) {
      if (ret->value) {
        Instruction inst;
        inst.op = Instruction::Op::Return;
        inst.lhs = emitExpr(*ret->value);
        emit(std::move(inst));
      } else {
        emit({Instruction::Op::ReturnVoid});
      }
      return;
    }
    fatal("unsupported statement in IR builder");
  }

  void emitDecl(const Decl &decl) {
    if (const auto *var = dynamic_cast<const VarDecl *>(&decl)) {
      const int slot = newSlot();
      scopes_.back()[var->name] = Binding{Binding::Kind::Slot, slot, 0, {}};
      const int value = emitExpr(*var->initializer);
      emitMove(slot, value);
      return;
    }
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&decl)) {
      scopes_.back()[constant->name] = Binding{Binding::Kind::Constant, -1, evalConst(*constant->initializer), {}};
      return;
    }
    fatal("unexpected declaration in function body");
  }

  int emitExpr(const Expr &expr) {
    if (const auto *number = dynamic_cast<const NumberExpr *>(&expr)) {
      const int dest = newTemp();
      Instruction inst;
      inst.op = Instruction::Op::Const;
      inst.dest = dest;
      inst.value = number->value;
      emit(std::move(inst));
      return dest;
    }
    if (const auto *name = dynamic_cast<const NameExpr *>(&expr)) {
      const Binding *binding = lookup(name->name);
      if (binding->kind == Binding::Kind::Slot) return binding->slot;
      const int dest = newTemp();
      if (binding->kind == Binding::Kind::Constant) {
        Instruction inst;
        inst.op = Instruction::Op::Const;
        inst.dest = dest;
        inst.value = binding->value;
        emit(std::move(inst));
      } else {
        Instruction inst;
        inst.op = Instruction::Op::LoadGlobal;
        inst.dest = dest;
        inst.name = binding->label;
        emit(std::move(inst));
      }
      return dest;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      const int operand = emitExpr(*unary->operand);
      const int dest = newTemp();
      Instruction inst;
      inst.op = Instruction::Op::Unary;
      inst.dest = dest;
      inst.lhs = operand;
      inst.unary = mapUnary(unary->op);
      emit(std::move(inst));
      return dest;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) return emitBinary(*binary);
    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      Instruction inst;
      inst.op = Instruction::Op::Call;
      inst.dest = newTemp();
      inst.name = call->callee;
      for (const auto &arg : call->args) inst.args.push_back(emitExpr(*arg));
      const int dest = inst.dest;
      emit(std::move(inst));
      return dest;
    }
    fatal("unsupported expression in IR builder");
  }

  int emitBinary(const BinaryExpr &binary) {
    if (binary.op == BinaryExpr::Op::LogicalAnd || binary.op == BinaryExpr::Op::LogicalOr) {
      const int dest = newTemp();
      const std::string shortLabel = newLabel(binary.op == BinaryExpr::Op::LogicalAnd ? "and_false" : "or_true");
      const std::string endLabel = newLabel("logic_end");
      const int left = emitExpr(*binary.left);
      emitBranch(left, shortLabel, binary.op == BinaryExpr::Op::LogicalOr);
      const int right = emitExpr(*binary.right);
      emitUnary(dest, UnaryOp::Not, right);
      emitUnary(dest, UnaryOp::Not, dest);
      emitGoto(endLabel);
      emitLabel(shortLabel);
      emitConst(dest, binary.op == BinaryExpr::Op::LogicalAnd ? 0 : 1);
      emitLabel(endLabel);
      return dest;
    }

    const int left = emitExpr(*binary.left);
    const int right = emitExpr(*binary.right);
    const int dest = newTemp();
    Instruction inst;
    inst.op = Instruction::Op::Binary;
    inst.dest = dest;
    inst.lhs = left;
    inst.rhs = right;
    inst.binary = mapBinary(binary.op);
    emit(std::move(inst));
    return dest;
  }

  void emitBranch(int cond, const std::string &label, bool onTrue = false) {
    Instruction inst;
    inst.op = Instruction::Op::Branch;
    inst.lhs = cond;
    if (onTrue) {
      inst.label = label;
    } else {
      inst.falseLabel = label;
    }
    emit(std::move(inst));
  }

  void emitConst(int dest, int value) {
    Instruction inst;
    inst.op = Instruction::Op::Const;
    inst.dest = dest;
    inst.value = value;
    emit(std::move(inst));
  }

  void emitMove(int dest, int src) {
    Instruction inst;
    inst.op = Instruction::Op::Move;
    inst.dest = dest;
    inst.lhs = src;
    emit(std::move(inst));
  }

  void emitUnary(int dest, UnaryOp op, int operand) {
    Instruction inst;
    inst.op = Instruction::Op::Unary;
    inst.dest = dest;
    inst.lhs = operand;
    inst.unary = op;
    emit(std::move(inst));
  }

  void storeName(const std::string &name, int value) {
    const Binding *binding = lookup(name);
    if (binding->kind == Binding::Kind::Global) {
      Instruction inst;
      inst.op = Instruction::Op::StoreGlobal;
      inst.lhs = value;
      inst.name = binding->label;
      emit(std::move(inst));
      return;
    }
    emitMove(binding->slot, value);
  }

  struct LoopLabels {
    std::string breakLabel;
    std::string continueLabel;
  };

  const CompUnit &ast_;
  Program program_;
  Function current_;
  std::vector<std::unordered_map<std::string, Binding>> scopes_;
  std::vector<LoopLabels> loopStack_;
  int nextLabel_ = 0;
};

}  // namespace

Program buildIr(const ast::CompUnit &program) { return Builder(program).build(); }

}  // namespace toycc::ir
