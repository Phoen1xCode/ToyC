# Development Log

This document records problems encountered during development and how they were resolved. Keep entries factual and tied to implementation evidence.

## Entry Format

```md
## YYYY-MM-DD - Short Problem Title

**Stage**: Lexer | Parser | AST | Semantic Analysis | IR | RISC-V Backend | Optimization | Testing | Build

**Problem**:
What failed, including the smallest source program or command that reproduced it.

**Cause**:
The concrete reason after investigation.

**Resolution**:
The change or decision that fixed the problem.

**Verification**:
The test, command, or generated assembly behavior that proved the fix.
```

## Open Issues

No issues recorded yet.

## 2026-06-22 - Bison output directive rejected

**Stage**: Build

**Problem**:
The first Project Skeleton build failed while generating `parser.tab.cpp`. Bison reported a syntax error at the `%output "parser.tab.cpp"` directive in `src/frontend/parser.y`.

**Cause**:
The local Bison version is 2.3, and the project already lets CMake's `bison_target` choose the generated parser output path. The explicit `%output` directive was unnecessary and incompatible with the available tool version.

**Resolution**:
Removed the `%output` directive from `src/frontend/parser.y` and kept CMake responsible for generated parser file names.

**Verification**:
`cmake --build build` completed successfully after the directive was removed.

## 2026-06-22 - Project switched to newer Bison requirement

**Stage**: Build

**Problem**:
The project required Bison 3.x behavior, while CMake initially found macOS `/usr/bin/bison` version 2.3 and rejected the build.

**Cause**:
macOS ships an old Bison version. Homebrew Bison 3.8.2 is installed at `/opt/homebrew/opt/bison/bin/bison`, but CMake must be configured with that executable.

**Resolution**:
Kept `CMakeLists.txt` requiring Bison 3.0 or newer and removed the hard-coded `%output` directive from `src/frontend/parser.y` so CMake's `bison_target` owns the generated output path.

**Verification**:
`cmake -S . -B build -DBISON_EXECUTABLE=/opt/homebrew/opt/bison/bin/bison` configured successfully, and `cmake --build build` completed successfully.

## 2026-06-22 - ToyC pipeline implemented

**Stage**: Lexer | Parser | Semantic Analysis | RISC-V Backend | Testing

**Problem**:
The skeleton compiler ignored input and always emitted a hard-coded `main` returning 0.

**Cause**:
`parseSource`, `analyze`, `buildIr`, and `emitRiscv` were stubs. `parser.y` accepted only empty input and `lexer.l` recognized no ToyC tokens.

**Resolution**:
Implemented Flex tokens and Bison grammar for ToyC, connected parser input through `parser_driver.cpp`, added minimal semantic checks, lowered the AST into typed three-address IR, and emitted RISC-V32 assembly for globals, locals, control flow, calls, recursion, short-circuit logic, constants, and returns.

**Verification**:
`cmake --build build` completed successfully. Behavioral samples for literals, precedence, unary operators, locals, assignment, nested scope shadowing, `if`/`else`, `while`, `break`, `continue`, short-circuit `&&`/`||`, functions, recursion, `void` calls, globals/constants, and ten-argument calls all matched expected exit codes in the local RISC-V instruction simulator.
