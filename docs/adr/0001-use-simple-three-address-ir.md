# Use simple three-address IR

The compiler will translate the ToyC abstract syntax tree into a simple three-address intermediate representation before emitting RISC-V32 assembly. The IR will lower ToyC control flow into labels, unconditional jumps, and conditional branches instead of preserving `if` or `while` nodes. This adds one implementation stage compared with direct AST-to-assembly generation, but it keeps semantic lowering, control-flow normalization, and later performance optimizations separate from target assembly emission.
