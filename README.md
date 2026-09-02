# LLVM SecretMixer

This repository is an experimental LLVM fork with an ARM/Thumb-2 instruction
scrambler integrated into LLVM's fast register allocator. It explores changing
physical register assignments and inserting equivalent, conditionally selected
machine instructions for selected functions.

## How it works

When the register allocator is used with `-scrambleName=<substring>`,
each matching machine function is passed through `SecretMixer` after register
allocation. The current implementation:

- reserves most ARM core registers for the selected function;
- remaps a small set of registers and clones supported machine instructions;
- expands selected Thumb-2 multi-instruction pseudos and inserts `IT` blocks;
- keeps mixer state in stack slots, loading the initial value from the
  address `0x50060808`.

Functions whose names do not contain the requested substring stay on the
normal path.

## Build

Build `llc` with ARM support using the normal LLVM prerequisites (CMake, Ninja,
and a C++ compiler):

```sh
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=ARM
cmake --build build --target llc
```

`llvm/lib/CodeGen/SecretMixerImpl.h` includes generated ARM headers through the
checkout-specific path `../dist/...`. Those files are build outputs, not source
files. For the `build` layout above, update both includes to use
`../../../build/lib/Target/ARM/...` before compiling, or use a build layout that
matches the checked-in path.

## Use

Compile LLVM IR for a Thumb-2 ARM target and select the fast allocator. The
substring match applies to every function whose name contains `secret`:

```sh
build/bin/llc \
  -mtriple=thumbv7-none-eabi \
  -regalloc=fast \
  -scrambleName=secret \
  input.ll -o output.s
```


## LLVM documentation

This tree is based on the LLVM monorepo. For general LLVM setup and
contribution guidance, see the [LLVM Getting Started guide](https://llvm.org/docs/GettingStarted.html)
and [LLVM contribution guide](https://llvm.org/docs/Contributing.html).
