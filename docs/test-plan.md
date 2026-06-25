# Test Plan

The local success standard is exit-code equivalence. For each ToyC sample, compile it with this compiler, assemble and run the generated RISC-V32 program, and compare the exit code with the expected value or with the equivalent C program compiled by `gcc`.

## Coverage Order

1. Integer literals, expression precedence, and unary operators.
2. Local variables, assignment, and nested scope shadowing.
3. `if`, `else`, `while`, `break`, and `continue`.
4. Short-circuit logic for `&&` and `||`.
5. Function calls, recursion, and multi-argument calls.
6. Global variables and global constants.
7. Return behavior for `int` and `void` functions.
8. Integrated programs that combine declarations, control flow, calls, and globals.

## Test Case Format

Each fixed sample should record:

- ToyC source file.
- Expected exit code, or reference C execution result.
- Feature category covered by the sample.
- Any bug or design decision the sample protects.
