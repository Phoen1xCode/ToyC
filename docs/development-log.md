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

## 2026-06-25 - 评测机 p04 汇编错误：栈帧超出 12 位立即数范围

**Stage**: RISC-V Backend / Testing

**Problem**:
评测平台反馈 p04_common_subexpr 报「汇编错误」0 分。本地用 `riscv64-elf-as` 复现：当函数局部槽位或入参超过约 508 个 4 字节时（frameSize > 2047），后端生成的所有 `addi sp, sp, -frameSize`、`sw ra, frameSize-4(sp)`、`addi s0, sp, frameSize` 以及栈槽 `sw/lw reg, offset(s0)` 都因 I-type 立即数范围 [-2048, 2047] 越界而成为 `illegal operands`。`emitCall` 里溢出参数的偏移 `(count-1-i)*4` 同样会越界。

**Cause**:
后端代码把所有立即数直接写进 `addi` 与 `sw/lw` 的 imm 区，没有为大栈帧/大参数列表做立即数拆分。RISC-V I-type imm 只有 12 位有符号。

**Resolution**:
在 `src/backend/riscv.cpp` 中引入立即数拆分 helper，并把 t6 作为专用地址临时寄存器（当前后端未使用 t6）：

- `emitAddiReg(rd, rs, imm)`：imm 在范围内走 `addi`；否则 `li t6, imm; add rd, rs, t6`。`emitAddSp` 是它的特化。
- `emitSwReg(reg, offset, base)` / `emitLwReg(reg, offset, base)`：offset 在范围内直接写；否则 `li t6, offset; add t6, base, t6; sw/lw reg, 0(t6)`。
- 把 prologue/epilogue、形参保存、`loadSlot`/`storeSlot`、`emitCall` 中所有栈/store/load 全部改走 helper。

**Verification**:
`cmake --build build` 通过；600 个局部变量与 700 个入参的 stress 用例 `riscv64-elf-as -march=rv32im -mabi=ilp32` 全部汇编通过；`tools/run_perf.py` 12 个性能样例退出码仍全部等价于 gcc -O2 参考实现。

## 2026-06-25 - 远程评测机链接 `yy_scan_bytes` 未定义

**Stage**: Build / RISC-V Backend

**Problem**:
提交到远程评测平台时构建失败，链接器报 `undefined reference to yy_scan_bytes(char const*, unsigned long)`，而本机 macOS 构建一直正常。

**Cause**:
`src/frontend/parser_driver.cpp` 手写了 `extern YY_BUFFER_STATE yy_scan_bytes(const char*, unsigned long)`，参数类型硬编码为 `unsigned long`。Flex 生成的真实签名为 `yy_scan_bytes(const char*, yy_size_t)`，而 `yy_size_t` 的定义随 Flex 版本不同：本机 Flex 2.6.4 为 `typedef size_t yy_size_t`（macOS LP64 下 `size_t == unsigned long`），符号 mangling 一致所以能链接；远程评测机的旧版 Flex 把 `yy_size_t` 定义为 `int`，生成的定义符号以 `int` 参数 mangling，与 `parser_driver.cpp` 期望的 `unsigned long` 不匹配，于是链接器找不到符号。

此前的 commit “use unsigned long for yy_scan_bytes to match Flex's size_t on Linux” 只在“远程 Flex 的 yy_size_t 恰好是 size_t”时成立，对旧 Flex 无效。

**Resolution**:
不再手写 Flex 函数的类型声明，改为让 Flex 生成头文件并直接包含。改动：
1. `CMakeLists.txt` 的 `flex_target` 增加 `DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/lexer.lex.hpp`，使 Flex 通过 `--header-file` 生成包含正确 `yy_size_t` 与函数声明的头文件。
2. `src/frontend/parser_driver.cpp` 改为 `#include "lexer.lex.hpp"`，删除手写的 `extern YY_BUFFER_STATE yy_scan_bytes`、`yy_delete_buffer`、`yylex_destroy` 以及 `struct yy_buffer_state`/`using YY_BUFFER_STATE` 自定义类型。`extern int yyparse(void);`（Bison 的）保留。

这样无论 Flex 把 `yy_size_t` 定义为 `int` 还是 `size_t`，`parser_driver.cpp` 的声明与生成的 `lexer.lex.cpp` 中的定义来自同一份 Flex 产物，类型永远一致。

**Verification**:
`rm -rf build && cmake -S . -B build -DBISON_EXECUTABLE=... && cmake --build build` 干净构建通过；`lexer.lex.hpp` 中 `yy_size_t` 与 `yy_scan_bytes` 声明与 `lexer.lex.cpp` 一致；递归、循环/`break`/`continue`、全局常量+`void`、十参数、逻辑短路等样例汇编仍能正常生成。

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
