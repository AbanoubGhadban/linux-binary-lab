# 02 — Linking and loading

> What actually happens between `gcc hello.c -o hello` and `./hello` saying "hello world". Static vs dynamic linking, the role of `ld.so`, and the `.dynamic` array.

## The two phases: link time and load time

There are two completely separate phases in the life of an executable:

1. **Link time** — the linker (`ld`, usually called via `gcc`) combines your `.o` files with library code to produce the final ELF file. This happens once, at build time.
2. **Load time** — the kernel (and optionally `ld.so`) maps that ELF file into memory and transfers control to it. This happens every time you run the program.

The two phases involve entirely different pieces of software:

| Phase | Who does it | When | What it produces / does |
|---|---|---|---|
| **Link** | `ld` (binutils) | Once, at `gcc` build time | An ELF file on disk |
| **Load (kernel)** | Linux kernel's `execve()` handler | Every invocation | Process with segments mapped |
| **Load (ld.so)** | `/lib64/ld-linux-x86-64.so.2` | After the kernel, for dynamic binaries | Libraries loaded, relocations applied, control passed to `_start` |

Two things are both called "linking", which is confusing:

- **Static linking** happens at link time. All library code is *copied* into the executable.
- **Dynamic linking** happens at both times: at link time the linker just records *references* to libraries; at load time the dynamic linker resolves them.

## Static linking — the simple case

Static linking is conceptually straightforward: every function your program uses, including every libc function, is bundled into the output ELF. The resulting binary is fully self-contained.

```sh
gcc -static hello.c -o hello-static
```

What happens:

1. `gcc` invokes `ld` with the `-static` flag.
2. `ld` looks up libc as `libc.a` (the static archive) instead of `libc.so.6`.
3. `ld` pulls every object file from `libc.a` that resolves any unresolved symbol, *and* every object file those pull in transitively.
4. All that code gets concatenated into the output's `.text`, `.data`, `.bss`, etc.
5. The result is a large ELF file with:
   - **No `PT_INTERP`** — nothing to interpret, just run
   - **No `PT_DYNAMIC`** — no dynamic linking needed
   - **No `DT_NEEDED`** — no libraries to find at run time
   - **`e_type == ET_EXEC`** (by default) — absolute addresses, not PIE
   - A huge `_start` → `__libc_start_main` → `main` chain baked in
   - Its own copy of every libc function used, plus their dependencies

The kernel's job at `execve()` time is dead simple: `mmap` the `PT_LOAD` segments, zero-fill the `.bss` slack, set up the stack with argv/envp/auxv, and jump to `e_entry`. No userspace linker runs before main.

A static "hello world" on glibc is enormous — roughly 900 KB — because it includes all the libc it needs, plus the dynamic linker bits that libc depends on internally, plus `_dl_*` machinery for loading plugins (even the static libc still has some runtime dynamic loading!).

## Dynamic linking — what actually happens

The dynamic case is much more interesting. Compile without `-static`:

```sh
gcc hello.c -o hello-dynamic   # implicitly dynamic
```

Your ELF is now about 16 KB. Where did everything go? It's still on disk — in `/lib/x86_64-linux-gnu/libc.so.6`, shared with every other program on the system. Your binary has been rigged up so that at load time, a runtime component pulls libc in and patches up the addresses.

That runtime component is **the dynamic linker**, also called the **interpreter** or **rtld** (runtime linker). On glibc/amd64 Linux it lives at `/lib64/ld-linux-x86-64.so.2` (which is itself a symlink to `/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2`). And here's the fun fact that everyone learns eventually:

> **`ld.so` is itself an ELF executable.** You can run it directly: `/lib64/ld-linux-x86-64.so.2 ./hello` will load and run `./hello` manually. And if you `./ld-linux-x86-64.so.2 --help` you get usage info.

### The full sequence for `./hello-dynamic`

Here's what happens, step by step. I'll name every participant so you can see the handoffs clearly.

**The shell (bash):**

1. You type `./hello-dynamic` and hit enter.
2. Bash `fork`s.
3. The child bash calls `execve("./hello-dynamic", argv, envp)`. That's a syscall into the kernel.

**The kernel (`fs/binfmt_elf.c`):**

4. Kernel parses the ELF header. It checks the magic, the class (ELF64), the machine (x86-64), the endianness. OK.
5. Kernel walks the program headers looking for `PT_INTERP`. **It finds one**, pointing at `/lib64/ld-linux-x86-64.so.2`.
6. Kernel maps all the `PT_LOAD` segments of `hello-dynamic` into memory at some ASLR-random base (because it's `ET_DYN`).
7. Kernel *also* loads `/lib64/ld-linux-x86-64.so.2` as an ELF, mapping *its* `PT_LOAD` segments at some other ASLR-random base.
8. Kernel constructs the initial stack for the new process: copies argv and envp, then appends the **auxiliary vector (auxv)**. The auxv is a table of kernel-to-userspace information including `AT_BASE` (where ld.so was mapped), `AT_PHDR` (where the program's program headers are in memory), `AT_ENTRY` (the program's own entry point), `AT_RANDOM` (16 random bytes for stack canaries and ASLR), and more.
9. Kernel sets `rip` to **`ld.so`'s** `_start`, not `hello-dynamic`'s. Return from syscall.

**The dynamic linker (`ld-linux-x86-64.so.2`):**

10. Its `_start` runs. It bootstraps itself (yes — `ld.so` has to relocate itself first, using only its own internals, because at this point nothing has resolved its symbols).
11. It reads the auxv from the stack to find the program it's supposed to load (via `AT_PHDR` / `AT_ENTRY`).
12. It walks `hello-dynamic`'s `PT_DYNAMIC` segment to find the `.dynamic` array. From there it learns: what libraries are needed (`DT_NEEDED`), where the dynamic string/symbol tables are (`DT_STRTAB` / `DT_SYMTAB`), where relocations live (`DT_RELA`, `DT_JMPREL`), etc.
13. For each `DT_NEEDED` entry, it `open`s the shared library, maps it into memory, and recurses — loading *its* dependencies too. For `hello-dynamic` there's usually just one: `libc.so.6`.
14. It applies **relocations**: patches up every place in the loaded binaries that refers to a symbol defined elsewhere, using the newly-known addresses. This is what the `.rela.dyn` and `.rela.plt` sections are for. For function calls, this usually means populating entries in `.got.plt` so the PLT trampolines know where to jump.
15. It runs the `.init_array` constructors of each loaded library (and the program itself). This includes C++ global constructors, GCC `__attribute__((constructor))` functions, and libc's own initialization.
16. Finally, it jumps to `AT_ENTRY` — the *program's own* `_start`.

**The program itself:**

17. `_start` (from `crt1.o` inside `hello-dynamic`) runs. It pulls argc/argv/envp off the stack, sets up a few registers, and calls `__libc_start_main`.
18. `__libc_start_main` (now resolved to a real address in `libc.so.6`) does remaining setup (exit handlers, TLS, stack canaries, more constructors), then calls `main`.
19. `main` runs. `printf("hello, world\n")` happens — which goes through libc, eventually calling `write(1, ...)`, which is a syscall.
20. `main` returns. `__libc_start_main` calls `exit()`. `exit()` flushes stdio, runs `.fini_array`, and calls the `exit_group` syscall. The kernel reaps the process.

Step 5 is the one people miss. **The kernel doesn't run your program first. It runs `ld.so` first, and `ld.so` runs your program.**

## The `.dynamic` array

Steps 12-14 above hinged on parsing `.dynamic`. This is the single data structure that makes dynamic linking work. It's an array of `(tag, value)` pairs, where the tag is one of a couple dozen constants and the value is either an address, a count, or an offset into the string table.

Tags you'll actually see in a typical binary, roughly in the order the dynamic linker uses them:

| Tag | Meaning |
|---|---|
| `DT_NEEDED` | "I need this shared library." Multiple entries, one per dependency. |
| `DT_STRTAB` / `DT_STRSZ` | Base and size of the dynamic string table. Names referred to by other tags live here. |
| `DT_SYMTAB` / `DT_SYMENT` | Dynamic symbol table base, and size of each entry (always 24 bytes for ELF64). |
| `DT_HASH` / `DT_GNU_HASH` | Hash table for fast symbol lookup. `DT_GNU_HASH` is the modern, much-faster one. |
| `DT_RELA` / `DT_RELASZ` / `DT_RELAENT` | Base, total size, and per-entry size of the relocation table (for everything except the PLT). |
| `DT_PLTGOT` | Address of the global offset table for PLT entries (what PLT trampolines index into). |
| `DT_JMPREL` / `DT_PLTRELSZ` | The relocations specifically for PLT entries. |
| `DT_INIT_ARRAY` / `DT_INIT_ARRAYSZ` | Array of constructor function pointers. |
| `DT_FINI_ARRAY` / `DT_FINI_ARRAYSZ` | Array of destructor function pointers. |
| `DT_VERNEED` / `DT_VERSYM` | Symbol versioning — which version of each imported symbol is needed. |
| `DT_FLAGS` / `DT_FLAGS_1` | Flags like "enable now binding", "PIE", etc. |
| `DT_NULL` | Array terminator. |

Everything a dynamic linker needs is in there, plus a few things it doesn't strictly need (`DT_DEBUG` is a spot for `ld.so` itself to store a debugger-facing state pointer at runtime — it starts as zero in the file).

## Comparing static and dynamic side by side

Run `mini-readelf` (or `readelf -a`) on both examples and look for:

| Look at | `hello-dynamic` | `hello-static` |
|---|---|---|
| `e_type` | `ET_DYN` (PIE) | `ET_EXEC` |
| `PT_INTERP` | present — `/lib64/ld-linux-x86-64.so.2` | absent |
| `PT_DYNAMIC` | present | absent |
| `DT_NEEDED` | `libc.so.6` | n/a |
| `.dynsym` | small — the few dozen symbols we import | n/a |
| File size | ~16 KB | ~900 KB |
| Virtual addresses in `PT_LOAD` | start at 0 (PIE is position-independent) | start at `0x400000` (fixed) |

The file-size difference is the real giveaway. The dynamic binary *says what it wants* and lets libc stay on disk, shared with every other program. The static binary *carries every byte it needs*.

## Static vs dynamic — tradeoffs

Static linking:

- **Pros:** Zero runtime dependencies. Runs even if `libc` is missing. Slightly faster startup (no dynamic linker). Easier to ship as a standalone binary (Go's default build mode uses this). Immune to library ABI skew.
- **Cons:** Huge file sizes. A security bug in libc requires rebuilding and redistributing every static binary on the system. More memory use (each process has its own copy of libc code). glibc specifically warns against fully-static linking because parts of it (NSS, iconv, locale data) intrinsically use dynamic loading internally and break when statically linked.

Dynamic linking:

- **Pros:** Small executables. One copy of libc in memory for everyone. Libraries can be upgraded independently (a libc security fix propagates to every program without rebuilding them). Supports plugins (`dlopen`).
- **Cons:** Startup does more work. Can break if library ABIs change ("DLL hell"). Requires the linker and libraries to be present (pain in containers and initramfs).

Modern distros default to dynamic everywhere. Modern language runtimes (Go, Rust with musl) often default to static, because they care about deploy simplicity more than memory efficiency.

## The big insight

Dynamic linking is a user-space operation. The kernel doesn't know `libc.so.6` exists — it only knows the binary asked for an interpreter at `/lib64/ld-linux-x86-64.so.2`, and it loaded that. Everything after that — finding libraries, resolving symbols, running constructors — is done by `ld.so` as ordinary userland code.

That's why you can *trace* dynamic linking with `strace`: you'll see `openat()` calls for `libc.so.6`, `mmap()` calls, `read()` calls of ELF headers, and eventually a `write()` from your `printf`. It's all just ordinary syscalls because `ld.so` is just ordinary code.

## Try it yourself

The single most illuminating experiment:

```sh
LD_DEBUG=all ./hello-dynamic 2>&1 | less
```

`ld.so` will narrate every single thing it does: loading libraries, looking up symbols, applying relocations, running init functions. Start with `LD_DEBUG=libs` for a gentler introduction. Then try `LD_DEBUG=bindings`. Then `LD_DEBUG=symbols`. The full `LD_DEBUG=all` is overwhelming but enlightening.

For a second experiment:

```sh
strace ./hello-dynamic 2>&1 | head -40
```

The first ten or so syscalls are the kernel setting up the process; the next dozen are `ld.so` opening `libc.so.6`, mapping it, reading its ELF header. You can *see* the dynamic linking happening as ordinary file I/O.

## Further reading

- Ulrich Drepper, "How to Write Shared Libraries" — the definitive reference (PDF, 80 pages)
- `man ld.so(8)` — the dynamic linker's documentation
- `man ld(1)` — the static linker
- Linux kernel source: `fs/binfmt_elf.c` — the kernel's half
- glibc source: `elf/rtld.c` — the dynamic linker's entry point and main loop
- Eli Bendersky's blog: "Load-time relocation of shared libraries", "PIC in shared libraries on x86-64"
