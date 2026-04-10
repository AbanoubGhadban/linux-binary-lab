# 05 — Walkthrough: reading the hello-world outputs line by line

> A guided tour of the snapshots in `examples/outputs/`. Open each referenced file alongside this doc and follow along. Every field shown here is the **actual** output from a build on Ubuntu 22.04 / glibc 2.35 / gcc 11.4 — your numbers may differ slightly but the shape will be the same.

The four snapshot files you want open:

- `examples/outputs/hello-dynamic.readelf.txt`
- `examples/outputs/hello-static.readelf.txt`
- `examples/outputs/hello-dynamic.lddebug.txt`
- `examples/outputs/hello-dynamic.strace.txt`

And for size reference:

```
examples/hello-dynamic   15,968 bytes   (~16 KB)
examples/hello-static   900,352 bytes   (~880 KB, 56x larger)
src/mini-readelf         24,688 bytes   (our tool itself)
```

That size ratio alone — the static binary being 56 times larger than the dynamic one — is most of the story. The dynamic binary is tiny because it *outsources* almost all its code to `libc.so.6`. The static binary is huge because it *carries* all that code inside it.

## Part 1: reading `hello-dynamic.readelf.txt`

### The ELF header

```
ELF Header
  Magic:                 7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
  Class:                 ELF64
  Data:                  little-endian
  OS/ABI:                0
  Type:                  0x3  DYN  (shared object OR position-independent executable)
  Machine:               0x3e  x86-64
  Version:               0x1
  Entry point address:   0x1080
```

Five things to notice:

1. **Magic `7f 45 4c 46`** is `\x7f E L F`, the four-byte signature identifying every ELF file. Everything else in the header is read only after this check passes.
2. **Class ELF64** — 64-bit ELF, so all the structs (headers, symbol entries, relocation entries) use 64-bit fields. On a 32-bit system this would be ELF32 and the structs would be half the size.
3. **OS/ABI `0`** is "System V" — the generic Unix ABI. Notice that the static binary below has OS/ABI `3` (Linux/GNU) — a small quirk introduced when `-static` pulls in GNU-specific startup code.
4. **Type `ET_DYN`** — the binary is position-independent. This is what makes it a **PIE**. See [`04-pie-and-aslr.md`](04-pie-and-aslr.md) for why this matters.
5. **Entry point `0x1080`** is suspiciously small. That's because it's *file-relative*: a PIE's virtual addresses all start from zero, and the kernel adds a random base at load time. Compare against the static binary's entry, which is `0x401670` — a fixed absolute address.

Everything below `Entry point` is metadata about the other tables (program/section header offsets and sizes). The important takeaway is: there are **13 program headers** and **31 section headers**. Program headers are what the kernel will use; section headers are what the linker used.

### The program headers

```
Program Headers (13 entries):
  Idx Type          Flags Offset         VirtAddr       FileSize       MemSize
    0 PHDR          R     0x000000000040 0x000000000040 0x0000000002d8 0x0000000002d8
    1 INTERP        R     0x000000000318 0x000000000318 0x00000000001c 0x00000000001c
    2 LOAD          R     0x000000000000 0x000000000000 0x000000000628 0x000000000628
    3 LOAD          R E   0x000000001000 0x000000001000 0x000000000179 0x000000000179
    4 LOAD          R     0x000000002000 0x000000002000 0x0000000000ec 0x0000000000ec
    5 LOAD          RW    0x000000002db8 0x000000003db8 0x000000000258 0x000000000260
    6 DYNAMIC       RW    0x000000002dc8 0x000000003dc8 0x0000000001f0 0x0000000001f0
    7 NOTE          R     ...
    8 NOTE          R     ...
    9 GNU_PROPERTY  R     ...
   10 GNU_EH_FRAME  R     ...
   11 GNU_STACK     RW    0x000000000000 0x000000000000 0x000000000000 0x000000000000
   12 GNU_RELRO     R     ...
```

Walk through this carefully, because these thirteen entries are **all the kernel needs to know to load this program**.

**Entry 0 — `PHDR`** is a self-reference: it tells the runtime where the program header table itself is (offset 0x40, which is right after the 64-byte ELF header). This is needed when the process later wants to walk its own program headers via the `AT_PHDR` auxv entry.

**Entry 1 — `INTERP`** is 28 bytes (0x1c) at file offset 0x318. Those 28 bytes are the NUL-terminated string `/lib64/ld-linux-x86-64.so.2`. When the kernel sees this segment, it loads that interpreter as an ELF and hands control to *it*, not to our program. (Our `mini-readelf` reads this string and prints it a few lines later.)

**Entries 2-5 — `LOAD`** are the four chunks the kernel will `mmap` into the process:

| # | Flags | Size | What it holds (roughly) |
|---|---|---|---|
| 2 | R   | 0x628 bytes  | ELF header + program headers + `.interp` + `.note.*` + `.gnu.hash` + `.dynsym` + `.dynstr` + `.rela.*` |
| 3 | R E | 0x179 bytes  | `.text` — our actual code (379 bytes of machine instructions!) |
| 4 | R   | 0xec bytes   | `.rodata` — the string "hello, dynamic\n" and eh_frame info |
| 5 | RW  | 0x258 → 0x260 bytes | `.init_array`, `.fini_array`, `.dynamic`, `.got`, `.data`, `.bss` |

Notice the last column: `FileSize 0x258` but `MemSize 0x260`. The 8-byte difference is the `.bss` — uninitialized globals that exist in memory but aren't stored on disk (the kernel just zero-fills the extra 8 bytes).

Why are there four `LOAD` segments when "traditionally" we'd expect two (code + data)? **Because the linker splits by permissions.** Each `LOAD` segment has uniform r/w/x flags — that way each one becomes a single `mmap` call with a single set of page permissions. Modern hardening wants code non-writable, read-only data non-executable, and writable data non-executable, which means three different permission sets, plus a fourth `R` segment for metadata that lives before the code. Hence four.

**Entry 6 — `DYNAMIC`** points at the `.dynamic` array we'll decode later. Notice its `p_filesz` of `0x1f0` = 496 bytes. That's enough for 31 entries of `Elf64_Dyn` (each is 16 bytes). Our output below shows 27 entries, which is close enough — the array has some slack plus a `DT_NULL` terminator.

**Entries 7-10** are metadata: NOTE (build-id), GNU_PROPERTY (CET markers for Intel Control-flow Enforcement), GNU_EH_FRAME (exception unwind info). The kernel mostly ignores these.

**Entry 11 — `GNU_STACK`** is the stack permissions marker. `RW` (not `E`) means the stack should be non-executable, which the kernel enforces via NX bit. If this were `RWE`, your stack would be executable — once upon a time this was the default, but it's been a security no-no for twenty years.

**Entry 12 — `GNU_RELRO`** marks a range that the runtime will make read-only after the dynamic linker finishes applying relocations. This prevents certain exploit techniques that overwrite the `.got`/`.got.plt` at runtime.

### The interpreter string

```
Interpreter (PT_INTERP):
  path: /lib64/ld-linux-x86-64.so.2
```

This is the single byte-level reason your program is "dynamic". If you remove this segment from the ELF (or link with `-static`), the kernel skips the whole `ld.so` step. This path is what we're going to chase in `LD_DEBUG` output below.

### The `.dynamic` array

```
Dynamic section (.dynamic):
  Idx Tag             Value
    0 NEEDED          shared library -> libc.so.6
    1 INIT            vaddr 0x1000
    2 FINI            vaddr 0x116c
    3 INIT_ARRAY      vaddr 0x3db8
    4 INIT_ARRAYSZ    8
    5 FINI_ARRAY      vaddr 0x3dc0
    6 FINI_ARRAYSZ    8
    7 GNU_HASH        vaddr 0x3b0
    8 STRTAB          vaddr 0x480
    9 SYMTAB          vaddr 0x3d8
   10 STRSZ           141
   11 SYMENT          24
   12 DEBUG           0x0
   13 PLTGOT          vaddr 0x3fb8
   14 PLTRELSZ        24
   15 PLTREL          0x7
   16 JMPREL          vaddr 0x610
   17 RELA            vaddr 0x550
   18 RELASZ          192
   19 RELAENT         24
   20 FLAGS           0x8
   21 FLAGS_1         0x8000001
   22 VERNEED         vaddr 0x520
   23 VERNEEDNUM      1
   24 VERSYM          vaddr 0x50e
   25 RELACOUNT       3
   26 NULL            (end of .dynamic)
```

This is **the dynamic linker's to-do list**. Read it as: "Dear `ld.so`, here's everything you need to know to run this program." Let me translate the critical entries:

- **Entry 0: `NEEDED libc.so.6`** — "Before running me, load this shared library." If there were multiple libraries, you'd see multiple `NEEDED` entries. This one program needs just libc.
- **Entries 1-6: `INIT` / `FINI` / `INIT_ARRAY` / `FINI_ARRAY`** — "After you've loaded everything, call the functions at these addresses." `INIT_ARRAYSZ 8` means there's exactly one 8-byte function pointer in the init array. That one pointer is the compiler-generated `frame_dummy` (related to exception-handling setup). The legacy `INIT` points at `_init`, a relic from pre-`init_array` days.
- **Entries 7-11: hash and symbol tables** — "Here's where to look up symbols." `GNU_HASH` is the modern fast hash table. `STRTAB` is the string table (141 bytes total). `SYMTAB` is the symbol table; each entry is `SYMENT = 24` bytes.
- **Entry 12: `DEBUG 0x0`** — a slot where `ld.so` will write a pointer at runtime. Debuggers (gdb) read this pointer to discover all loaded shared libraries in a running process.
- **Entries 13-16: PLT machinery** — where the PLT's GOT is (`PLTGOT`), how big the PLT relocations are (`PLTRELSZ 24`, so one 24-byte entry), what flavor of relocations (`PLTREL 7` = `DT_RELA`), and where they are (`JMPREL`). One PLT relocation is for `puts` (the only libc function we actually call).
- **Entries 17-19: non-PLT relocations** — a 192-byte `RELA` section with 8 entries (192/24). These are the data relocations the dynamic linker has to fix up.
- **Entries 22-24: symbol versioning** — says "we need `libc.so.6` version `GLIBC_2.2.5`" (from `VERNEED`). Each symbol in `.dynsym` gets a version selected from this table via `VERSYM`.
- **Entry 26: `NULL`** — the end of the array.

If you take *nothing else* from this walkthrough, take this: **the `.dynamic` array is the interface between the compiled program and the dynamic linker**. Everything `ld.so` does at load time is driven by the entries here.

### The dynamic symbol table

```
Dynamic symbol table (.dynsym, 7 entries):
  Idx Bind   Type     Value              Name
    1 GLOBAL FUNC     0x0000000000000000 __libc_start_main
    2 WEAK   NOTYPE   0x0000000000000000 _ITM_deregisterTMCloneTable
    3 GLOBAL FUNC     0x0000000000000000 puts
    4 WEAK   NOTYPE   0x0000000000000000 __gmon_start__
    5 WEAK   FUNC     0x0000000000000000 __cxa_finalize
    6 WEAK   NOTYPE   0x0000000000000000 _ITM_registerTMCloneTable
```

Note the tool showed 7 entries — index 0 is the reserved `STN_UNDEF` null symbol and is skipped.

All six have `Value 0x0`, which looks broken but is exactly right. Zero-value in `.dynsym` means **undefined** — the symbol is imported from somewhere, and the dynamic linker will fill in the real address at load time. Every one of these is a libc import:

- **`__libc_start_main`** — the function `_start` will call first. It sets up the environment, runs constructors, and finally calls `main`. See [`03-entry-point.md`](03-entry-point.md).
- **`puts`** — the one libc function our program actually calls. `printf("hello…\n")` was optimized to `puts("hello…")` by the compiler, because the format string has no `%` and ends with `\n`.
- **`__gmon_start__`** — weak reference to `gprof` profiling setup. Weak means "if it exists, use it; if not, skip". On a non-profiled build, nobody defines it, so it stays unresolved (`_start` checks for this before calling it).
- **`__cxa_finalize`** — C++ destructor cleanup. Even our C program has this because libc's startup code can be shared with C++.
- **`_ITM_deregisterTMCloneTable` / `_ITM_registerTMCloneTable`** — placeholders for transactional memory support. Almost always weak-undefined and unused. They're in every binary as an historical accident.

**Exactly one function call matters**: `puts`. Everything else is bookkeeping.

## Part 2: reading `hello-static.readelf.txt`

Now the contrast. Open `hello-static.readelf.txt`.

### The ELF header (static)

```
ELF Header
  Magic:                 7f 45 4c 46 02 01 01 03 00 00 00 00 00 00 00 00
  Class:                 ELF64
  Data:                  little-endian
  OS/ABI:                3
  Type:                  0x2  EXEC (classic non-PIE executable)
  Machine:               0x3e  x86-64
  Version:               0x1
  Entry point address:   0x401670
```

Differences from the dynamic header:

1. **Byte 7 of magic is `03`**, not `00`. That byte is `EI_OSABI`. Here it's Linux/GNU (3), because `-static` on glibc pulls in GNU-specific startup code that marks the binary.
2. **Type is `ET_EXEC`** — not a PIE. A fixed-address executable. The linker hard-coded addresses throughout the file; you *can't* relocate it.
3. **Entry point is `0x401670`** — a real, absolute virtual address. At runtime the kernel will map `.text` at `0x401000` and this byte will be exactly at that address. Every run. No randomization of the executable itself.

### The program headers (static)

```
Program Headers (10 entries):
  Idx Type          Flags Offset         VirtAddr       FileSize       MemSize
    0 LOAD          R     0x000000000000 0x000000400000 0x000000000528 0x000000000528
    1 LOAD          R E   0x000000001000 0x000000401000 0x00000009665d 0x00000009665d
    2 LOAD          R     0x000000098000 0x000000498000 0x000000028534 0x000000028534
    3 LOAD          RW    0x0000000c07b0 0x0000004c17b0 0x000000005ae0 0x00000000b490
    4 NOTE          R     ...
    5 NOTE          R     ...
    6 TLS           R     0x0000000c07b0 0x0000004c17b0 0x000000000020 0x000000000068
    7 GNU_PROPERTY  R     ...
    8 GNU_STACK     RW    ...
    9 GNU_RELRO     R     ...
```

Three headers got deleted compared to the dynamic version:

- **No `PHDR`** self-reference (some linker configurations omit it for static binaries)
- **No `INTERP`** — there is no dynamic linker
- **No `DYNAMIC`** — there is no dynamic linking to do
- **No `GNU_EH_FRAME`** dedicated PT header (the section still exists, it's just inside a regular LOAD)

And one new header appeared:

- **`PT_TLS`** — thread-local storage template. Static glibc binaries use TLS internally for their own book-keeping (errno, locales, stdio state), so they need a template for the initial thread's TLS block. The dynamic binary has TLS too, but in the dynamic case `ld.so` allocates and sets it up; in the static case the binary carries the template directly.

Now look at the `LOAD` segment sizes:

| # | FileSize | ~KB |
|---|---|---|
| 0 | 0x528 | 1.3 |
| 1 | 0x9665d | **601**   ← `.text` is 601 KB |
| 2 | 0x28534 | 161 ← `.rodata` is 161 KB |
| 3 | 0x5ae0 | 23 ← `.data` is 23 KB |

The `.text` segment is **~601 KB**. That's glibc. Every libc function you'll ever need — plus a bunch you won't — plus libc's own internals, plus parts of the dynamic linker that even static glibc uses for things like locale loading. That is the entire reason the static binary is 880 KB on disk and the dynamic binary is 16 KB.

Virtual addresses **start at `0x400000`**. That's the classic Linux ELF base address for x86-64 `ET_EXEC` binaries. These are absolute, not file-relative: every time you run the binary, these segments appear at exactly these addresses.

### No dynamic section, no dynamic symbols

```
No PT_INTERP segment.
  -> this looks like a statically-linked binary; the kernel will
     jump straight to e_entry after mapping the PT_LOAD segments.

No .dynsym / .dynstr sections found.
```

Exactly as expected. A static binary has no dynamic linker, no library dependencies, no dynamic symbols to resolve. There's nothing to print here because there's nothing happening.

Note though that a static binary **still has a regular `.symtab` and `.strtab`**, with every symbol from every linked object. You can verify:

```sh
readelf -s examples/hello-static | wc -l   # thousands of symbols
readelf -s examples/hello-dynamic | wc -l  # dozens
```

Our `mini-readelf` only prints the dynamic symbol table (`.dynsym`), not the static one (`.symtab`). This is a deliberate simplification — static symbols aren't interesting for the runtime view we're building.

## Part 3: watching `ld.so` at work — `hello-dynamic.lddebug.txt`

Run `LD_DEBUG=libs ./hello-dynamic 2>&1` and you get:

```
    227076:    find library=libc.so.6 [0]; searching
    227076:     search cache=/etc/ld.so.cache
    227076:      trying file=/lib/x86_64-linux-gnu/libc.so.6

    227076:    calling init: /lib64/ld-linux-x86-64.so.2

    227076:    calling init: /lib/x86_64-linux-gnu/libc.so.6

    227076:    initialize program: examples/hello-dynamic

    227076:    transferring control: examples/hello-dynamic

    227076:    calling fini: examples/hello-dynamic [0]
```

Read this top-to-bottom. It is exactly steps 12-17 from [`02-linking-and-loading.md`](02-linking-and-loading.md) happening in front of you:

1. **`find library=libc.so.6 [0]; searching`** — `ld.so` read our program's `.dynamic` section, found `DT_NEEDED libc.so.6`, and now has to locate the file on disk.
2. **`search cache=/etc/ld.so.cache`** — first it consults the ldconfig cache, which is a prebuilt index of known shared libraries. This avoids a directory scan on every program startup.
3. **`trying file=/lib/x86_64-linux-gnu/libc.so.6`** — the cache gave it a path, and it's opening that file.
4. **`calling init: /lib64/ld-linux-x86-64.so.2`** — `ld.so` is running *its own* init functions. Yes, `ld.so` has constructors it runs on itself. This is the "bootstrap" phase.
5. **`calling init: /lib/x86_64-linux-gnu/libc.so.6`** — libc's constructors. These set up stdio, locale, threading defaults, etc.
6. **`initialize program: examples/hello-dynamic`** — now the `ld.so` is running our binary's own init functions (`.init_array`). For a C program this is minimal.
7. **`transferring control: examples/hello-dynamic`** — **this is the handoff**. `ld.so` jumps to our program's `_start`. From this line onward, our code is running. `main` prints hello, returns, libc calls `exit()`, `exit()` runs atexit handlers and `.fini_array`.
8. **`calling fini: examples/hello-dynamic`** — one of those atexit hooks runs our `.fini_array` back through `ld.so`.

Try `LD_DEBUG=all` instead of `LD_DEBUG=libs` if you want to see the individual symbol resolutions. It's overwhelming on a first read but instructive.

## Part 4: watching the kernel + `ld.so` — `hello-dynamic.strace.txt`

This is the full syscall trace. 40 syscalls to print "hello, dynamic\n". Let me annotate the interesting lines:

```
 1: execve("examples/hello-dynamic", ["examples/hello-dynamic"], ...) = 0
```

The initial `execve`. This is bash running `./hello-dynamic`. Everything after this line is running in the new process.

```
 2: brk(NULL)                               = 0x5d11b5b3c000
```

Ask the kernel "where does the heap currently end?" — the first of many setup calls. This is libc probing for its initial heap location.

```
 3: arch_prctl(0x3001, ...)                 = -1 EINVAL
```

A CPU feature probe. It fails on this system, which is expected and fine — libc falls back to a different setup.

```
 5: access("/etc/ld.so.preload", R_OK)      = -1 ENOENT
```

`ld.so` checks for `/etc/ld.so.preload`, a system-wide `LD_PRELOAD` mechanism. The file doesn't exist (`ENOENT`), so nothing to preload.

```
 6: openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
 7: newfstatat(3, "", ..., AT_EMPTY_PATH)   = 0
 8: mmap(NULL, 101512, PROT_READ, ...)      = 0x77eac6444000
 9: close(3)                                = 0
```

`ld.so` reads the `ld.so.cache` file, mmaps it (101,512 bytes), and closes the fd. The mapping stays — the cache is now in memory as a lookup table.

```
10: openat(AT_FDCWD, "/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3
11: read(3, "\177ELF\2\1\1\3..."..., 832)   = 832
```

**There it is.** `ld.so` opens `libc.so.6` — notice the ELF magic `\177ELF` in the first read. This is exactly the same four bytes we saw at the top of our hello-world binary's header. libc is itself an ELF file and `ld.so` is reading its headers.

```
12: pread64(3, ..., 784, 64)                = 784
13: pread64(3, ..., 48, 848)                = 48
14: pread64(3, ..., 68, 896)                = 68
15: newfstatat(3, "", ..., AT_EMPTY_PATH)   = 0
16: pread64(3, ..., 784, 64)                = 784
```

A series of positioned reads: `ld.so` is reading libc's program headers (at offset 64, the same place our own binary has them — remember, `e_phoff` was typically 64), its notes, and other metadata.

```
17: mmap(NULL, 2264656, PROT_READ, ...)     = 0x77eac6200000
18: mprotect(0x77eac6228000, 2023424, PROT_NONE) = 0
19: mmap(0x77eac6228000, 1658880, PROT_READ|PROT_EXEC, MAP_FIXED, ...)
20: mmap(0x77eac63bd000, 360448, PROT_READ, MAP_FIXED, ...)
21: mmap(0x77eac6416000, 24576, PROT_READ|PROT_WRITE, MAP_FIXED, ...)
22: mmap(0x77eac641c000, 52816, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_ANONYMOUS, ...)
```

**This is `ld.so` doing exactly what the kernel does for our main binary, but in userspace**. It reserves 2.2 MB of virtual address space (line 17) and then carves it up with four `MAP_FIXED` calls matching libc's four `PT_LOAD` segments: read-only header area, read+execute `.text`, read-only `.rodata`, read+write `.data`. Line 22 is an anonymous mapping for `.bss`.

**This is the entire mechanism of dynamic linking in four syscalls.** The kernel handles the main binary; `ld.so` handles everything else by issuing the same kind of `mmap` calls. There's no magic.

```
25: arch_prctl(ARCH_SET_FS, 0x77eac6441740) = 0
```

Set up thread-local storage by pointing the `fs` segment register at the TLS block. On x86-64, TLS is accessed via `fs:offset`. This is how `errno` becomes a per-thread variable.

```
26: set_tid_address(0x77eac6441a10)         = 227082
27: set_robust_list(0x77eac6441a20, 24)     = 0
28: rseq(0x77eac64420e0, 0x20, 0, ...)     = 0
```

More thread-related setup: robust futex list, restartable sequences. Minor glibc housekeeping.

```
29: mprotect(0x77eac6416000, 16384, PROT_READ) = 0
30: mprotect(0x5d11a2222000, 4096, PROT_READ) = 0
31: mprotect(0x77eac6497000, 8192, PROT_READ) = 0
```

Three `mprotect` calls removing write access from regions. This is **RELRO in action** — making the GOT and other relocated-at-load-time regions read-only once relocation is finished. Before this point they had to be writable so `ld.so` could fix them up; after this point they're protected against overwrite exploits.

```
32: prlimit64(0, RLIMIT_STACK, NULL, {...}) = 0
```

Read the stack size limit — libc needs to know it to set up stack guard pages later.

```
33: munmap(0x77eac6444000, 101512)          = 0
```

Unmap the `ld.so.cache` file. `ld.so` is done with it and wants to free the memory.

**At this point `ld.so` is done.** Lines 1-33 are entirely `ld.so`'s work. From here on it's our program and libc's startup code.

```
34: newfstatat(1, "", {st_mode=S_IFCHR|0666, ...}) = 0
35: ioctl(1, TCGETS, ...)                   = -1 ENOTTY
```

stdio is probing stdout (fd 1) to see if it's a terminal. Answer: it's a character device (a pipe or TTY), but not a real TTY (the ioctl failed with `ENOTTY`), so stdio will line-buffer its output.

```
36: getrandom(..., 8, GRND_NONBLOCK)        = 8
```

8 bytes of randomness. These become the stack canary value — a per-process random number the compiler inserts into stack frames to detect overflow.

```
37: brk(NULL)                               = 0x5d11b5b3c000
38: brk(0x5d11b5b5d000)                     = 0x5d11b5b5d000
```

Grow the heap by 0x21000 (132 KB). libc's allocator is setting up its first arena.

```
39: write(1, "hello, dynamic\n", 15)        = 15
```

**This is our `puts` call.** Every previous line was overhead. 38 syscalls to get to the point where we can actually do the one thing the program exists to do: write 15 bytes to stdout.

```
40: exit_group(0)                           = ?
41: +++ exited with 0 +++
```

Clean exit. `exit_group` terminates every thread in the process group (here there's just the one thread). The `+++` line is strace's notation for "process ended".

**38 syscalls of setup, 1 syscall of work, 1 syscall to exit.** That's the price of dynamic linking for a C hello-world. On a static binary, steps 5-33 are skipped entirely — the binary is self-contained so there's nothing to load — and you see roughly 10 syscalls total. Try it:

```sh
strace examples/hello-static 2>&1 | wc -l
```

## Key things to take away

- **Two views of the same file.** Section headers describe link-time organization; program headers describe runtime mapping. The kernel only reads program headers.
- **`PT_INTERP` is the pivot.** If it's present, the kernel loads that interpreter. If not, the kernel runs the binary directly. This single segment determines whether your binary is "dynamic" or "static".
- **The `.dynamic` array is the dynamic linker's instruction manual.** Every entry tells `ld.so` what to load, where the tables are, what to run. You can read it front-to-back.
- **Dynamic linking is userspace code**. `ld.so` uses the same `open`/`mmap`/`mprotect` syscalls you'd use yourself. There is no kernel magic. This is why you can watch it all happen in `strace`.
- **"Hello world" takes 40 syscalls dynamically and ~10 statically.** Most of the work in a dynamic binary happens before `main`. Most of the work in a static binary happens at compile time.
- **File size is the headline.** 16 KB vs 880 KB is the whole story in one number.

## What's next

Now that you can read an ELF by hand, you're ready for [Step 2 — the syscall tracer](../../02-syscall-tracer/). There, we'll build our own version of the `strace` output you just read, using `ptrace`.

## Try this

Before moving on, try running `mini-readelf` on a few more things and predicting what you'll see first:

```sh
./src/mini-readelf /bin/ls
./src/mini-readelf /lib/x86_64-linux-gnu/libc.so.6
./src/mini-readelf /lib64/ld-linux-x86-64.so.2
./src/mini-readelf src/mini-readelf
```

Questions to answer for each:

1. `ET_DYN` or `ET_EXEC`?
2. Is there a `PT_INTERP`? (Note: `ld-linux-x86-64.so.2` doesn't have one — it's the thing everyone else points at.)
3. How many `DT_NEEDED` entries?
4. How many dynamic symbols?
5. Is the entry point a small offset (PIE) or a fixed high address (EXEC)?

If you can answer those five questions by looking at the output, you've internalized step 1.
