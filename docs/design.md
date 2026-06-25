# ToyC Compiler Design

This document captures the current implementation design for the course project.

## Goals

1. Complete a correct implementation for valid ToyC programs.
2. Add performance optimizations only after correctness is stable.

## Pipeline

```text
stdin ToyC source
  -> Flex/Bison frontend
  -> Abstract Syntax Tree
  -> Minimal semantic analysis
  -> Simple three-address IR
  -> RISC-V32 assembly on stdout
```

## Source Layout

```text
src/
  main.cpp
  frontend/
    lexer.l
    parser.y
  ast/
    ast.hpp
    ast.cpp
  sema/
    symbol.hpp
    semantic.hpp
    semantic.cpp
  ir/
    ir.hpp
    ir.cpp
    ir_builder.hpp
    ir_builder.cpp
  backend/
    riscv.hpp
    riscv.cpp
  support/
    diagnostic.hpp
    diagnostic.cpp
```

Keep these modules lightweight. Do not introduce optimizer, CFG, or register-allocation modules until the correct implementation is stable.

## Frontend

Use Flex for tokenization and Bison for parsing. The Bison parser constructs the AST directly.

The lexer emits only nonnegative integer `NUMBER` tokens. A negative numeric form such as `-1` is parsed as unary minus applied to `1`, which avoids ambiguity with subtraction in expressions like `a-1`.

The Bison grammar keeps the assignment's expression layers: `LOrExpr`, `LAndExpr`, `RelExpr`, `AddExpr`, `MulExpr`, `UnaryExpr`, and `PrimaryExpr`. These layers encode operator precedence and associativity directly in the grammar.

## Abstract Syntax Tree

Use a classic inheritance hierarchy with `std::unique_ptr` ownership. The main node groups are:

```text
CompUnit
Decl
  ConstDecl
  VarDecl
FuncDef
Param

Stmt
  BlockStmt
  EmptyStmt
  ExprStmt
  AssignStmt
  DeclStmt
  IfStmt
  WhileStmt
  BreakStmt
  ContinueStmt
  ReturnStmt

Expr
  NumberExpr
  NameExpr
  CallExpr
  UnaryExpr
  BinaryExpr
```

Declaration nodes can appear at global scope or inside statement blocks. Semantic analysis decides whether a declaration is global or local from its containing scope.

## Semantic Analysis

Semantic analysis records only the information required for correct IR generation on valid ToyC programs. It does not aim to provide complete diagnostics for invalid programs.

Global constants are evaluated at compile time and do not receive storage. Global variables receive assembly labels and are emitted in the data section with their initial values. Nested local scopes may shadow global declarations according to ToyC scope rules.

Variables, constants, and parameters are stored in a lexical scope stack. Functions are stored in a separate global function table because ToyC functions cannot be used as values. Function names must not conflict with global variable or global constant names.

Global variable initializers must be compile-time evaluable so their initial values can be emitted in the data section. Local variable initializers may be runtime expressions.

The compiler does not emit signed-overflow checks. It uses normal RISC-V32 integer instructions and assumes valid judge programs do not depend on signed overflow behavior.

Semantic analysis includes a simple `mustReturn` check for function bodies. A `ReturnStmt` returns, a block returns if its last reachable statement must return, and an `if` returns only when both branches must return. `void` functions without an explicit final return receive an implicit empty return during lowering.

## Intermediate Representation

Use a low-level three-address IR. ToyC `if`, `while`, `break`, `continue`, and short-circuit logic are lowered to labels and jumps before backend emission.

Initial IR instruction categories:

```text
label L
goto L
br cond, L_true, L_false
t = const n
t = load name
store name, v
t = unary op v
t = binary op a b
t = call f(args...)
return v
return
```

### Expression Lowering

Expression lowering supports two modes:

1. **Value mode** produces a concrete IR value.
2. **Condition mode** emits branches to caller-provided true and false labels.

Logical `&&` and `||` use condition mode when they appear in control-flow positions such as `if` and `while`. When a logical expression is needed as a value, IR generation creates join labels and materializes the result as `0` or `1`.

### Loop Lowering

`IRBuilder` maintains a stack of loop labels while lowering statements. Entering a `while` pushes its break and continue targets; leaving the loop pops them. `break` lowers to `goto breakLabel`, and `continue` lowers to `goto continueLabel`. The AST is not modified to store loop targets.

## Correct Backend

The first backend stores local variables, parameters, and IR temporaries in stack slots. Registers are used as short-lived operands while emitting individual RISC-V32 instructions.

Global variables are accessed through their assembly labels. Global constants are emitted as immediate values after compile-time evaluation.

Function calls follow the basic RISC-V calling convention: `a0` through `a7` carry the first eight integer arguments, additional arguments are passed on the stack, and integer return values are read from `a0`.

## Testing Standard

Local correctness tests compare program exit codes. A ToyC sample passes when assembly generated by this compiler runs to the expected exit code, or to the same exit code as the equivalent C program compiled by `gcc`.

## Optimization Scope

After the correct implementation is stable, the first optimization pass set is limited to constant folding, simple algebraic simplification, and dead code deletion. Basic-block register reuse is optional stretch work. Full register allocation is out of scope for the first optimized version.
