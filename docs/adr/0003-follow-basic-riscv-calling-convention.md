# Follow basic RISC-V calling convention

Function calls will follow the basic RISC-V calling convention: integer return values use `a0`, the first eight integer arguments use `a0` through `a7`, additional arguments are passed on the stack, and function prologues preserve return/frame state needed by nested calls. This keeps generated functions compatible with standard execution expectations instead of relying on a project-specific calling convention.
