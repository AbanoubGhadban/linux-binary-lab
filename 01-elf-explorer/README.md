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
│   └── mini-readelf.c             ← ~540 LOC ELF64 parser (heavily commented)
├── docs/
│   ├── 01-elf-anatomy.md          ← the two views: sections vs segments
│   ├── 02-linking-and-loading.md  ← static vs dynamic, what execve + ld.so do
│   ├── 03-entry-point.md          ← _start, __libc_start_main, and main
│   ├── 04-pie-and-aslr.md         ← ET_DYN, PIE, and why modern binaries look weird
│   ├── 05-walkthrough.md          ← read the mini-readelf output line by line
│   ├── 06-c-idioms-for-this-project.md ← C language + POSIX deep dive (with labs)
│   └── 07-security-perspective.md     ← ELF security: attacks, defenses, reverse engineering
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

## Using the tool to investigate ELF files

### Your first run — the two hello-worlds

After `make`, start by comparing the two example binaries side by side. These two programs have **identical source code** — the only difference is how they were linked. Every difference you see in the output is caused by static vs dynamic linking.

```sh
# Run them side by side (or use `make run` to do both at once):
./src/mini-readelf examples/hello-dynamic
./src/mini-readelf examples/hello-static
```

Things to look for on this first comparison:

1. **`Type` in the ELF header.** The dynamic binary says `DYN` (a PIE). The static one says `EXEC`. Why?
2. **`Entry point address`.** The dynamic one has a small value like `0x1080` (file-relative). The static one has a big one like `0x401670` (absolute). What does this tell you about ASLR?
3. **`PT_INTERP` segment.** The dynamic one has it (pointing at `/lib64/ld-linux-x86-64.so.2`). The static one doesn't. This single segment is the *reason* one is dynamic and the other is not.
4. **The `.dynamic` section.** Only the dynamic binary has it. Find the `NEEDED` entry — it says `libc.so.6`. That's the library dependency.
5. **The `.dynsym` table.** The dynamic binary imports `puts` and `__libc_start_main` from libc. The static binary has no dynamic symbols at all — everything is baked in.
6. **File sizes.** Run `ls -la examples/hello-*` — the static binary is ~56x larger. All that extra weight is glibc's code, copied into the binary.

### Investigating real-world binaries

Once the two hello-worlds make sense, try the tool on real programs. Each one teaches you something different:

```sh
# A real system program — how many libraries does ls need?
./src/mini-readelf /bin/ls

# libc itself — a shared library, not an executable
./src/mini-readelf /lib/x86_64-linux-gnu/libc.so.6

# The dynamic linker — has no PT_INTERP (it IS the interpreter!)
./src/mini-readelf /lib64/ld-linux-x86-64.so.2

# The tool on itself
./src/mini-readelf ./src/mini-readelf

# Your shell
./src/mini-readelf /bin/bash
```

For each binary, ask yourself these five questions:

1. **`ET_DYN` or `ET_EXEC`?** — is it a PIE or a fixed-address binary?
2. **Is there `PT_INTERP`?** — does it use the dynamic linker?
3. **How many `DT_NEEDED` entries?** — how many libraries does it pull in?
4. **How many dynamic symbols?** — how much does it import/export?
5. **Is `e_entry` a small offset or a big fixed address?** — PIE vs classic layout.

### Going deeper with companion tools

`mini-readelf` shows the structure. These standard Linux tools let you dig further:

```sh
# See the dynamic linker narrate what it does (try libs, symbols, bindings, or all):
LD_DEBUG=libs ./examples/hello-dynamic

# Watch every syscall — you'll see ld.so opening libc, mmapping segments:
strace ./examples/hello-dynamic 2>&1 | head -40

# Compare with the static binary — far fewer syscalls:
strace ./examples/hello-static 2>&1 | head -20

# See ALL symbols (not just dynamic) — thousands in the static binary:
nm examples/hello-static | wc -l
nm examples/hello-dynamic | wc -l

# Disassemble _start to see the entry point code:
objdump -d examples/hello-dynamic | grep -A 15 '<_start>'

# List shared library dependencies:
ldd examples/hello-dynamic

# Strip a binary and confirm it still runs but loses section headers:
cp examples/hello-dynamic /tmp/hello-stripped
strip /tmp/hello-stripped
./src/mini-readelf /tmp/hello-stripped    # sections gone, but segments remain
/tmp/hello-stripped                       # still runs
```

### Exercises

Work through these in order. Each one locks in a concept from the docs:

1. **Static vs dynamic.** Run `mini-readelf` on both hello-worlds. Write down every difference. Then read `docs/02-linking-and-loading.md` and match each difference to the explanation.
2. **The interpreter.** Run `mini-readelf` on `ld-linux-x86-64.so.2` itself. Notice it has **no** `PT_INTERP`. Why? (Because it doesn't need an interpreter — it *is* the interpreter.)
3. **Strip test.** Copy `hello-dynamic`, `strip` it, run `mini-readelf` on both. Notice sections disappear but segments stay. Then run the stripped binary — it still works. Read `docs/01-elf-anatomy.md` § "The two views" and connect the dots.
4. **Entry point.** Find `e_entry` in the `mini-readelf` output, then use `nm` or `objdump -d` to find the symbol at that address. It'll be `_start`. Read `docs/03-entry-point.md` and trace the chain from `_start` → `__libc_start_main` → `main`.
5. **Library count.** Run `mini-readelf` on `/bin/bash`. Count the `DT_NEEDED` entries. Then run `ldd /bin/bash` and compare. The lists should match.
6. **PIE vs EXEC.** Compile a program with `gcc -no-pie hello-dynamic.c -o hello-nopie`. Run `mini-readelf` on it. Notice: `e_type` is now `EXEC`, `e_entry` is a big address, and `PT_LOAD` segments start at `0x400000`. Read `docs/04-pie-and-aslr.md` and connect to what you see.
7. **The strace window.** Run `strace ./examples/hello-dynamic 2>&1 | less` and find each of these in the output: (a) `execve`, (b) `openat` of `libc.so.6`, (c) `mmap` of libc's segments, (d) `write` of "hello, dynamic\n", (e) `exit_group`. Match them to the walkthrough in `docs/05-walkthrough.md`.

## Reading order

**If you're comfortable with C**, read docs 01–05 in order, then read `05-walkthrough.md` with the generated outputs open. **If C is rusty**, start with doc 06 first — it covers every C/POSIX pattern used in the code, with compilable labs.

1. **`docs/06-c-idioms-for-this-project.md`** *(optional first — read this if C feels unfamiliar)* — Pointers, struct casting, unions, bit packing, `mmap`, POSIX I/O, string tables, and common pitfalls. Each section has a lab you can compile and run.
2. **`docs/01-elf-anatomy.md`** — What ELF is, the sections-vs-segments duality, what's in the ELF header.
3. **`docs/02-linking-and-loading.md`** — What actually happens when you type `./hello`. Static vs dynamic linking. The `.dynamic` array.
4. **`docs/03-entry-point.md`** — Why your program doesn't actually start at `main`. The `_start` → `__libc_start_main` → `main` chain.
5. **`docs/04-pie-and-aslr.md`** — Why `hello-dynamic` has type `ET_DYN` instead of `ET_EXEC`, and what ASLR has to do with it.
6. **`docs/05-walkthrough.md`** — Line-by-line tour of the two example outputs, pointing out every field you should notice.
7. **`docs/07-security-perspective.md`** *(optional — read after 01–05)* — How pen testers and reverse engineers read the same ELF output: hardening detection, GOT hijacking, ROP, format strings, binary patching, seccomp, anti-debugging, and a tools/resources list.

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
