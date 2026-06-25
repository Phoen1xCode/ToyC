# ToyC 编译系统实践报告

## 1. 任务目标

本项目实现一个 ToyC 到 RISC-V32 汇编的编译器。编译器从标准输入读取 ToyC 源程序，向标准输出写出 RISC-V32 汇编，并接受评测系统可能传入的 `-opt` 参数。

ToyC 覆盖的核心能力包括：

- 全局变量、全局常量、局部变量、局部常量。
- `int` / `void` 函数、参数传递、递归调用、多参数调用。
- 块作用域、变量遮蔽、赋值、表达式语句和空语句。
- `if` / `else`、`while`、`break`、`continue`、`return`。
- 算术、关系、逻辑表达式，以及 `&&` / `||` 短路语义。

## 2. 总体架构

编译管线如下：

```text
stdin ToyC source
  -> Flex lexer
  -> Bison parser
  -> AST
  -> Minimal semantic analysis
  -> Three-address IR
  -> RISC-V32 assembly on stdout
```

各模块职责：

- `src/frontend/lexer.l`：识别关键字、标识符、非负十进制数字、运算符、分隔符，并跳过空白和 C 风格注释。
- `src/frontend/parser.y`：按 ToyC 表达式层级构造 AST，处理声明、函数定义、语句、表达式和函数调用。
- `src/ast/`：用 `std::unique_ptr` 维护 AST 所有权，节点覆盖 ToyC 文法中的声明、语句和表达式。
- `src/sema/semantic.cpp`：执行最小语义分析，记录作用域、函数签名、返回路径、循环上下文和常量约束。
- `src/ir/ir.hpp`、`src/ir/ir_builder.cpp`：将 AST 降低为低级三地址 IR，控制流被规范化为标签、跳转和条件分支。
- `src/backend/riscv.cpp`：将 IR 发射为 RISC-V32 汇编，局部变量、形参和临时值均使用栈槽保存。

## 3. 前端实现

词法分析器只把非负整数识别为 `NUMBER`；负数形式由语法分析器解析为一元负号作用于数字。这避免了 `a-1` 在词法阶段被错误拆分的问题。

语法分析器直接构造 AST。表达式文法按优先级分层：

1. `LOrExpr`：`||`
2. `LAndExpr`：`&&`
3. `RelExpr`：`<`、`>`、`<=`、`>=`、`==`、`!=`
4. `AddExpr`：`+`、`-`
5. `MulExpr`：`*`、`/`、`%`
6. `UnaryExpr`：`+`、`-`、`!`
7. `PrimaryExpr`：标识符、数字、括号表达式、函数调用

`if-else` 使用 Bison 优先级处理悬挂 `else`，使 `else` 绑定到最近的未匹配 `if`，与 C 语言规则一致。

## 4. 语义分析

语义分析假设评测输入为合法 ToyC 程序，但仍维护生成正确代码所需的语义事实：

- 全局作用域记录全局变量、全局常量和函数签名。
- 函数体、语句块维护嵌套词法作用域，支持内层声明遮蔽外层声明。
- 常量声明初始化表达式必须可编译期求值。
- 全局变量初始化表达式必须可编译期求值，便于直接写入 `.data` 段。
- `break` / `continue` 必须位于循环内部。
- `void` 函数不能返回值；`int` 函数不能空返回。
- `int` 函数必须在所有可能路径上返回值。
- `void` 函数若没有显式返回，则后端降低阶段补隐式返回。
- `void` 函数调用不能作为条件、赋值右值或返回值使用。

## 5. 中间表示

IR 使用三地址形式：

- `Label`、`Goto`、`Branch` 表示控制流。
- `Const`、`Move`、`LoadGlobal`、`StoreGlobal` 表示值和存储访问。
- `Unary`、`Binary` 表示表达式运算。
- `Call`、`Return`、`ReturnVoid` 表示函数调用与返回。

`if`、`while`、`break`、`continue` 在 IR 构造阶段被降低为标签和跳转。`IRBuilder` 使用循环标签栈维护当前 `break` 和 `continue` 目标。

逻辑表达式实现短路语义：

- `a && b`：若 `a` 为假，直接得到 `0`，不计算 `b`。
- `a || b`：若 `a` 为真，直接得到 `1`，不计算 `b`。

## 6. RISC-V32 后端

后端生成 RV32IM 风格汇编：

- 每个函数建立栈帧，保存 `ra` 和 `s0`。
- 形参、局部变量和 IR 临时值都映射为 `s0` 相对栈槽。
- 表达式发射时只使用短生命周期寄存器 `a0`、`t0`。
- 函数返回值使用 `a0`。
- 前 8 个整型参数使用 `a0` 到 `a7`，额外参数放在调用者栈上传递。
- 全局变量发射到 `.data` 段，使用 `.word` 保存初始化值。
- 全局常量不分配存储，使用立即数参与 IR 构造和代码生成。

当前实现优先保证正确性，没有引入全局寄存器分配、CFG 优化或复杂优化模块。

## 7. 构建与使用

本地 macOS 环境需让 CMake 使用 Homebrew Bison：

```sh
cmake -S . -B build -DBISON_EXECUTABLE=/opt/homebrew/opt/bison/bin/bison
cmake --build build
```

使用方式：

```sh
build/compiler < input.tc > output.s
build/compiler -opt < input.tc > output.s
```

`-opt` 参数当前被接受但不启用额外优化。

## 8. 测试与验证

本地验证标准为退出码等价。已覆盖的行为样例包括：

- 整数字面量、表达式优先级、一元运算。
- 局部变量、赋值、嵌套作用域遮蔽。
- `if` / `else`、悬挂 `else`、`while`、`break`、`continue`。
- `&&` / `||` 短路，使用有副作用的函数调用确认右侧表达式是否被跳过。
- 普通函数调用、递归调用、十参数调用。
- `void` 函数显式返回和隐式返回。
- 全局变量、全局常量、局部常量遮蔽全局常量。
- `-opt` 参数路径。
- 非法语义样例：缺少返回、循环外 `break`、`void` 值使用、常量赋值、非编译期全局初始化。

最终验证命令：

```sh
cmake -S . -B build -DBISON_EXECUTABLE=/opt/homebrew/opt/bison/bin/bison && cmake --build build
```

该命令已成功完成。随后使用本地 RISC-V 指令模拟器执行生成汇编，以上样例的返回码均与期望值一致；非法语义样例均被编译器拒绝。

## 9. 结论

项目已从骨架实现推进到正确性优先的 ToyC 编译器：前端能够构造完整 AST，语义分析记录必要事实，IR 规范化控制流，后端输出可执行语义一致的 RISC-V32 汇编。后续若继续提升性能，可在当前 IR 上增加常量折叠、代数化简、死代码删除和基本块内寄存器复用。
