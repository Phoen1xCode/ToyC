%code requires {
#include <memory>
#include <vector>

#include "ast/ast.hpp"
}

%{
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "support/diagnostic.hpp"

int yylex(void);
void yyerror(const char *message);

namespace toycc::frontend {
std::unique_ptr<toycc::ast::CompUnit> g_parseResult;
}

namespace {

std::string takeString(char *text) {
  std::string value(text);
  std::free(text);
  return value;
}

template <typename T>
std::unique_ptr<T> own(T *ptr) {
  return std::unique_ptr<T>(ptr);
}

}  // namespace
%}

%union {
  int ival;
  char *str;
  toycc::ast::CompUnit *comp_unit;
  std::vector<std::unique_ptr<toycc::ast::Decl>> *decls;
  toycc::ast::Decl *decl;
  toycc::ast::FuncDef::ReturnType return_type;
  toycc::ast::Stmt *stmt;
  std::vector<std::unique_ptr<toycc::ast::Stmt>> *stmts;
  toycc::ast::Expr *expr;
  std::vector<toycc::ast::Param> *params;
  std::vector<std::unique_ptr<toycc::ast::Expr>> *args;
  toycc::ast::BinaryExpr::Op binary_op;
}

%token <str> ID
%token <ival> NUMBER
%token CONST INT VOID IF ELSE WHILE BREAK CONTINUE RETURN
%token LE GE EQ NE LAND LOR

%type <comp_unit> comp_unit
%type <decls> top_items
%type <decl> top_item decl const_decl var_decl
%type <stmt> block stmt
%type <stmts> stmt_list
%type <expr> expr lor_expr land_expr rel_expr add_expr mul_expr unary_expr primary_expr
%type <params> param_list param_list_opt
%type <args> arg_list arg_list_opt
%type <binary_op> rel_op add_op mul_op

%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE

%start input

%%

input:
  comp_unit { toycc::frontend::g_parseResult.reset($1); }
  ;

comp_unit:
  top_items {
    auto *unit = new toycc::ast::CompUnit();
    unit->decls = std::move(*$1);
    delete $1;
    $$ = unit;
  }
  ;

top_items:
  top_item {
    $$ = new std::vector<std::unique_ptr<toycc::ast::Decl>>();
    $$->emplace_back($1);
  }
| top_items top_item {
    $1->emplace_back($2);
    $$ = $1;
  }
  ;

top_item:
  const_decl { $$ = $1; }
| INT ID '=' expr ';' {
    $$ = new toycc::ast::VarDecl(takeString($2), own($4));
  }
| INT ID '(' param_list_opt ')' block {
    $$ = new toycc::ast::FuncDef(toycc::ast::FuncDef::ReturnType::Int, takeString($2), std::move(*$4), own($6));
    delete $4;
  }
| VOID ID '(' param_list_opt ')' block {
    $$ = new toycc::ast::FuncDef(toycc::ast::FuncDef::ReturnType::Void, takeString($2), std::move(*$4), own($6));
    delete $4;
  }
  ;

decl:
  const_decl { $$ = $1; }
| var_decl { $$ = $1; }
  ;

const_decl:
  CONST INT ID '=' expr ';' {
    $$ = new toycc::ast::ConstDecl(takeString($3), own($5));
  }
  ;

var_decl:
  INT ID '=' expr ';' {
    $$ = new toycc::ast::VarDecl(takeString($2), own($4));
  }
  ;


param_list_opt:
  %empty { $$ = new std::vector<toycc::ast::Param>(); }
| param_list { $$ = $1; }
  ;

param_list:
  INT ID {
    $$ = new std::vector<toycc::ast::Param>();
    $$->emplace_back(takeString($2));
  }
| param_list ',' INT ID {
    $1->emplace_back(takeString($4));
    $$ = $1;
  }
  ;

block:
  '{' stmt_list '}' {
    auto *block = new toycc::ast::BlockStmt();
    block->statements = std::move(*$2);
    delete $2;
    $$ = block;
  }
  ;

stmt_list:
  %empty { $$ = new std::vector<std::unique_ptr<toycc::ast::Stmt>>(); }
| stmt_list stmt {
    $1->emplace_back($2);
    $$ = $1;
  }
  ;

stmt:
  block { $$ = $1; }
| ';' { $$ = new toycc::ast::EmptyStmt(); }
| expr ';' { $$ = new toycc::ast::ExprStmt(own($1)); }
| ID '=' expr ';' { $$ = new toycc::ast::AssignStmt(takeString($1), own($3)); }
| decl { $$ = new toycc::ast::DeclStmt(own($1)); }
| IF '(' expr ')' stmt %prec LOWER_THAN_ELSE {
    $$ = new toycc::ast::IfStmt(own($3), own($5), nullptr);
  }
| IF '(' expr ')' stmt ELSE stmt {
    $$ = new toycc::ast::IfStmt(own($3), own($5), own($7));
  }
| WHILE '(' expr ')' stmt { $$ = new toycc::ast::WhileStmt(own($3), own($5)); }
| BREAK ';' { $$ = new toycc::ast::BreakStmt(); }
| CONTINUE ';' { $$ = new toycc::ast::ContinueStmt(); }
| RETURN ';' { $$ = new toycc::ast::ReturnStmt(nullptr); }
| RETURN expr ';' { $$ = new toycc::ast::ReturnStmt(own($2)); }
  ;

expr:
  lor_expr { $$ = $1; }
  ;

lor_expr:
  land_expr { $$ = $1; }
| lor_expr LOR land_expr {
    $$ = new toycc::ast::BinaryExpr(toycc::ast::BinaryExpr::Op::LogicalOr, own($1), own($3));
  }
  ;

land_expr:
  rel_expr { $$ = $1; }
| land_expr LAND rel_expr {
    $$ = new toycc::ast::BinaryExpr(toycc::ast::BinaryExpr::Op::LogicalAnd, own($1), own($3));
  }
  ;

rel_expr:
  add_expr { $$ = $1; }
| rel_expr rel_op add_expr {
    $$ = new toycc::ast::BinaryExpr($2, own($1), own($3));
  }
  ;

rel_op:
  '<' { $$ = toycc::ast::BinaryExpr::Op::Less; }
| '>' { $$ = toycc::ast::BinaryExpr::Op::Greater; }
| LE { $$ = toycc::ast::BinaryExpr::Op::LessEqual; }
| GE { $$ = toycc::ast::BinaryExpr::Op::GreaterEqual; }
| EQ { $$ = toycc::ast::BinaryExpr::Op::Equal; }
| NE { $$ = toycc::ast::BinaryExpr::Op::NotEqual; }
  ;

add_expr:
  mul_expr { $$ = $1; }
| add_expr add_op mul_expr {
    $$ = new toycc::ast::BinaryExpr($2, own($1), own($3));
  }
  ;

add_op:
  '+' { $$ = toycc::ast::BinaryExpr::Op::Add; }
| '-' { $$ = toycc::ast::BinaryExpr::Op::Sub; }
  ;

mul_expr:
  unary_expr { $$ = $1; }
| mul_expr mul_op unary_expr {
    $$ = new toycc::ast::BinaryExpr($2, own($1), own($3));
  }
  ;

mul_op:
  '*' { $$ = toycc::ast::BinaryExpr::Op::Mul; }
| '/' { $$ = toycc::ast::BinaryExpr::Op::Div; }
| '%' { $$ = toycc::ast::BinaryExpr::Op::Mod; }
  ;

unary_expr:
  primary_expr { $$ = $1; }
| '+' unary_expr { $$ = new toycc::ast::UnaryExpr(toycc::ast::UnaryExpr::Op::Plus, own($2)); }
| '-' unary_expr { $$ = new toycc::ast::UnaryExpr(toycc::ast::UnaryExpr::Op::Minus, own($2)); }
| '!' unary_expr { $$ = new toycc::ast::UnaryExpr(toycc::ast::UnaryExpr::Op::Not, own($2)); }
  ;

primary_expr:
  ID {
    $$ = new toycc::ast::NameExpr(takeString($1));
  }
| NUMBER {
    $$ = new toycc::ast::NumberExpr($1);
  }
| '(' expr ')' {
    $$ = $2;
  }
| ID '(' arg_list_opt ')' {
    $$ = new toycc::ast::CallExpr(takeString($1), std::move(*$3));
    delete $3;
  }
  ;

arg_list_opt:
  %empty { $$ = new std::vector<std::unique_ptr<toycc::ast::Expr>>(); }
| arg_list { $$ = $1; }
  ;

arg_list:
  expr {
    $$ = new std::vector<std::unique_ptr<toycc::ast::Expr>>();
    $$->emplace_back($1);
  }
| arg_list ',' expr {
    $1->emplace_back($3);
    $$ = $1;
  }
  ;

%%

void yyerror(const char *message) { toycc::fatal(message); }
