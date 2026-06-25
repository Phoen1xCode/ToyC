# 本地性能验证框架

## 摘要

编译器现已加入 IR 级与后端窥孔优化。所有 12 个按评测平台用例命名推测的本地性能样例（`tests/perf/p*.tc`）仍与 `gcc -O2` 的退出码完全一致；模拟步数显著下降。

## 验证回路

- `tools/rv32im_sim.py`：纯 Python 的极简 RV32I 用户态解释器，只模拟我们后端实际可能发射的指令子集（`li/mv/addi/add/mul/div/rem/slt/sw/lw/la/j/jal/call/ret/beqz/bnez/...`）。以 `main` 为入口，把 `a0 & 0xff` 作为进程退出码返回；在 `ret` 且 `ra==0` 时认为程序结束。仅用于本地功能等价验证与"步数"代理指标，不用于实测 cycles。
- `tests/perf/*.tc`：按平台用例名 `p01_const … p12_const_expr_chain` 推测的等价程序。每个样例的"期望退出码"由 `gcc -O2 -x c` 编译同一份源文件并执行得到。
- `tools/run_perf.py`：对每个样例分别取 gcc 参考退出码、用本编译器生成汇编、用 `rv32im_sim.py` 模拟运行，输出对照表（含退出码 + 汇编行数 + 模拟步数）。

```sh
python3 tools/run_perf.py
```

## 已落地的优化

1. **后端立即数溢出修复**（最高优先级 / 救回平台 p04 的 0 分）
   当函数局部槽位或入参超过约 508 个 4 字节、栈帧大于 2047 字节时，原先所有 `addi sp,sp,-X`、`sw/lw reg,off(s0)` 因 RISC-V I-type 立即数 12 位有界而成为 `illegal operands`。后端现以 `t6` 作地址临时寄存器，对所有此类访存统一走立即数拆分 helper（`emitAddiReg`/`emitSwReg`/`emitLwReg`）：imm 在范围内直接写，否则 `li t6, imm; add t6, base, t6; … 0(t6)`。
2. **后端多槽 LRU 寄存器缓存窥孔**
   后端维护最近 8 个"刚写入栈槽的本寄存器值"的事实。`load` 命中时直接 `mv`，避免冗余 `lw`。跨越控制流汇合点（被 `Branch`/`Goto` 命中的 `Label`）才整体失效；单纯的 fall-through `Label`（如循环体入口）沿用缓存，正好让循环体进入处保留循环前一次的寄存器值。
3. **IR 级常量折叠与传播**
   基本块内：`Const` 登记、`Move/Unary/Binary` 折叠、`x+0/x-0/x*1/x*0/0+x/0-x` 代数化简。
4. **IR 级复写传播**
   基本块内：`Move d <- s` 之后对 `d` 的读替换为 `s`，跨副作用边界失效。
5. **IR 级基本块公共子表达式消除**
   同一基本块内相同的 `Binary`（含加法/乘法的交换归一化）复用首次结果的槽位，重写为 `Move`。
6. **IR 级复写合并**
   单定值、单使用的纯 `Binary/Unary/Const/LoadGlobal` 通过一条 `Move d <- s` 落到 `d` 时，把该定值的目标直接改为 `d` 并删掉 `Move`。一次扫描确认可合并集合后再一次性重写（避免边改边引用带来的陈旧索引问题——此前的实现曾因此把循环体内的归纳变量更新连同 Move 一起删掉，导致 `p01` 无限循环）。
7. **死代码删除**
   全函数级使用计数，删除无人读的纯定值；迭代到不动点。

这些 pass 在 `toycc::ir::optimizeProgram` 内迭代至不动点（上限 16 轮），由 `main` 对每个 build 都调用，**不影响功能正确性**，因此即使评测不传 `-opt` 也安全启用。

## 性能进展（模拟步数）

| 用例 | platform 基线 | +cache | +IR opt/coalesce |
|---|---|---|---|
| p01_const | 1762 ms / 1.18 | 3426 | **1724** |
| p07_loop | 10145 ms / 0.25 | 340026 | **200024** |
| p08_basic_combined | **超时 / 0** | 750026 | **550024** |
| p11_global_const_prop | 1010 ms / 1.44 | 395026 | **335024** |
| p12_const_expr_chain | 75 ms / 1.91 | 180103 | **160072** |

注：模拟步数 ∝ 指令条数，与真实 cycle 不同；但能反映"相同语义下指令数减少"的程度，作为退化回归断言足够。

## 已知后续项（未做，留作下阶段）

- **用户局部变量 → callee-saved 寄存器分配**。IR 已在 `Function::namedSlots` 标记参数与 `VarDecl` 槽位，便于后端挑选高频槽位映射到 `s2..s11` 以彻底消除循环承载的 `lw/sw`。初步实现存在多 s-reg 跨调用的微妙正确性 bug（涉及被调用方保存/恢复与调用方 s-reg 复用的交互），已在 `docs/development-log.md` 中记录，回退到稳定状态留待后续修正。
- 跨基本块全局寄存器分配 / 循环不变量外提 / 尾递归转循环。