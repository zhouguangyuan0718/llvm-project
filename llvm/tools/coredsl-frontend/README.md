# CoreDSL front-end

`coredsl-frontend` is a target-independent first stage for a CoreDSL backend
generator. It has no dependency on TableGen, a target machine, GlobalISel, or
LLVM IR.

```console
coredsl-frontend input.coredsl -o output.coredslir
```

Its output is CDSLIR, a deterministic textual form of the in-memory model in
`CoreDSLIR.h`:

- `coredsl.module` contains source instruction definitions;
- `coredsl.instruction` records operands, encoding fragments, and assembly;
- `behavior` is a scoped tree of declarations, expressions, conditionals, and
  loops; and
- expressions are explicit nodes such as `binary`, `subscript`, and `cast`.

The front-end validates encoding widths and ranges, records implicit encoding
fields as symbols, and rejects undefined symbols in behavior. Lowering CDSLIR
to an instruction-description format is deliberately outside this tool.
