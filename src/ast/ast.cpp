#include "ast/ast.hpp"

namespace toycc::ast {

ConstDecl::ConstDecl(std::string name, std::unique_ptr<Expr> initializer)
    : name(std::move(name)), initializer(std::move(initializer)) {}

VarDecl::VarDecl(std::string name, std::unique_ptr<Expr> initializer)
    : name(std::move(name)), initializer(std::move(initializer)) {}

Param::Param(std::string name) : name(std::move(name)) {}

FuncDef::FuncDef(ReturnType returnType, std::string name,
                 std::vector<Param> params, std::unique_ptr<Stmt> body)
    : returnType(returnType),
      name(std::move(name)),
      params(std::move(params)),
      body(std::move(body)) {}

ExprStmt::ExprStmt(std::unique_ptr<Expr> expr) : expr(std::move(expr)) {}

AssignStmt::AssignStmt(std::string name, std::unique_ptr<Expr> value)
    : name(std::move(name)), value(std::move(value)) {}

DeclStmt::DeclStmt(std::unique_ptr<Decl> decl) : decl(std::move(decl)) {}

IfStmt::IfStmt(std::unique_ptr<Expr> condition,
               std::unique_ptr<Stmt> thenBranch,
               std::unique_ptr<Stmt> elseBranch)
    : condition(std::move(condition)),
      thenBranch(std::move(thenBranch)),
      elseBranch(std::move(elseBranch)) {}

WhileStmt::WhileStmt(std::unique_ptr<Expr> condition,
                     std::unique_ptr<Stmt> body)
    : condition(std::move(condition)), body(std::move(body)) {}

ReturnStmt::ReturnStmt(std::unique_ptr<Expr> value)
    : value(std::move(value)) {}

NumberExpr::NumberExpr(int value) : value(value) {}

NameExpr::NameExpr(std::string name) : name(std::move(name)) {}

CallExpr::CallExpr(std::string callee, std::vector<std::unique_ptr<Expr>> args)
    : callee(std::move(callee)), args(std::move(args)) {}

UnaryExpr::UnaryExpr(Op op, std::unique_ptr<Expr> operand)
    : op(op), operand(std::move(operand)) {}

BinaryExpr::BinaryExpr(Op op, std::unique_ptr<Expr> left,
                       std::unique_ptr<Expr> right)
    : op(op), left(std::move(left)), right(std::move(right)) {}

}  // namespace toycc::ast
