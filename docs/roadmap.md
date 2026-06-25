# Implementation Roadmap

Build the compiler in runnable slices. Each slice should leave the project buildable and add tests for the newly supported ToyC feature set.

## Solo Workflow

Develop as one person, but keep the same vertical-slice discipline:

- Keep the active task small enough to build and verify in one sitting.
- Finish one end-to-end behavior before expanding the next feature area.
- Update tests and the development log during the same slice that exposes a problem.
- Keep module boundaries clear even though there is no separate module owner.

Each roadmap stage should produce an end-to-end compiler behavior before expanding the next feature area.

## 1. Project Skeleton

Create AST node structure, shared diagnostics/exit utilities, and keep the build passing.

**Verify**: The compiler target builds successfully.

## 2. Minimal Expressions and Return

Support `int main() { return 42; }`, arithmetic precedence, and unary operators.

**Verify**: Generated RISC-V32 assembly exits with the expected return code.

## 3. Local Variables and Assignment

Support initialized local variables, assignment, and reading local values.

**Verify**: Local variable samples return expected values.

## 4. Control Flow

Support `if`, `else`, `while`, `break`, and `continue` by lowering control flow to labels and branches in IR.

**Verify**: Branch and loop samples return expected values.

## 5. Functions

Support parameters, return values, recursive calls, and `void` functions.

**Verify**: Recursive and multi-argument function samples return expected values.

## 6. Global Declarations

Support global variables in the data section and compile-time global constants.

**Verify**: Global read/write, constant use, and shadowing samples return expected values.

## 7. Semantic Completion

Complete the semantic facts needed by the backend, including function signatures, return behavior, void value restrictions, loop context, and constant evaluation.

**Verify**: Integrated ToyC samples pass.

## 8. Optimization

Add optimizations only after the correct implementation is stable. The first optimization pass set is limited to constant folding, simple algebraic simplification, and dead code deletion. Basic-block register reuse is optional stretch work.

**Verify**: Optimized output preserves exit-code equivalence and improves or preserves runtime on performance samples.
