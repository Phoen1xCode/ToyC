# Use stack slots for the correct backend

The first correct backend will store local variables, function parameters, and IR temporaries in stack slots, using registers only as short-lived instruction operands. This favors predictable correctness for control flow, calls, and expression evaluation; later optimizations may add register reuse without changing the frontend or IR design.
