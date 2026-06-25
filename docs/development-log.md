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

## 2026-06-26 - 性能优化阶段二：内联 + LICM + 调用约定 + 除法 magic（已落地）

**Stage**: Optimization / IR / RISC-V Backend / Testing

**Problem**:
平台反馈性能仅 15/100：p01_const 2769ms（gcc-O2 的 10%）、p07_loop 15s（3%）、p08/p09/p10 超时、p12 104ms（63%）。本地 26 个 perf 用例虽全 PASS 但都是玩具级重构，掩盖了真实规模问题。从 `github/compiler2021` 的 `performance_test2021-private/` 看到平台真用例形态：hoist（10 万次循环 + 15 个不变参数累加 70 遍）、div-opt（1000 个参数各 / 常数）、inst-combining（input=input+1 重复 1 万次）、dce（10 万行未用局部变量）。

**Cause**:
读 `p08`/`p09`/`p10` 生成汇编定位 5 个根因：
1. 调用约定全栈化 —— 被调方把每个 a-reg 参数 `sw` 落栈再 `lw` 回来；调用方对每个实参 `addi sp,-4; sw 0(sp)` push 再 `lw` pop 回 a-reg。3 参纯函数调用 ≈ 12 条 sw/lw 往返。
2. 无函数内联 —— `compute(5,7,11)`、`adj(i,j)` 等纯函数循环调用没被消除。
3. 常量实参调用不 specialize —— `compute(5,7,11)` 实参全常量却走完整 calling convention。
4. LICM 缺失 —— 循环不变量每次迭代重算，IR 优化全是基本块内局部。
5. 非 2 幂除法/取模未强度削减 —— `% 7`、`/ 300` 仍发 `rem`/`div`。

**Resolution**:
分 5 阶段实现，每阶段独立 commit：

1. **阶段 0**：从 compiler2021/private 迁移 4 个 ToyC 用例（hoist/div-opt/inst-combining/dce 各 1）到 `tests/perf/p27-p30.tc`，规模缩减到本地可跑，由 gcc -O2 定标退出码。这给后续优化提供贴近平台的回归基准。
2. **阶段 1**：`src/ir/optim.cpp` 新增 `inlinePass`：调用图 Tarjan SCC 找环、纯 leaf/loop-free/小体（≤400 IR）判定、实参替换内联、Return→Move+Goto。驱动器先对每个 fn 跑局部优化瘦身（让 DCE-heavy 的 `dce-1` 风格 callee 在内联前就崩塌成 `global=i0` 一条），再判定可内联性、内联、再清理，迭代到不动点。结果：p08 -87%、p25 -63%、p26 -56%、p28 -77%、p29 -66%、p30 -52%。
3. **阶段 2**：`licmPass` 利用 IR builder 固定的 while 标签模式识别循环，按"纯定值 + 操作数循环外定义 + 自身在循环内单 def"判定不变量并外提到 cond label 前。`Call` 阻塞其后外提；`LoadGlobal` 仅在循环无 `StoreGlobal` 时可外提。结果：p23 -96%、p27 -95%、p28 -81%、p29 -87%、p10 -23%。
4. **阶段 3**：`src/backend/riscv.cpp` 调用约定改造。`paramSlots` 与 `VarDecl` slot 一视同仁参与寄存器分配；prologue 用 `mv sX, aY`（前 8 参数）或 `lw sX, (i-8)*4(s0)`（溢出参数）把实参搬入 callee-saved s-reg。调用方 emitCall 直接 `loadSlot("aY", arg)`，反向顺序加载到 a0-a7，不再 push/pop 栈往返。修复了"溢出参数被分配到 s-reg 但 prologue 没初始化"的隐藏 bug（p23 退出码错）。结果：p13 fib -16%、p15 ackermann -18%、p06 尾递归 -21%、p19 gcd -21%、p21 tribonacci -16%。
5. **阶段 4**：`emitDivByConst` 实现 Hacker's Delight Fig.10-1 的 magic multiplier 算法。对非 0/±1/±2^k 除数 `d` 算 `(M, s, addMarker)`，发射 `li M; mulh; [add/sub x]; [srai s]; srli sign; add` 序列；Mod 加 `mul; sub`。修复模拟器 `mulh` 用 unsigned 实现的 bug（应是 signed × signed → 高 32 位）。把硬件 20-40 周期的 `div`/`rem` 换成 ~5-10 周期的 mulh+移位序列。模拟步数轻微退化（每条指令算 1 步，多发指令看似变慢）但平台真机预期显著加速。

**Verification**:
全部 30 个 perf 用例（26 推测 + 4 迁移）退出码与 `gcc -O2` 完全一致；功能样例（10 参数、void+全局、短路、嵌套调用、递归）全部 PASS。整体收益（vs 本次优化前）：p08 -87%、p23 -95%、p25 -63%、p26 -56%、p27 -92%、p28 -95%、p29 -96%、p30 -52%、p13/p15 -16~18%。

每阶段独立 commit（8167fc7/01845d3/8c11eb3/eb5b35d/4ee0290），便于回退定位。回退 bug 在阶段 3 通过"先小复现（单调用、循环调用、多 s-reg）逐步验证"的方法识别并修复。

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

## 2026-06-25 - 性能优化阶段：IR 优化与后端窥孔（已落地）

**Stage**: Optimization / RISC-V Backend / IR

**Problem**:
平台反馈 p04 汇编错误（0 分）、p08 运行超时，且多数用例得分 0.24–1.91，主要瓶颈是后端对每条 IR 都做"算出 a0 → sw 栈槽 → 下条 lw 取回"的内存往返。

**Resolution**:
1. 后端立即数溢出修复（救回 p04）：见 `docs/perf.md`。
2. 后端多槽 LRU 寄存器缓存窥孔：cursor 指向刚写入的 slot 时直接 `mv`，避免冗余 `lw`；只在被 `Branch`/`Goto` 命中的 `Label` 整体失效。
3. 新增 `src/ir/optim.cpp`：基本块内常量折叠/传播、代数化简、复写传播、基本块 CSE、单定值-单使用复写合并、全函数死代码删除，迭代至不动点，由 `main` 对每个 build 启用。

**Verification**:
`tools/run_perf.py` 对 12 个样例退出码与 `gcc -O2` 完全一致；模拟步数相对未优化版本下降 12%–44%。

## 2026-06-25 - 后端寄存器分配（investigation, 已回退）

**Stage**: RISC-V Backend / Optimization

**Problem**:
尝试把 `Function::namedSlots`（参数 + `VarDecl`）按使用频率映射到 `s2..s11`，并在 prologue/epilogue 保存/恢复，目的是彻底消除循环承载变量在 cond label 处的 `lw`。引入后 `p08_basic_combined`、`p09_advanced_graph`、`p11_global_const_prop` 退化为模拟超时（无限循环），`p04_common_subexpr` 汇编可执行但退出码错（68 vs 140）。

**Cause** (部分定位):
- 单调用 (`int g(a,b){return a*b;} int main(){return g(3,4);}`) 仍正确，说明调用约定本体无误。
- 递归用例 `p06_tail_recursion` 仍正确。
- 失败集中在"调用方有多个活跃 s-reg 且被调用方也使用重叠 s-reg 的循环"场景。怀疑点是被调用方 prologue `sw sX, frameSize-12-i*4(sp)` 与 epilogue `lw sX, -12-i*4(s0)` 在 sp 与 s0 关系下对齐正确，但调用方在 `emitCall` 里反复 `addi sp,sp,-4/sw 0(sp)` 压参，可能改变了被调用方看到的对齐基址，或 cache 与 s-reg 持久值交互不当；定位未完成。

**Resolution**:
回退 `src/backend/riscv.cpp` 到上一个全绿提交（仅保留 cache + IR opt + 立即数修复）。IR 侧保留 `namedSlots` 元数据（无害），供下次再尝试。

**Next**:
重新引入寄存器分配时，先写一个最小复现：调用方多 s-reg + 在循环中调用一个同样映射连续 s-reg 的纯函数，再逐项排查 prologue/epilogue 与 `emitCall` 的 sp 一致性。
