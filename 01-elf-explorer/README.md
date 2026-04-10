# Step 1 — ELF Explorer

> **Goal of this step:** build a rock-solid mental model of what a Linux ELF executable actually is, by writing a small tool that parses one from scratch and documenting every concept that parsing reveals.
>
> **Output of this step is primarily docs and worked examples**, not a polished tool. The tool (`mini-readelf`) is small on purpose: it's a vehicle for the docs, not the end product.

## Contents

```
01-elf-explorer/
├── README.md                      ← you are here
├── Makefile                       ← build tool + examples + generate snapshots
├── src/
│   └── mini-readelf.c             ← ~400 LOC ELF64 parser
├── docs/
│   ├── 01-elf-anatomy.md          ← the two views: sections vs segments
│   ├── 02-linking-and-loading.md  ← static vs dynamic, what execve + ld.so do
│   ├── 03-entry-point.md          ← _start, __libc_start_main, and main
│   ├── 04-pie-and-aslr.md         ← ET_DYN, PIE, and why modern binaries look weird
│   └── 05-walkthrough.md          ← read the mini-readelf output line by line
└── examples/
    ├── hello-dynamic.c            ← ordinary PIE hello world
    ├── hello-static.c             ← same source, built with -static
    └── outputs/                   ← pre-generated snapshots (committed)
        ├── hello-dynamic.readelf.txt
        ├── hello-static.readelf.txt
        ├── hello-dynamic.lddebug.txt
        └── hello-dynamic.strace.txt
```

## Quick start

```sh
make             # build the tool + both example binaries
make outputs     # regenerate the snapshots in examples/outputs/
make run         # build + dump the tool's output for both examples side by side
make clean       # remove build artifacts
```

Then try the tool on other binaries:

```sh
./src/mini-readelf ./src/mini-readelf       # the tool on itself (PIE)
./src/mini-readelf examples/hello-dynamic   # PIE hello world
./src/mini-readelf examples/hello-static    # classic static hello world
./src/mini-readelf /bin/ls                   # real program
./src/mini-readelf /lib/x86_64-linux-gnu/libc.so.6   # a shared library
```

## Reading order

The docs are numbered. Read them in order, then read `05-walkthrough.md` with the generated outputs in front of you:

1. **`docs/01-elf-anatomy.md`** — What ELF is, the sections-vs-segments duality, what's in the ELF header.
2. **`docs/02-linking-and-loading.md`** — What actually happens when you type `./hello`. Static vs dynamic linking. The `.dynamic` array.
3. **`docs/03-entry-point.md`** — Why your program doesn't actually start at `main`. The `_start` → `__libc_start_main` → `main` chain.
4. **`docs/04-pie-and-aslr.md`** — Why `hello-dynamic` has type `ET_DYN` instead of `ET_EXEC`, and what ASLR has to do with it.
5. **`docs/05-walkthrough.md`** — Line-by-line tour of the two example outputs, pointing out every field you should notice.

## What you should be able to answer after step 1

- Why can you `strip` a binary and have it still run?
- What's the difference between a section and a segment?
- What does `PT_INTERP` do, and why is it the key to understanding dynamic linking?
- What is `e_entry` and why does it point at `_start` instead of `main`?
- Why is a statically-linked "hello world" ~900 KB?
- Why is the dynamic one only ~16 KB, and where did the rest go?
- What is `ET_DYN`, and why does it mean both "shared library" and "PIE executable"?
- What does `ld.so` do that the kernel *doesn't* do?
- What's in `.dynamic` and why does the dynamic linker need it?
- What's the difference between `.symtab` and `.dynsym`?
- Why can you run `ld-linux-x86-64.so.2` as if it were a program?

If any of those feel fuzzy, re-read the relevant doc and re-run `./src/mini-readelf` on a real binary. The tool is designed to be run many times against many different files.

## Out of scope for this step

- Anything that traces a running program — that's [step 2](../02-syscall-tracer/).
- Intercepting library calls — that's [step 3](../03-library-tracer/).
- Parsing ELF32, big-endian ELF, or non-x86-64 architectures. The tool rejects them.
- Relocation record parsing, exception-handling tables (`.eh_frame`), DWARF debug info. Each of those is a rabbit hole of its own.

## Prerequisites

- `gcc`, `make`
- `libc6-dev` (Ubuntu/Debian) — needed so `-static` links against `libc.a`
- On other distros the equivalent package is usually called `glibc-static` (Fedora/RHEL) or similar.
- Optional: `strace` (for one of the generated snapshots — the Makefile skips it gracefully if it's missing)

## License

MIT or equivalent — this is a learning project, use any of it however you like.
