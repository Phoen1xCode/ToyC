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

## 运行

```sh
python3 tools/run_perf.py            # 全部用例
python3 tools/run_perf.py p27_hoist.tc p28_div_const.tc   # 指定子集
```
