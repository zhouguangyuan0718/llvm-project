# LLVM 23 generated-backend bring-up

This guide exercises the current end-to-end path:

```text
CoreDSL
  -> TargetModel
  -> generated TableGen and C++ target
  -> LLVM 23 llc
  -> selected target instructions
```

Generation itself links only LLVM Support. It does not run an LLVM IR or
CodeGen pipeline. The generated files are subsequently compiled as an LLVM 23
experimental target for validation.

## 1. Build the standalone generator

Point `LLVM_DIR` at an installed or built LLVM 23 CMake package:

```sh
cmake -S coredsl-backend-gen -B build-coredsl -G Ninja \
  -DLLVM_DIR=/path/to/llvm23/lib/cmake/llvm
cmake --build build-coredsl --target coredsl-backend-gen
ctest --test-dir build-coredsl --output-on-failure
```

The generator can instead be enabled in the monorepo with
`-DLLVM_ENABLE_PROJECTS=coredsl-backend-gen`.

## 2. Generate the Tiny32 target

The output directory must not already exist:

```sh
build-coredsl/coredsl-backend-gen \
  --emit-llvm23-backend="$PWD/llvm/lib/Target/Tiny32" \
  coredsl-backend-gen/test/Model/tiny32.core_desc
```

This produces:

```text
llvm/lib/Target/Tiny32/
  CMakeLists.txt
  Tiny32.td
  Tiny32Target.cpp
  TargetInfo/
    CMakeLists.txt
    Tiny32TargetInfo.h
    Tiny32TargetInfo.cpp
```

`Tiny32.td` contains the physical registers, register class, register bank,
instruction definitions, patterns, and the minimal return pseudo. The C++
source contains target/MC registration and the minimal GlobalISel call
lowering, legalizer, register-bank mapping, instruction selector, and pass
configuration.

## 3. Build llc with the generated target

Tiny32 deliberately uses an explicit `-march` name and `UnknownArch`; it does
not add a central LLVM triple mapping:

```sh
cmake -S llvm -B build-tiny32 -G Ninja \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Tiny32 \
  -DLLVM_TARGETS_TO_BUILD=host \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_BUILD_TOOLS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tiny32 --target llc -j8
```

## 4. Compile LLVM IR through instruction selection

```sh
build-tiny32/bin/llc \
  -march=tiny32 \
  -mtriple=unknown-unknown-none \
  -global-isel \
  -global-isel-abort=1 \
  -O0 \
  -stop-after=instruction-select \
  coredsl-backend-gen/test/GlobalISel/tiny32-add.ll \
  -o -
```

The resulting MIR is marked `legalized`, `regBankSelected`, and `selected`,
and its body contains:

```text
%2:gpr = ADD %0, %1
%3:gpr = SUB %2, %1
$r0 = COPY %3
COREDSL_RET implicit $r0
```

The selector can also be tested directly from post-legalizer,
post-register-bank MIR:

```sh
build-tiny32/bin/llc \
  -march=tiny32 \
  -mtriple=unknown-unknown-none \
  -run-pass=instruction-select \
  coredsl-backend-gen/test/GlobalISel/tiny32-add.mir \
  -o -
```

## Current boundary

The generated ABI scaffolding accepts non-variadic functions whose arguments
are single scalar values of `register_width`, passed in the generated
registers in declaration order. It accepts either no return value or one
scalar return value in register zero. This is sufficient for the LLVM IR
arithmetic probe.

Calls, stack arguments, memory operations, PHIs/control-flow completeness,
register allocation, assembly printing, machine-code emission, and object
writing are not implemented yet. Stop at instruction selection as shown
above.
