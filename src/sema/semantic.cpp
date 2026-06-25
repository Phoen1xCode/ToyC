#include "sema/semantic.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "support/diagnostic.hpp"

namespace toycc::sema {
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

struct FunctionInfo {
  FuncDef::ReturnType returnType = FuncDef::ReturnType::Int;
  std::size_t arity = 0;
  std::size_t order = 0;
};

struct ValueInfo {
  bool isConst = false;
};

class Analyzer {
 public:
  explicit Analyzer(const CompUnit &program) : program_(program) {}

  void run() {
    collectGlobalsAndFunctions();
    requireMain();
    for (const auto &decl : program_.decls) {
      if (const auto *func = dynamic_cast<const FuncDef *>(decl.get())) analyzeFunction(*func);
    }
  }

 private:
  void collectGlobalsAndFunctions() {
    pushScope();
    for (const auto &decl : program_.decls) {
      if (const auto *constant = dynamic_cast<const ConstDecl *>(decl.get())) {
        analyzeConstExpr(*constant->initializer);
        declareValue(constant->name, true);
      } else if (const auto *var = dynamic_cast<const VarDecl *>(decl.get())) {
        analyzeConstExpr(*var->initializer);
        declareValue(var->name, false);
      } else if (const auto *func = dynamic_cast<const FuncDef *>(decl.get())) {
        if (lookupValueLocal(func->name) != nullptr) fatal("function conflicts with global value: " + func->name);
        if (!functions_.emplace(func->name, FunctionInfo{func->returnType, func->params.size(), nextFunctionOrder_++}).second) {
          fatal("duplicate function: " + func->name);
        }
      }
    }
  }

  void requireMain() const {
    const auto found = functions_.find("main");
    if (found == functions_.end() || found->second.returnType != FuncDef::ReturnType::Int || found->second.arity != 0) {
      fatal("missing valid int main()");
    }
  }

  void analyzeFunction(const FuncDef &func) {
    currentReturnType_ = func.returnType;
    currentFunctionName_ = func.name;
    currentFunctionOrder_ = functions_.at(func.name).order;
    loopDepth_ = 0;
    pushScope();
    for (const auto &param : func.params) declareValue(param.name, false);
    analyzeStmt(*func.body);
    popScope();
    if (func.returnType == FuncDef::ReturnType::Int && !mustReturn(*func.body)) {
      fatal("int function may not return on all paths: " + func.name);
    }
  }

  void analyzeStmt(const Stmt &stmt) {
    if (dynamic_cast<const EmptyStmt *>(&stmt)) return;
    if (const auto *block = dynamic_cast<const BlockStmt *>(&stmt)) {
      pushScope();
      for (const auto &child : block->statements) analyzeStmt(*child);
      popScope();
      return;
    }
    if (const auto *exprStmt = dynamic_cast<const ExprStmt *>(&stmt)) {
      analyzeExpr(*exprStmt->expr, false);
      return;
    }
    if (const auto *assign = dynamic_cast<const AssignStmt *>(&stmt)) {
      const auto *value = lookupValue(assign->name);
      if (value->isConst) fatal("assignment to constant: " + assign->name);
      analyzeExpr(*assign->value, true);
      return;
    }
    if (const auto *declStmt = dynamic_cast<const DeclStmt *>(&stmt)) {
      analyzeDecl(*declStmt->decl);
      return;
    }
    if (const auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
      analyzeExpr(*ifStmt->condition, true);
      analyzeStmt(*ifStmt->thenBranch);
      if (ifStmt->elseBranch) analyzeStmt(*ifStmt->elseBranch);
      return;
    }
    if (const auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
      analyzeExpr(*whileStmt->condition, true);
      ++loopDepth_;
      analyzeStmt(*whileStmt->body);
      --loopDepth_;
      return;
    }
    if (dynamic_cast<const BreakStmt *>(&stmt)) {
      if (loopDepth_ == 0) fatal("break outside loop");
      return;
    }
    if (dynamic_cast<const ContinueStmt *>(&stmt)) {
      if (loopDepth_ == 0) fatal("continue outside loop");
      return;
    }
    if (const auto *ret = dynamic_cast<const ReturnStmt *>(&stmt)) {
      if (currentReturnType_ == FuncDef::ReturnType::Void) {
        if (ret->value) fatal("void function returns a value");
      } else if (!ret->value) {
        fatal("int function returns without a value");
      } else {
        analyzeExpr(*ret->value, true);
      }
      return;
    }
    fatal("unsupported statement in semantic analysis");
  }

  void analyzeDecl(const Decl &decl) {
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&decl)) {
      analyzeConstExpr(*constant->initializer);
      declareValue(constant->name, true);
      return;
    }
    if (const auto *var = dynamic_cast<const VarDecl *>(&decl)) {
      analyzeExpr(*var->initializer, true);
      declareValue(var->name, false);
      return;
    }
    fatal("function declaration inside block");
  }

  void analyzeExpr(const Expr &expr, bool requireValue) {
    if (dynamic_cast<const NumberExpr *>(&expr)) return;
    if (const auto *name = dynamic_cast<const NameExpr *>(&expr)) {
      lookupValue(name->name);
      return;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      analyzeExpr(*unary->operand, true);
      return;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
      analyzeExpr(*binary->left, true);
      analyzeExpr(*binary->right, true);
      return;
    }
    if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
      const auto found = functions_.find(call->callee);
      if (found == functions_.end()) fatal("call to unknown function: " + call->callee);
      if (currentFunctionName_ != call->callee && found->second.order >= currentFunctionOrder_) {
        fatal("call to function before declaration: " + call->callee);
      }
      if (found->second.arity != call->args.size()) fatal("wrong argument count: " + call->callee);
      if (requireValue && found->second.returnType == FuncDef::ReturnType::Void) {
        fatal("void function used as value: " + call->callee);
      }
      for (const auto &arg : call->args) analyzeExpr(*arg, true);
      return;
    }
    fatal("unsupported expression in semantic analysis");
  }

  void analyzeConstExpr(const Expr &expr) {
    if (dynamic_cast<const NumberExpr *>(&expr)) return;
    if (const auto *name = dynamic_cast<const NameExpr *>(&expr)) {
      const auto *value = lookupValue(name->name);
      if (!value->isConst) fatal("non-constant in constant initializer: " + name->name);
      return;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
      analyzeConstExpr(*unary->operand);
      return;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
      analyzeConstExpr(*binary->left);
      analyzeConstExpr(*binary->right);
      return;
    }
    fatal("call in constant initializer");
  }


  bool mustReturn(const Stmt &stmt) const {
    if (dynamic_cast<const ReturnStmt *>(&stmt)) return true;
    if (const auto *block = dynamic_cast<const BlockStmt *>(&stmt)) {
      for (const auto &child : block->statements) {
        if (mustReturn(*child)) return true;
      }
      return false;
    }
    if (const auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
      return ifStmt->elseBranch && mustReturn(*ifStmt->thenBranch) && mustReturn(*ifStmt->elseBranch);
    }
    return false;
  }

  void pushScope() { scopes_.emplace_back(); }

  void popScope() { scopes_.pop_back(); }

  void declareValue(const std::string &name, bool isConst) {
    auto &scope = scopes_.back();
    if (!scope.emplace(name, ValueInfo{isConst}).second) fatal("duplicate name in scope: " + name);
  }

  const ValueInfo *lookupValueLocal(const std::string &name) const {
    if (scopes_.empty()) return nullptr;
    const auto found = scopes_.back().find(name);
    return found == scopes_.back().end() ? nullptr : &found->second;
  }

  const ValueInfo *lookupValue(const std::string &name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = it->find(name);
      if (found != it->end()) return &found->second;
    }
    fatal("unknown value: " + name);
  }

  const CompUnit &program_;
  std::unordered_map<std::string, FunctionInfo> functions_;
  std::vector<std::unordered_map<std::string, ValueInfo>> scopes_;
  FuncDef::ReturnType currentReturnType_ = FuncDef::ReturnType::Int;
  std::string currentFunctionName_;
  std::size_t currentFunctionOrder_ = 0;
  std::size_t nextFunctionOrder_ = 0;
  int loopDepth_ = 0;
};

}  // namespace

void analyze(const ast::CompUnit &program) { Analyzer(program).run(); }

}  // namespace toycc::sema
