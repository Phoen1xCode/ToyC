#pragma once

#include "ast/ast.hpp"

#include <string>
#include <vector>

namespace toycc::ir {

enum class UnaryOp { Plus, Minus, Not };

enum class BinaryOp {
  Less,
  Greater,
  LessEqual,
  GreaterEqual,
  Equal,
  NotEqual,
  Add,
  Sub,
  Mul,
  Div,
  Mod
};

struct Global {
  std::string name;
  std::string label;
  int value = 0;
};

struct Instruction {
  enum class Op {
    Label,
    Goto,
    Branch,
    Const,
    Move,
    LoadGlobal,
    StoreGlobal,
    Unary,
    Binary,
    Call,
    Return,
    ReturnVoid
  };

  Op op = Op::Label;
  int dest = -1;
  int lhs = -1;
  int rhs = -1;
  int value = 0;
  UnaryOp unary = UnaryOp::Plus;
  BinaryOp binary = BinaryOp::Add;
  std::string label;
  std::string falseLabel;
  std::string name;
  std::vector<int> args;
};

struct Function {
  std::string name;
  std::vector<int> paramSlots;
  int slotCount = 0;
  std::vector<Instruction> instructions;
};

struct Program {
  std::vector<Global> globals;
  std::vector<Function> functions;
};

Program buildIr(const ast::CompUnit &program);

}  // namespace toycc::ir
