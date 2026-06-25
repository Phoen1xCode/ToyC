# 本地性能测试样例

按评测平台用例名称推测的类似程序，以及从公开性能用例库迁移改写的程序。每个 `pXX.tc` 对应一个优化维度。期望退出码由 `tools/run_perf.py` 用 `gcc -O2 -x c` 编译同一份源并执行得到（ToyC 是合法 C 子集）。

测试目的：在本地验证优化前后的退出码等价（语义正确）和指令条数 / 模拟步数（性能代理指标）。

## 用例清单

### p01–p26：按平台用例名推测的等价程序
- `p01_const` ~ `p12_const_expr_chain`：对应平台已知命名的 12 个用例，覆盖常量、死代码、复写、CSE、代数化简、尾递归、循环、综合优化、全局常量传播、常量表达式链。
- `p13_fib_recursive` ~ `p26_dce_chain`：补充的递归、模幂、Ackermann、Collatz、嵌套循环、GCD 对、Newton 平方根、Tribonacci、分支风暴、外提链、除法优化、指令合并、DCE 链等维度。

### p27–p30：从 compiler2021 私有性能组迁移改写
来源：`github/compiler2021/公开用例与运行时库/performance_test2021-private/` 的 `-1` 变体。SysY 用 `getint()`/`putint()` 做 I/O，ToyC 无 I/O，故改写为固定常量输入 + `return result % 256`。规模相对原用例缩减以适配本地 Python 模拟器与编译时限，但保留各自考察的优化特征。

| 用例 | 改写自 | 考察优化 | 特征 |
|---|---|---|---|
| `p27_hoist` | `hoist-1.sy` | LICM 循环不变量外提 | 8 个不变参数，循环体内累加重复 10 遍 |
| `p28_div_const` | `integer-divide-optimization-1.sy` | 非 2 幂除法 magic number 强度削减 | 20 个参数各 `/3`，内层调用 `/size` |
| `p29_inst_combine` | `instruction-combining-1.sy` | 指令合并 / 窥孔 + 函数内联 | `input=input+1` 重复 80 次 |
| `p30_dce` | `dead-code-elimination-1.sy` | 死代码消除 + 调用开销 | 150 个未用局部变量，只 `global=i0` 有用 |

注：原计划的 `-2`/`-3` 变体经核对与 `-1` 字节级相同（仅 `.in` 循环次数不同），本地模拟器跑不动那么大的规模，故未迁移。

### p31–p36：针对优化缺口的合成用例
为即将实现的优化阶段提前准备的回归用例。每个用例针对一个当前编译器做不好、计划改进的优化维度。

| 用例 | 目标阶段 | 考察优化 | 特征 |
|---|---|---|---|
| `p31_cse_cross_bb` | Phase 5 全局 CSE | 跨基本块公共子表达式 | `i*2+1` 在 if 前后各算一次，BB-local CSE 抓不到 |
| `p32_copy_prop_cross_bb` | Phase 4 全局复写传播 | 跨基本块复写传播 | `Move x<-i` 在 if 之前，if 之后读 x |
| `p33_licm_alias` | Phase 7 LICM 放宽 | LICM 别名分析 | LoadGlobal `read_only` + StoreGlobal `write_only`（不同全局），当前 LICM 拒绝外提 |
| `p34_regalloc_pressure` | Phase 6 线性扫描寄存器分配 | 寄存器分配压力 | 12 个同时活跃命名变量 + s + i = 14 个，超 s2..s11 的 10 个 |
| `p35_branch_fold` | Phase 1 分支常量折叠 | 死分支删除 + `while(1)` 折叠 | `if (0)` 死分支 + `while (1)` 条件折叠 |
| `p36_global_const_chain` | Phase 3 全局常量传播 | 跨基本块常量传播 | 普通变量 `k=17`（非 const）跨 if 传播 |

## 运行

```sh
python3 tools/run_perf.py            # 全部用例
python3 tools/run_perf.py p27_hoist.tc p28_div_const.tc   # 指定子集
```
