# 01 — Anatomy of an ELF file

> What an ELF file is made of, and the two very different "views" that the linker and the kernel take of the same bytes.

## What ELF even is

ELF — the **Executable and Linkable Format** — is the file format used on Linux, most BSDs, and many embedded systems for:

- **Executables** (`/bin/ls`, your compiled C program)
- **Shared libraries** (`libc.so.6`, `libpthread.so.0`)
- **Object files** (`*.o` from the compiler, before linking)
- **Core dumps** (what the kernel writes when a process crashes)

All four are the same file format. The `e_type` field in the header tells you which variant you're looking at. The same tools (`readelf`, `nm`, `objdump`) work on all of them.

ELF replaced the older `a.out` format in Linux in the mid-1990s. Its design is deliberately dual-purpose: it supports both **linking** (combining `.o` files into an executable) and **execution** (loading an executable into a running process). Both roles are baked into the file layout — and they see different things.

## The two views

This is the single most important thing to understand about ELF:

> **Every ELF file has two independent tables of contents: one for the linker, one for the loader.**

|  | Link-time view | Run-time view |
|---|---|---|
| Described by | **Section headers** (`Elf64_Shdr`) | **Program headers** (`Elf64_Phdr`) |
| Called | **Sections** (`.text`, `.data`, `.bss`, …) | **Segments** (`PT_LOAD`, `PT_INTERP`, …) |
| Used by | `ld` (the linker), `objdump`, `readelf` | The kernel's `execve()` and the dynamic linker |
| Granularity | Fine — dozens of small named sections | Coarse — a handful of aligned segments |
| Can be stripped? | **Yes** — `strip` removes most of them and the binary still runs | **No** — without these the kernel can't load the file |

A section like `.text` (code) and `.rodata` (read-only data) are usually bundled together into one `PT_LOAD` segment with R+E or R permissions. The linker groups logically-similar sections into segments based on which permissions they need at run time. The loader doesn't care about the section names — it cares about "give me this range of bytes with these rwx bits".

You can run `strip` on a binary and remove every section header — the file still executes, because the kernel reads program headers, not section headers. Try it:

```sh
gcc hello.c -o hello
./hello                    # works
strip hello
./hello                    # still works
readelf -S hello           # → "There are no sections in this file"
readelf -l hello           # → program headers are still there
```

This duality is why you'll see section names mentioned all over the ELF literature (`.text`, `.plt`, `.got.plt`, `.rela.dyn`), but the kernel never touches any of them at `execve` time.

## The overall file layout

A typical ELF executable laid out by a modern linker looks like this (offsets are schematic):

```
+----------------------+  offset 0
|   Elf64_Ehdr         |  the ELF header — magic, arch, type, e_entry,
|                      |  and where to find the other tables.
+----------------------+
|   Elf64_Phdr[]       |  program header table (runtime view), at e_phoff.
|                      |  typically comes immediately after the ELF header.
+----------------------+
|                      |
|   ...file body...    |  code, data, rodata, the .dynamic array, string
|                      |  tables, note records, symbol tables, relocations.
|                      |  Laid out in segments described by the Phdrs above.
|                      |
+----------------------+
|   Elf64_Shdr[]       |  section header table (link-time view), at e_shoff.
|                      |  usually at the very end of the file.
+----------------------+
```

Two things worth noticing:

1. **Program headers come first, section headers come last.** That's because the loader reads the program headers immediately after the ELF header (they're typically at offset 64 on a 64-bit file). The section headers are only needed by tools; they can sit anywhere.
2. **Section and segment contents overlap.** A `.text` section and a `PT_LOAD` segment are *the same bytes in the file* — just viewed under different lenses. There isn't a "copy" for each.

## The ELF header (`Elf64_Ehdr`)

The first 64 bytes of every ELF64 file. Key fields:

| Field | Meaning |
|---|---|
| `e_ident[16]` | Magic (`0x7f 'E' 'L' 'F'`), class (32/64), data order, OS/ABI |
| `e_type` | `ET_REL`, `ET_EXEC`, `ET_DYN`, `ET_CORE` — what kind of file is this |
| `e_machine` | Architecture: `EM_X86_64`, `EM_AARCH64`, `EM_ARM`, … |
| `e_entry` | Virtual address where execution should begin |
| `e_phoff`, `e_phentsize`, `e_phnum` | Program header table: offset, entry size, count |
| `e_shoff`, `e_shentsize`, `e_shnum` | Section header table: same, for sections |
| `e_shstrndx` | Index of the section holding the section-name string table |

A few observations you'll want to internalize:

- **`e_entry` is a virtual address, not a file offset.** To find that address in the mapped process, the kernel maps `PT_LOAD` segments into memory and then jumps to that address in the mapped process.
- **`e_entry` points at `_start`, never at `main`.** `_start` is a tiny assembly stub from the C runtime (`crt1.o`) that eventually calls `__libc_start_main`, which eventually calls `main`. See [03 — The entry point](03-entry-point.md).
- **`e_type == ET_DYN` is ambiguous.** It can mean either "shared library" or "PIE executable". The two are structurally identical — the "is it an executable?" decision is made at link time (whether `PT_INTERP` is present, whether there's a `_start`), not via `e_type`. This is why `file` sometimes says `shared object` for a perfectly good PIE executable.

## Program headers (`Elf64_Phdr`) — the segment view

Each entry describes one segment. The kernel cares about these types:

| Type | What the kernel does with it |
|---|---|
| `PT_LOAD` | `mmap` the `p_filesz` bytes from `p_offset` to `p_vaddr` with rwx bits from `p_flags`. The `p_memsz - p_filesz` slack is zero-filled (this is how `.bss` works). |
| `PT_INTERP` | The string at `p_offset` names the dynamic linker. If present, the kernel loads *that* ELF and jumps to its entry point instead. |
| `PT_DYNAMIC` | Points at the `.dynamic` array; the dynamic linker reads this to figure out what libraries to load, where the symbol table is, etc. |
| `PT_NOTE` | Metadata (e.g. build-id). The kernel mostly ignores it. |
| `PT_TLS` | A template for the thread-local storage block of the initial thread. |
| `PT_GNU_STACK` | A permission marker: whether the stack should be executable. |
| `PT_GNU_RELRO` | Range of memory that should be made read-only *after* relocations are applied — a modern hardening feature. |
| `PT_PHDR` | Self-reference: the program header table's own location, so the runtime knows where it is. |

For a concrete example, here's the program header list for a typical PIE hello-world (output of our `mini-readelf`):

```
Program Headers (13 entries):
  Idx Type          Flags Offset         VirtAddr       …
    0 PHDR          R
    1 INTERP        R
    2 LOAD          R     ← headers + rodata-before-code
    3 LOAD          R E   ← .text (code)
    4 LOAD          R     ← read-only data
    5 LOAD          RW    ← writable data + bss
    6 DYNAMIC       RW
    7 NOTE          R
    8 NOTE          R
    9 GNU_PROPERTY  R
   10 GNU_EH_FRAME  R
   11 GNU_STACK     RW    ← note: not executable
   12 GNU_RELRO     R
```

Notice there are four `PT_LOAD` segments. Modern linkers split them by permissions so that code gets its own `R+E` page, read-only data gets its own `R` page, and writable data gets its own `RW` page. Older layouts used to merge more things together.

## Section headers (`Elf64_Shdr`) — the link-time view

You'll see section names like these in every executable:

| Section | Contains | Ends up in |
|---|---|---|
| `.text` | Machine code | `PT_LOAD` `R+E` |
| `.rodata` | String literals, const arrays | `PT_LOAD` `R` |
| `.data` | Initialized global/static variables | `PT_LOAD` `RW` |
| `.bss` | Uninitialized globals (zero-filled, no bytes on disk) | `PT_LOAD` `RW` (mem > file) |
| `.plt`, `.plt.sec` | The Procedure Linkage Table — trampolines for calls into shared libraries | `PT_LOAD` `R+E` |
| `.got`, `.got.plt` | Global Offset Table — slots the dynamic linker fills in with real addresses | `PT_LOAD` `RW` (but often `RELRO`) |
| `.dynsym` | Dynamic symbol table — imports/exports the loader needs | `PT_LOAD` `R` |
| `.dynstr` | Strings referenced from `.dynsym` | `PT_LOAD` `R` |
| `.rela.dyn`, `.rela.plt` | Relocations the dynamic linker has to apply | `PT_LOAD` `R` |
| `.dynamic` | The `.dynamic` array — same bytes as `PT_DYNAMIC` | `PT_LOAD` `RW` |
| `.init_array`, `.fini_array` | Arrays of function pointers to run before/after `main` | `PT_LOAD` `RW` |
| `.eh_frame`, `.eh_frame_hdr` | Unwind info for exception handling and stack traces | `PT_LOAD` `R`, plus `PT_GNU_EH_FRAME` |

The key point: **sections are named, fine-grained, and purely descriptive**. The linker gathers them into segments at the end based on permissions. After that, the names don't matter for running the program — only for tooling.

## Why there are two views in the first place

Think of it this way:

- **The linker** needs to merge `.o` files. It needs to know "here's some code from `foo.o`, here's some code from `bar.o`, concatenate them into `.text`". That requires many small, well-labeled units — *sections*.
- **The loader** doesn't care about any of that. It just needs "map these bytes at this address with these rights". That's much coarser — *segments*.

Same bytes on disk, two parallel indexes. Once you grok this, ELF stops feeling weird.

## Try it yourself

Make sure you've built the mini-readelf tool (`make` in the step directory) and the examples, then:

```sh
./src/mini-readelf examples/hello-dynamic    # PIE executable
./src/mini-readelf examples/hello-static     # classic static executable
./src/mini-readelf /bin/ls                    # a real program
./src/mini-readelf /lib/x86_64-linux-gnu/libc.so.6   # a shared library
```

Compare the `e_type`, the presence of `PT_INTERP`, and the `DT_NEEDED` list. Those three differences tell you 80% of what static vs dynamic linking means on Linux.

## Further reading

- `man elf(5)` — authoritative reference for every struct and constant
- System V Application Binary Interface — AMD64 supplement, Chapter 4 and 5 ("Object Files" and "Program Loading and Dynamic Linking")
- Eli Bendersky, "Position Independent Code (PIC) in shared libraries" and "Load-time relocation of shared libraries"
- Linux kernel source: `fs/binfmt_elf.c` — the kernel's ELF loader in ~2000 heavily commented lines
