# Compiler Systems Practice

This context defines the shared language for the ToyC compiler course project.

## Language

**ToyC Compiler**:
A program that translates a valid **ToyC Program** into RISC-V32 assembly whose execution result matches the equivalent C program.
_Avoid_: transpiler, interpreter

**ToyC Program**:
A source program written in the ToyC language described by the course assignment, including global declarations, function definitions, statements, and expressions.
_Avoid_: C program, input file

**Correct Implementation**:
An implementation whose generated assembly terminates within the judge limits and returns the same exit code as the equivalent ToyC program for all valid judge inputs.
_Avoid_: complete compiler, working compiler

**Abstract Syntax Tree**:
A structured representation of a **ToyC Program** after parsing, preserving declarations, statements, and expressions in source-level form.
_Avoid_: parse tree, syntax output

**Flex/Bison Frontend**:
The compiler front end that tokenizes ToyC source with Flex and parses it with Bison into an **Abstract Syntax Tree**.
_Avoid_: hand-written parser, ANTLR frontend

**Intermediate Representation**:
A simple three-address representation of a **ToyC Program** between the **Abstract Syntax Tree** and RISC-V32 assembly.
_Avoid_: LLVM IR, bytecode, assembly

**Minimal Semantic Analysis**:
The semantic pass that records only the information required for correct IR generation for valid ToyC programs, without aiming to diagnose every invalid program.
_Avoid_: full semantic checker, error recovery

**Performance Optimization**:
Any optional improvement intended to reduce the runtime of generated assembly after the **Correct Implementation** is already in place.
_Avoid_: required feature, correctness fix

## Example Dialogue

Developer: Should we add constant folding now?

Domain expert: Only if the **Correct Implementation** is already stable. Constant folding is a **Performance Optimization**, not a substitute for semantic correctness.

Developer: Does a **ToyC Program** include invalid syntax cases?

Domain expert: No. The assignment says judge inputs are valid ToyC programs that satisfy the semantic constraints.

Developer: What does the **Flex/Bison Frontend** produce?

Domain expert: It produces an **Abstract Syntax Tree**, not final assembly.

Developer: Where should control flow be normalized?

Domain expert: In the **Intermediate Representation**, so the RISC-V32 backend does not need to reason directly about nested ToyC statements.

Developer: Should invalid ToyC programs get polished diagnostics?

Domain expert: No. **Minimal Semantic Analysis** assumes judge inputs are valid and focuses on the facts needed to generate correct code.
