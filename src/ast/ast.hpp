#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace toycc::ast {

struct Node {
  virtual ~Node() = default;
};

struct Decl : Node {};
struct Stmt : Node {};
struct Expr : Node {};

struct CompUnit final : Node {
  std::vector<std::unique_ptr<Decl>> decls;
};

struct ConstDecl final : Decl {
  std::string name;
  std::unique_ptr<Expr> initializer;

  ConstDecl(std::string name, std::unique_ptr<Expr> initializer);
};

struct VarDecl final : Decl {
  std::string name;
  std::unique_ptr<Expr> initializer;

  VarDecl(std::string name, std::unique_ptr<Expr> initializer);
};

struct Param final : Node {
  std::string name;

  explicit Param(std::string name);
};

struct FuncDef final : Decl {
  enum class ReturnType { Int, Void };

  ReturnType returnType;
  std::string name;
  std::vector<Param> params;
  std::unique_ptr<Stmt> body;

  FuncDef(ReturnType returnType, std::string name, std::vector<Param> params,
          std::unique_ptr<Stmt> body);
};

struct BlockStmt final : Stmt {
  std::vector<std::unique_ptr<Stmt>> statements;
};

struct EmptyStmt final : Stmt {};

struct ExprStmt final : Stmt {
  std::unique_ptr<Expr> expr;

  explicit ExprStmt(std::unique_ptr<Expr> expr);
};

struct AssignStmt final : Stmt {
  std::string name;
  std::unique_ptr<Expr> value;

  AssignStmt(std::string name, std::unique_ptr<Expr> value);
};

struct DeclStmt final : Stmt {
  std::unique_ptr<Decl> decl;

  explicit DeclStmt(std::unique_ptr<Decl> decl);
};

struct IfStmt final : Stmt {
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Stmt> thenBranch;
  std::unique_ptr<Stmt> elseBranch;

  IfStmt(std::unique_ptr<Expr> condition, std::unique_ptr<Stmt> thenBranch,
         std::unique_ptr<Stmt> elseBranch);
};

struct WhileStmt final : Stmt {
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Stmt> body;

  WhileStmt(std::unique_ptr<Expr> condition, std::unique_ptr<Stmt> body);
};

struct BreakStmt final : Stmt {};
struct ContinueStmt final : Stmt {};

struct ReturnStmt final : Stmt {
  std::unique_ptr<Expr> value;

  explicit ReturnStmt(std::unique_ptr<Expr> value);
};

struct NumberExpr final : Expr {
  int value;

  explicit NumberExpr(int value);
};

struct NameExpr final : Expr {
  std::string name;

  explicit NameExpr(std::string name);
};

struct CallExpr final : Expr {
  std::string callee;
  std::vector<std::unique_ptr<Expr>> args;

  CallExpr(std::string callee, std::vector<std::unique_ptr<Expr>> args);
};

struct UnaryExpr final : Expr {
  enum class Op { Plus, Minus, Not };

  Op op;
  std::unique_ptr<Expr> operand;

  UnaryExpr(Op op, std::unique_ptr<Expr> operand);
};

struct BinaryExpr final : Expr {
  enum class Op {
    LogicalOr,
    LogicalAnd,
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

  Op op;
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;

  BinaryExpr(Op op, std::unique_ptr<Expr> left, std::unique_ptr<Expr> right);
};

}  // namespace toycc::ast
