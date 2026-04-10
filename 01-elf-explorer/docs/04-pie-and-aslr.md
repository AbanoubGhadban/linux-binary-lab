# 04 — PIE, `ET_DYN`, and ASLR

> Why your `hello-dynamic` says `ET_DYN` instead of `ET_EXEC`, why `e_entry` is suspiciously small (like `0x2190`), and what that has to do with security.

## The historical baseline: `ET_EXEC`

Before about 2010, every Linux executable was type `ET_EXEC`. Its program headers listed `PT_LOAD` segments at **fixed absolute virtual addresses** — typically starting at `0x400000` on x86-64. The kernel's job at `execve()` was straightforward: whatever addresses the binary said, map it there. Same address every time. If you ran the program twice, `main` was at the exact same virtual address both times.

This made everything easy:

- Debuggers knew where `main` would be.
- The linker could hard-code absolute addresses everywhere in the binary.
- Relocations were rare because nothing moved.

It also made exploits easy. If an attacker knew there was a buffer overflow in your program, they could just hardcode the address of `system()` or a gadget in libc and jump to it. Every run of the program had the same memory layout. Every user on every machine running the same distro had the same addresses. This is called **return-to-libc** and **ROP** (return-oriented programming), and it's the main reason we now live in a different world.

## What ASLR is

**Address Space Layout Randomization** is the kernel's answer: every time you run a program, the kernel picks random base addresses for:

- The stack
- The heap (well, `brk`)
- Any `mmap`-ed regions
- Shared libraries loaded by `ld.so`
- Optionally, the executable itself

ASLR has been on in Linux since 2.6.12 (2005) for the first four of those. The executable itself was still loaded at a fixed address — because `ET_EXEC` binaries have absolute addresses baked in; if you moved them, the binary would break.

The fix was to rebuild executables as **position-independent code** — the same way shared libraries had always been built — so that they could be loaded at *any* address and still work. A position-independent executable is a **PIE**, and in the ELF header it's marked as `ET_DYN`.

## `ET_DYN` is overloaded

Here's the confusing part. The ELF standard has only four core types: `ET_REL`, `ET_EXEC`, `ET_DYN`, `ET_CORE`. And `ET_DYN` is used for both:

1. Shared libraries (`libc.so.6`, `libpthread.so.0`, your own `.so` files)
2. PIE executables (`/bin/ls` on modern Debian/Ubuntu, your `hello-dynamic`)

How does the kernel tell them apart? **It doesn't, really.** It looks at whether the file has a `PT_INTERP` segment:

- If it has `PT_INTERP`, treat it like an executable: load the interpreter, hand off to `ld.so`.
- If it doesn't have `PT_INTERP`, it's probably a shared library being `dlopen`-ed or directly loaded.

But the distinction is fuzzy. `ld-linux-x86-64.so.2` is technically a shared library, but you can execute it (`./ld.so /bin/ls`). `libc.so.6` is also somewhat executable — on glibc there's an `__libc_main` that prints version info if you run it directly. "Is this file an executable or a library?" turns out not to be a yes/no question.

The practical upshot: when `mini-readelf` says `Type: ET_DYN`, you have to look at `PT_INTERP` and the filename to figure out what kind of `ET_DYN` it is.

## "PIE" as a concept

A PIE binary has to work when loaded at any random base address. That means:

- **No absolute addresses in code.** Every reference to a global variable or function uses a relative address (PC-relative, via `rip` on x86-64) or goes through a table that the loader fills in.
- **The `e_entry` field stores a file-relative offset, not an absolute address.** You'll see values like `0x2190` instead of `0x401650`.
- **`PT_LOAD` segments use `p_vaddr` values starting from 0.** The kernel will add a random base offset when it maps them.

Here's the contrast, from your two example binaries:

```
# hello-dynamic (PIE, ET_DYN)
Entry point address:   0x2190
PT_LOAD VirtAddr:      0x000000000000  ← starts at 0
PT_LOAD VirtAddr:      0x000000001000
PT_LOAD VirtAddr:      0x000000003000
PT_LOAD VirtAddr:      0x000000005d68

# hello-static (ET_EXEC, fixed addresses)
Entry point address:   0x401650
PT_LOAD VirtAddr:      0x000000400000  ← fixed!
PT_LOAD VirtAddr:      0x000000401000
PT_LOAD VirtAddr:      0x000000498000
PT_LOAD VirtAddr:      0x0000004c17b0
```

The static binary's segments are at fixed addresses starting at `0x400000`. The PIE binary's segments nominally start at 0, but the kernel will add a random base like `0x5612f3a00000` at runtime.

## How PIE works under the hood

Position-independent code predates PIE by decades — shared libraries have always needed it, because you don't know in advance where each library will be loaded in a process. The trick is the same in both cases:

1. **For function calls and global variable accesses in the same binary:** Use PC-relative addressing. On x86-64 this is `lea rax, [rip + offset]` / `mov rax, [rip + offset]`. The offset is fixed at link time; the actual address resolves at run time based on `rip`.
2. **For calls to functions in other shared objects:** Go through the PLT (Procedure Linkage Table). The PLT is just a tiny stub that reads a real address out of the GOT (Global Offset Table). The GOT is filled in by the dynamic linker at load time with actual addresses.
3. **For imported data:** Go through the GOT directly. `mov rax, [rip + x@GOTPCREL]` — load the address of `x` from a GOT slot using a PC-relative address.

The first call into any given shared-library function is slow, because it triggers the lazy resolver in `ld.so` to figure out the real address and patch the GOT slot. Subsequent calls are cheap: PLT stub → GOT slot → real address, two indirections that the CPU's branch predictor handles well.

**The upshot**: PIE adds a small performance cost (an extra level of indirection for some memory accesses, and relocations to apply at load time), but in return your program can be loaded anywhere and ASLR-randomized. On modern x86-64 with good branch prediction, the overhead is often negligible. On constrained architectures (32-bit, no PC-relative addressing) it can be bigger.

## Why this matters for security

ASLR hardens programs against several attack classes:

- **Return-to-libc**: attacker wants to call `system("/bin/sh")` using a known address of `system`. If libc is at a random base, they don't know the address.
- **ROP (return-oriented programming)**: attacker wants to chain "gadgets" — short sequences of existing instructions followed by `ret` — from libc or the executable itself. Randomizing the base means they don't know gadget addresses.
- **Information leak + targeted overwrite**: writing a known pointer to a known location. Randomization means knowing "where" requires first leaking an address, which is an extra step attackers must arrange.

ASLR isn't a silver bullet. There are well-known bypasses:

- **Brute force on 32-bit systems.** The randomized portion is small, sometimes as little as 16 bits. An attacker can just try all possibilities until something works.
- **Information leaks.** A bug that prints the address of any libc function also reveals the base of libc, which gives you everything else in libc (since the libc internal layout is fixed).
- **Partial overwrites.** If only part of a pointer is random, overwriting just the low bits can still land on a useful location.
- **Non-randomized data.** The vDSO used to be at a fixed address on many systems. Certain kernel structures are still at fixed addresses. Some old `ET_EXEC` binaries exist on most systems.
- **ASLR doesn't apply where PIE isn't enabled.** Older software compiled without `-fPIE -pie` gets a fixed executable base even with ASLR on everywhere else.

This is why PIE is now the default on most modern distros: ASLR is only useful for the parts of the process it can actually randomize, and for that you need the executable itself to be position-independent.

## How to check if a binary is PIE

Quick checks:

```sh
# file(1) will say "pie executable"
file hello-dynamic
# hello-dynamic: ELF 64-bit LSB pie executable, x86-64, ...

file hello-static
# hello-static: ELF 64-bit LSB executable, x86-64, ...

# or via readelf — look for Type
readelf -h hello-dynamic | grep Type
# Type: DYN (Position-Independent Executable file)

readelf -h hello-static | grep Type
# Type: EXEC (Executable file)
```

Or with our `mini-readelf`:

```
Type:  0x3  DYN  (shared object OR position-independent executable)
```

(Our tool can't tell a PIE from a library without also checking `PT_INTERP` — see the note further down.)

## Heuristic: is it a PIE executable or a shared library?

If all of these are true, it's almost certainly a PIE executable, not a library:

- `e_type == ET_DYN`
- Has `PT_INTERP`
- The `PT_INTERP` path is a normal dynamic linker (`/lib64/ld-linux-*.so.*`)
- Has a `_start` symbol in `.dynsym`

If `e_type == ET_DYN` and `PT_INTERP` is *absent*, it's almost certainly a shared library — libraries don't usually have an interpreter because they're meant to be loaded into an existing process, not run directly.

A good future enhancement to `mini-readelf` would be to print this heuristic explicitly ("This looks like a PIE executable" vs "This looks like a shared library"). Left as an exercise.

## Controlling it at compile time

You can force each mode via `gcc` flags:

```sh
gcc hello.c -o hello                # default — likely PIE on modern distros
gcc -no-pie hello.c -o hello-nopie  # classic ET_EXEC layout
gcc -pie -fPIE hello.c -o hello-pie # explicit PIE
gcc -static hello.c -o hello-static # static, ET_EXEC
gcc -static-pie hello.c -o hello-static-pie  # modern: static AND PIE
```

`static-pie` is a fun mode: statically linked (no `ld.so`, no libc.so.6) but still position-independent and randomized. Supported on modern glibc/kernel combinations.

Check what you got:

```sh
./src/mini-readelf hello | grep Type
```

## Key takeaways

- `ET_DYN` means **position-independent**. It's used for both shared libraries and PIE executables.
- The distinction between "PIE executable" and "shared library" comes from `PT_INTERP` presence and the file's intended use, not from `e_type`.
- `e_entry` in a PIE is a **file-relative offset**. The real runtime address is `base + e_entry` where `base` is wherever the kernel decided to map it.
- PIE exists so that **ASLR can apply to the executable itself**, not just libraries and the stack.
- Modern distros default to PIE. Older binaries and explicitly `-no-pie` builds are still `ET_EXEC`.

## Further reading

- Matt Miller (Microsoft), "Modeling the exploit mitigation landscape" — measures ASLR's real-world value
- PaX / Grsecurity documentation on full ASLR — the hardened approach
- Ulrich Drepper, "Position-Independent Executables (PIE)" — original glibc rationale
- LWN article: "Static PIE" — how static-pie works
- kernel source: `fs/binfmt_elf.c` — look for `load_elf_binary` and `elf_map` for how the kernel decides where to put things
