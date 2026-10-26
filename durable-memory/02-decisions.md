# Public decisions

- Schema 2 uses arrays of signatures so one name can have exact overloads.
- Conversions are declared and single-step; the compiler resolves them before bytecode is written.
- Object arithmetic operators are schema mappings to ordinary methods.
- Host-owned collections use `len()` and `at()`; no collection opcode is part of the bytecode.
- Compiler-inlined functions do not add a runtime function table.
- Runtime errors retain the source line and column when the bytecode line table provides them.
