# 06 — C language and POSIX deep dive

> Docs `01`–`05` explain what ELF *is*. This doc explains the **C language and POSIX features** that show up in `mini-readelf.c` in enough depth that you can read the code without getting stuck on any non-ELF thing.
>
> Target reader: a senior web developer with rusty C. You know `if`/`for`/functions/basic `printf`. You've heard of pointers. You may not be fluent in mmap, unions, bit packing, or the preprocessor.
>
> This doc is designed to be **worked through, not just read**. Almost every section ends with a small lab you can compile and run. The investment pays off: every skill here reappears in steps 2 and 3 of the project.

## Setup — a scratch directory for labs

Before you start, make a throwaway directory and compile-check that your toolchain works:

```sh
mkdir -p /tmp/c-lab && cd /tmp/c-lab
cat > smoke.c <<'EOF'
#include <stdio.h>
int main(void) { printf("hello from /tmp/c-lab\n"); return 0; }
EOF
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic smoke.c -o smoke && ./smoke
```

You should see:

```
hello from /tmp/c-lab
```

From here on, whenever I say "**Lab**:", paste the snippet into a new file in `/tmp/c-lab`, compile it with the same `gcc` line, and run it. The labs are the whole point — **reading C is not the same skill as writing and running it**.

## Reading order

Work top-to-bottom. Later sections depend on earlier ones.

1. [Types, sizes, and memory layout](#1-types-sizes-and-memory-layout)
2. [Pointers and memory](#2-pointers-and-memory)
3. [Structs, casts, and alignment](#3-structs-casts-and-alignment)
4. [Unions — two names for the same bytes](#4-unions--two-names-for-the-same-bytes)
5. [Bitwise operators and bit packing](#5-bitwise-operators-and-bit-packing)
6. [Integer types and the `printf` format language](#6-integer-types-and-the-printf-format-language)
7. [The preprocessor in depth](#7-the-preprocessor-in-depth)
8. [Storage classes: `const`, `static`, `extern`](#8-storage-classes-const-static-extern)
9. [Operators and control flow you'll see in the code](#9-operators-and-control-flow-youll-see-in-the-code)
10. [POSIX file I/O: `open`, `fstat`, `close`, `perror`](#10-posix-file-io-open-fstat-close-perror)
11. [`mmap` — deep dive](#11-mmap--deep-dive)
12. [C strings and string tables](#12-c-strings-and-string-tables)
13. [Common pitfalls and undefined behavior](#13-common-pitfalls-and-undefined-behavior)
14. [Putting it all together — re-read `main()`](#14-putting-it-all-together--re-read-main)
15. [Lab index — all the labs in one place](#15-lab-index)

---

## 1. Types, sizes, and memory layout

### What you'll see

```c
#include <stdint.h>
uint32_t st_name;
uint64_t e_entry;
size_t size = sizeof(Elf64_Ehdr);
off_t filesize = st.st_size;
```

### Why this section goes first

Everything in C is ultimately "a chunk of memory with a type label on it". Before you can understand pointers or structs, you need to know how big each type is, what "memory layout" means for a process, and why C has so many integer types.

### Primitive types (the old ones)

C's original types don't specify exact sizes:

| Type | Typical size (64-bit Linux) | Range (signed) |
|---|---|---|
| `char` | 1 byte | -128..127 |
| `short` | 2 bytes | -32768..32767 |
| `int` | 4 bytes | ~±2 billion |
| `long` | 8 bytes | ~±9 quintillion |
| `long long` | 8 bytes | same |

There are also `unsigned` variants: `unsigned char`, `unsigned int`, etc., which double the positive range at the cost of no negatives.

**The sizes are not guaranteed.** The C standard only says "`int` is at least 16 bits" and "`long` is at least 32 bits". On 64-bit Windows, `long` is only 4 bytes. On some embedded systems, `int` is only 2 bytes. This matters when you're parsing binary formats where a field must be *exactly* 4 bytes or *exactly* 8 bytes — which is every binary format on Earth.

### Fixed-width types (the ones you should actually use)

`<stdint.h>` defines types with **guaranteed** sizes:

| Type | Exact size | Example use |
|---|---|---|
| `int8_t`, `uint8_t` | 1 byte | A single byte, like a flag or a char |
| `int16_t`, `uint16_t` | 2 bytes | ELF's `e_type`, `e_machine`, `e_phnum` |
| `int32_t`, `uint32_t` | 4 bytes | ELF's `e_flags`, symbol `st_name` |
| `int64_t`, `uint64_t` | 8 bytes | ELF's `e_entry`, `p_offset`, `p_vaddr` |

ELF's own typedefs (`Elf64_Half`, `Elf64_Word`, `Elf64_Off`, `Elf64_Addr`) are defined in `<elf.h>` in terms of these. For instance, `Elf64_Half` is `uint16_t`, always 2 bytes, period.

**Rule of thumb for systems code**: if a variable represents something with a fixed on-wire or on-disk size, use `uintN_t`. If it's a count or index with no external constraint, `size_t` is fine.

### `size_t`, `off_t`, `ptrdiff_t`

Three special types:

- **`size_t`** — an unsigned integer big enough to hold the size of any object in memory. On 64-bit Linux it's 8 bytes; on 32-bit systems it's 4. Produced by `sizeof`, accepted by `malloc`, `memcpy`, `mmap`'s `length`, etc.
- **`off_t`** — a *signed* integer big enough to hold a file offset. On 64-bit Linux it's 8 bytes. Used for `lseek` offsets and `stat.st_size`.
- **`ptrdiff_t`** — a signed integer big enough to hold the difference between two pointers. Rarely used directly.

You'll see `(off_t)sizeof(Elf64_Ehdr)` in `mini-readelf.c` — we cast the `size_t` result of `sizeof` to `off_t` so we can compare it to `st.st_size` without a signed/unsigned warning.

### The `sizeof` operator

`sizeof` is a **compile-time** operator that returns the size in bytes of a type or expression. It's not a function; the compiler computes the value while compiling.

```c
sizeof(char)       // 1 (always, everywhere)
sizeof(int)        // 4 (typical)
sizeof(uint64_t)   // 8 (always)
sizeof(Elf64_Ehdr) // 64 (on any x86-64 Linux)

int x = 5;
sizeof(x)          // 4 (or whatever sizeof(int) is)
sizeof x           // parentheses optional for expressions
```

**When dividing to get a count**, the idiom is `sizeof(array) / sizeof(array[0])`:

```c
int arr[] = {1, 2, 3, 4};
size_t n = sizeof(arr) / sizeof(arr[0]);  // = 4
```

In `mini-readelf.c`:

```c
int max_entries = pt_dynamic->p_filesz / sizeof(Elf64_Dyn);
```

"How many `Elf64_Dyn` entries fit in `p_filesz` bytes?" = `p_filesz / 16`.

### Memory layout of a process

When your program runs, its memory is divided into regions (segments). From low addresses to high:

```
0x0000000000000000
       |
       | (nothing — the kernel reserves address 0 so NULL dereferences crash)
       |
  .text       ← your code (read + execute, never written to)
  .rodata     ← constants and string literals (read-only)
  .data       ← initialized globals + static vars (read + write)
  .bss        ← zero-initialized globals + static vars (read + write)
       |
  heap ↓      ← grows up from there (malloc lives here)
       |
  mmap region ← shared libraries, mmap()'d files, etc.
       |
  stack ↑     ← grows down from the top (local variables, function frames)
       |
  kernel space (hidden from userspace)
0xffffffffffffffff
```

The specific addresses are randomized by ASLR (see `04-pie-and-aslr.md`), but the *layout* is stable.

**Where your variables live**:

- **Local variables** (`int x;` inside a function) — on the stack. Freed automatically when the function returns.
- **`malloc`'d memory** — on the heap. You must `free` it.
- **Global variables** with an initializer — in `.data`.
- **Global variables** without an initializer — in `.bss`.
- **String literals** (`"hello"`) — in `.rodata`. Any `char *` pointing at a string literal points into `.rodata` and you must not write through it.
- **Code** (functions) — in `.text`.

`mini-readelf` lives almost entirely on the stack (all its variables are local) plus one huge `mmap` region (the file being parsed). It calls `malloc` zero times. That's why you don't see any `free` calls.

### Lab 1: size and address of everything

```c
// sizes.c
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <elf.h>

int global_var = 42;         // .data
int zero_global;             // .bss
const char *literal = "hi";  // the pointer is on stack; "hi" is in .rodata

int main(void) {
    int local = 5;           // stack

    printf("sizeof char       = %zu\n", sizeof(char));
    printf("sizeof short      = %zu\n", sizeof(short));
    printf("sizeof int        = %zu\n", sizeof(int));
    printf("sizeof long       = %zu\n", sizeof(long));
    printf("sizeof long long  = %zu\n", sizeof(long long));
    printf("sizeof void *     = %zu\n", sizeof(void *));
    printf("sizeof size_t     = %zu\n", sizeof(size_t));
    printf("sizeof off_t      = %zu\n", sizeof(off_t));
    printf("sizeof uint64_t   = %zu\n", sizeof(uint64_t));
    printf("sizeof Elf64_Ehdr = %zu\n", sizeof(Elf64_Ehdr));
    printf("sizeof Elf64_Phdr = %zu\n", sizeof(Elf64_Phdr));
    printf("sizeof Elf64_Dyn  = %zu\n", sizeof(Elf64_Dyn));
    printf("sizeof Elf64_Sym  = %zu\n", sizeof(Elf64_Sym));

    printf("\naddresses:\n");
    printf("  &local       = %p  (stack)\n",   (void *)&local);
    printf("  &global_var  = %p  (.data)\n",   (void *)&global_var);
    printf("  &zero_global = %p  (.bss)\n",    (void *)&zero_global);
    printf("  literal      = %p  (.rodata)\n", (void *)literal);
    printf("  main         = %p  (.text)\n",   (void *)main);
    return 0;
}
```

Compile and run. You should see something like:

```
sizeof char       = 1
sizeof short      = 2
sizeof int        = 4
sizeof long       = 8
...
sizeof Elf64_Ehdr = 64
sizeof Elf64_Phdr = 56
sizeof Elf64_Dyn  = 16
sizeof Elf64_Sym  = 24

addresses:
  &local       = 0x7ffd...    (stack — very high address)
  &global_var  = 0x55d5...    (.data — in executable's data segment)
  &zero_global = 0x55d5...    (.bss — next to .data)
  literal      = 0x55d5...    (.rodata — back in the executable)
  main         = 0x55d5...    (.text — the code segment)
```

Notice how `&local` is in a totally different address range (high) from everything else (low). That's the stack being near the top of the address space while everything else lives near the bottom. The others clump together because they're all in the same mapped binary.

### Key takeaway

> Use fixed-width integer types (`uint64_t`, etc.) for anything with a defined size. `sizeof` is a compile-time operator, not a function. Your program's memory is divided into stack, heap, and data/code segments, each serving a different purpose.

---

## 2. Pointers and memory

### What you'll see

```c
const unsigned char *base = map;
if (base[EI_MAG0] != ELFMAG0 || ...)
const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
```

### Concept: a pointer is an address

In JS, variables hold *references* to objects managed by the runtime. In C, you can hold the raw memory address of any variable. That's all a pointer is: **a variable whose value is an address**.

```c
int x = 42;
int *p = &x;   // p holds the address of x
```

- `&x` — "the address of `x`"
- `*p` — "the thing `p` points at" (dereference)
- The declaration `int *p` reads as "`*p` is an int", i.e. "`p` is a pointer to an int"

The `int` in `int *p` isn't decoration — it tells the compiler how to interpret the bytes at that address (4 bytes, signed, in the platform's native byte order). Change it to `char *p` and `*p` is one byte.

### Lab 2: address and dereference

```c
// ptrs.c
#include <stdio.h>
int main(void) {
    int x = 42;
    int *p = &x;

    printf("x   = %d\n",   x);
    printf("&x  = %p\n",   (void *)&x);
    printf("p   = %p\n",   (void *)p);
    printf("*p  = %d\n",   *p);

    *p = 99;                // write through the pointer
    printf("x after *p=99: %d\n", x);
    return 0;
}
```

Output:

```
x   = 42
&x  = 0x7ffd...
p   = 0x7ffd...   ← same address
*p  = 42
x after *p=99: 99  ← modifying *p modified x
```

`p` and `&x` are the same value. Writing `*p = 99` is literally writing 99 into the memory at address `&x`, which *is* `x`.

### Concept: pointer arithmetic is typed

If `p` is an `int *`, then `p + 1` doesn't mean "address + 1 byte". It means "address + 1 int's worth of bytes" — the next `int` in an array. The compiler automatically scales by `sizeof(*p)`:

```c
int arr[] = {10, 20, 30};
int *p = arr;
printf("%d\n", *p);        // 10
printf("%d\n", *(p + 1));  // 20 — address was bumped by sizeof(int) = 4
printf("%d\n", p[1]);      // 20 — identical to *(p + 1)
```

**This is why the pointer type matters for byte-precise work**. If you have a byte buffer and want `buffer + 5` to mean "5 bytes forward", you must use a 1-byte pointer type: `char *` or (for binary data) `unsigned char *`.

In `mini-readelf.c`:

```c
const unsigned char *base = map;
...
ph = (const Elf64_Phdr *)(base + eh->e_phoff);
```

Because `base` is `unsigned char *`, `base + eh->e_phoff` moves forward by exactly `eh->e_phoff` bytes — regardless of what follows. Then we cast the resulting byte pointer to `const Elf64_Phdr *`. From that moment, `ph[0]`, `ph[1]`, ... are struct entries in the program header table.

### `char *` vs `unsigned char *` vs `void *`

- **`char *`** — used for ASCII strings. Signed vs unsigned `char` is implementation-defined, so relying on values beyond 0-127 is fragile.
- **`unsigned char *`** — used for raw bytes. Always unsigned 0-255. This is what you want for binary data.
- **`void *`** — a generic pointer. You can't dereference or index it directly; you must cast it to a real type first. `mmap` and `malloc` return `void *` because they don't know what you'll store there.

### NULL and null checks

`NULL` is a macro (defined as `((void *)0)`) that represents "a pointer to nothing". Dereferencing `NULL` causes a segfault (SIGSEGV), because address 0 is deliberately unmapped to catch this bug.

```c
int *p = NULL;
if (p) {           // false, because p is NULL
    *p = 5;        // would crash
}
```

`if (p)` is idiomatic for "if `p` is not NULL". The same pattern applies to function return values that might fail:

```c
const char *result = vaddr_to_filebuf(base, ph, n, vaddr);
if (result) {
    // use it
} else {
    // fall back
}
```

### Lab 3: pointer arithmetic

```c
// arith.c
#include <stdio.h>
#include <stdint.h>
int main(void) {
    uint8_t buf[] = {0x7f, 'E', 'L', 'F', 0x02, 0x01, 0x01, 0x00};

    uint8_t *p = buf;
    printf("p[0] = 0x%02x  (\\x7f, ELF magic)\n",    p[0]);
    printf("p[1] = '%c'    (E)\n",                    p[1]);
    printf("p[2] = '%c'    (L)\n",                    p[2]);
    printf("p[3] = '%c'    (F)\n",                    p[3]);
    printf("p[4] = 0x%02x  (class: 1=ELF32, 2=ELF64)\n", p[4]);
    printf("p[5] = 0x%02x  (data: 1=LE, 2=BE)\n",     p[5]);

    // Byte-precise pointer arithmetic:
    uint8_t *after_magic = p + 4;
    printf("\nafter_magic[0] = 0x%02x  (should match p[4])\n", after_magic[0]);
    return 0;
}
```

Run it; verify `after_magic[0]` equals `p[4]`. That's the same trick `mini-readelf.c` uses on the mmapped file.

### Key takeaway

> A pointer is an address. `*p` reads the bytes there, typed. `p + 1` advances by `sizeof(*p)`. Use `unsigned char *` for byte-precise work on raw binary data.

---

## 3. Structs, casts, and alignment

### What you'll see

```c
const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
printf("Entry: 0x%lx\n", eh->e_entry);
```

### Concept: structs are sequential in memory

A `struct` is a bundle of fields. In memory, the fields sit **one after another**, in declaration order, each at its natural alignment:

```c
struct Point {
    int32_t  x;        // offset 0, size 4
    int32_t  y;        // offset 4, size 4
    int8_t   tag;      // offset 8, size 1
    /* padding */      // offset 9-15: 7 bytes of padding
    int64_t  value;    // offset 16, size 8
};
```

Total size: 24 bytes (not 21). We'll get to padding in a moment.

`p->x` is at offset 0 from the struct address. `p->value` is at offset 16. The compiler computes these offsets at compile time and turns `p->value` into "memory access at `p + 16`".

### `->` vs `.`

```c
struct Point p_val;     // a struct value
struct Point *p_ptr;    // a pointer to a struct

p_val.x = 1;            // .  when you have a value
p_ptr->x = 1;           // -> when you have a pointer
(*p_ptr).x = 1;         // equivalent, but ugly
```

In real C code, structs are almost always passed and stored by pointer, so `->` is far more common than `.`.

### Concept: struct size and padding

CPUs read memory most efficiently when data is **aligned**: a 4-byte int should live at an address divisible by 4, an 8-byte value should live at an address divisible by 8, etc. Reading an 8-byte int from an unaligned address is either slow or (on some CPUs) impossible.

So the compiler inserts **padding** between struct fields to keep each field aligned:

```c
struct A {
    char  c;     // offset 0, size 1
    /*pad*/      // offset 1, 3 bytes of padding (so int starts at offset 4)
    int   i;     // offset 4, size 4
};
// sizeof(struct A) == 8
```

Without padding, `i` would sit at offset 1 — unaligned. So the compiler skips 3 bytes. The final struct is 8 bytes, not 5.

Rules of thumb:
- Each field is aligned to at least `sizeof(that field)`.
- The whole struct is aligned to its **strictest-aligned** field (so arrays of structs are also well-aligned).
- Reordering fields from largest to smallest often eliminates padding.

### Why ELF structs don't need explicit packing

`Elf64_Ehdr`, `Elf64_Phdr`, `Elf64_Dyn`, `Elf64_Sym` — none of these use `#pragma pack` or `__attribute__((packed))`. They don't need to, because the ELF spec designed each struct's field order such that natural alignment gives the right on-disk layout with zero padding:

```c
typedef struct {
    unsigned char e_ident[16];  // 0..15,  16 bytes
    Elf64_Half    e_type;       // 16..17, 2 bytes
    Elf64_Half    e_machine;    // 18..19, 2 bytes
    Elf64_Word    e_version;    // 20..23, 4 bytes
    Elf64_Addr    e_entry;      // 24..31, 8 bytes
    Elf64_Off     e_phoff;      // 32..39, 8 bytes
    Elf64_Off     e_shoff;      // 40..47, 8 bytes
    Elf64_Word    e_flags;      // 48..51, 4 bytes
    Elf64_Half    e_ehsize;     // 52..53, 2 bytes
    Elf64_Half    e_phentsize;  // 54..55, 2 bytes
    Elf64_Half    e_phnum;      // 56..57, 2 bytes
    Elf64_Half    e_shentsize;  // 58..59, 2 bytes
    Elf64_Half    e_shnum;      // 60..61, 2 bytes
    Elf64_Half    e_shstrndx;   // 62..63, 2 bytes
} Elf64_Ehdr;
// sizeof == 64, no padding
```

Every field lands at an offset that's a multiple of its size. This is a **design choice** of the ELF spec, and it's why the struct-on-mmap trick works so cleanly.

### The `offsetof` macro

`<stddef.h>` provides `offsetof(type, member)` which returns the byte offset of a member within a struct at compile time. Useful for verification:

```c
#include <stddef.h>
#include <elf.h>
printf("%zu\n", offsetof(Elf64_Ehdr, e_entry));  // 24
```

### Concept: reinterpreting bytes as a struct — the CORE trick

Here is the single most important C idiom in this whole project:

```c
void *map = mmap(NULL, filesize, ...);          // raw bytes from file
const Elf64_Ehdr *eh = (const Elf64_Ehdr *)map; // "pretend these bytes are a struct"
printf("%lx\n", eh->e_entry);                   // read field 24 bytes in
```

**No copying. No parsing. Zero runtime cost for the cast itself.**

What happens mechanically:
1. `mmap` gives us a pointer to the file bytes in memory.
2. The cast `(const Elf64_Ehdr *)map` relabels that pointer's type. The bytes are unchanged.
3. `eh->e_entry` becomes a load instruction at `map + 24` of 8 bytes, interpreted as `uint64_t`.

It works because:
- **Endianness matches**: the file is little-endian, our CPU is little-endian, so multi-byte fields line up correctly.
- **The C struct layout matches the file format exactly**: same field order, same sizes, same padding.
- **`mmap`'d pages are aligned**: `mmap` returns page-aligned addresses (typically multiples of 4096), so `eh` is sufficiently aligned for any field.

This trick is the reason `mini-readelf.c` is only ~540 lines instead of ~2000. Without it, you'd have to write code that reads each field byte-by-byte and assembles the result.

### Lab 4: cast bytes to a struct and watch it work

```c
// castbytes.c
#include <stdio.h>
#include <stdint.h>

typedef struct {
    uint32_t magic;    // 4 bytes
    uint16_t version;  // 2 bytes
    uint16_t flags;    // 2 bytes
    uint64_t entry;    // 8 bytes
} MyHeader;  // sizeof == 16

int main(void) {
    // Construct 16 raw bytes representing a MyHeader (little-endian):
    uint8_t raw[] = {
        0xef, 0xbe, 0xad, 0xde,                         // magic  = 0xdeadbeef
        0x01, 0x00,                                      // version = 1
        0x02, 0x00,                                      // flags  = 2
        0x78, 0x56, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, // entry = 0x12345678
    };

    MyHeader *h = (MyHeader *)raw;
    printf("magic   = 0x%08x\n", h->magic);
    printf("version = %u\n",      h->version);
    printf("flags   = %u\n",      h->flags);
    printf("entry   = 0x%lx\n",   h->entry);
    printf("sizeof(MyHeader) = %zu\n", sizeof(MyHeader));
    return 0;
}
```

Output:

```
magic   = 0xdeadbeef
version = 1
flags   = 2
entry   = 0x12345678
sizeof(MyHeader) = 16
```

You just did what `mini-readelf.c` does to `Elf64_Ehdr`. Same trick, smaller struct.

### Aside: strict aliasing (you can skim)

The C standard formally restricts casting between unrelated pointer types ("strict aliasing"). In full-paranoia mode, the cast we just did could be considered non-portable C.

In practice: gcc/clang carve out a specific exception for `char *` / `unsigned char *`-based access, which is why the idiom works on every real compiler. ELF parsers in real codebases (binutils, elfutils, LLVM) use this trick. Don't lose sleep over it — you'd only hit a problem with exotic optimization settings on exotic compilers.

If you ever need to be bulletproof, use `memcpy` into a local struct: `memcpy(&eh, base, sizeof(eh))`. The compiler optimizes this to zero extra work in most cases.

### Key takeaway

> A struct's bytes sit sequentially in memory with natural alignment. Casting a byte pointer to a struct pointer gives you field-level access for free. This trick works because ELF was deliberately designed so that natural C struct layout matches the on-disk format.

---

## 4. Unions — two names for the same bytes

### What you'll see

```c
typedef struct {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr  d_ptr;
    } d_un;
} Elf64_Dyn;

// ... in main():
if (d[i].d_tag == DT_STRTAB) strtab_vaddr = d[i].d_un.d_ptr;
if (d[i].d_tag == DT_STRSZ)  strtab_size  = d[i].d_un.d_val;
```

### Concept

A **union** is a struct-like thing where **every member occupies the same memory**. The union's size is the size of its largest member.

```c
union IntOrFloat {
    uint32_t as_int;    // 4 bytes
    float    as_float;  // 4 bytes
};
// sizeof(union IntOrFloat) == 4, not 8
```

Writing `u.as_float = 3.14f` stores 4 bytes. Reading `u.as_int` reads *those same 4 bytes* but interprets them as an integer. This is called **type punning**: the bytes don't change; you just relabel what they mean.

### Lab 5: type punning with a union

```c
// union.c
#include <stdio.h>
#include <stdint.h>

int main(void) {
    union {
        uint32_t as_int;
        float    as_float;
    } u;

    u.as_float = 3.14f;
    printf("3.14 as an unsigned int: 0x%08x\n", u.as_int);

    u.as_int = 0x40490fdb;   // bit pattern of pi in IEEE 754
    printf("0x40490fdb as a float:  %f\n", u.as_float);
    return 0;
}
```

Output:

```
3.14 as an unsigned int: 0x4048f5c3
0x40490fdb as a float:  3.141593
```

Two perspectives on the same 4 bytes. This is exactly how debuggers show you float values alongside their bit patterns.

### Why ELF uses a union for `d_un`

The `.dynamic` array has entries that look like `[tag][value]`. The value is 8 bytes, but its **meaning** depends on the tag:

- `DT_NEEDED = 1` → value is a string table offset (integer)
- `DT_STRTAB = 5` → value is a virtual address (pointer)
- `DT_STRSZ = 10` → value is a size (integer)
- `DT_INIT = 12` → value is a function address (pointer)

The ELF designers could have made `d_un` a plain `uint64_t`. Functionally identical. But they chose a union so that **code expressing intent reads better**:

```c
d[i].d_un.d_val   // "I'm treating this as a count/size/offset integer"
d[i].d_un.d_ptr   // "I'm treating this as a virtual address"
```

Both read the same 8 bytes. The difference is purely documentation for the person reading the code.

### Unions as tagged data (optional but useful)

A common pattern: pair a union with a discriminator (a "tag") to represent a value that can be one of several types. This is a "tagged union" or "sum type":

```c
enum ValueType { V_INT, V_FLOAT, V_STRING };
struct Value {
    enum ValueType type;
    union {
        int    as_int;
        float  as_float;
        char  *as_string;
    } data;
};
```

Use:

```c
struct Value v;
v.type = V_INT;
v.data.as_int = 42;

if (v.type == V_INT) {
    printf("it's an int: %d\n", v.data.as_int);
}
```

`Elf64_Dyn` is exactly this pattern. `d_tag` is the discriminator; `d_un` is the union of possible value types. The tag tells you which union member is active.

Langages like Rust and OCaml make tagged unions first-class (`enum`, `variant`). C makes you build them by hand.

### Key takeaway

> Unions let one memory slot have multiple type interpretations. `d_un.d_val` and `d_un.d_ptr` are the same 8 bytes. The union documents which interpretation the code is currently using.

---

## 5. Bitwise operators and bit packing

### What you'll see

```c
#define ELF64_ST_BIND(info)  ((info) >> 4)
#define ELF64_ST_TYPE(info)  ((info) & 0xf)

static void print_pflags(uint32_t flags) {
    putchar(flags & PF_R ? 'R' : ' ');
    putchar(flags & PF_W ? 'W' : ' ');
    putchar(flags & PF_X ? 'E' : ' ');
}
```

### The six bitwise operators

C has six operators that manipulate bits directly:

| Op | Name | Example | Result |
|---|---|---|---|
| `&` | AND | `0b1100 & 0b1010` | `0b1000` |
| `\|` | OR | `0b1100 \| 0b1010` | `0b1110` |
| `^` | XOR | `0b1100 ^ 0b1010` | `0b0110` |
| `~` | NOT | `~0b00001111` (uint8) | `0b11110000` |
| `<<` | shift left | `0b0011 << 2` | `0b1100` |
| `>>` | shift right | `0b1100 >> 2` | `0b0011` |

Don't confuse them with logical operators:

- `&&` — logical AND (returns 0 or 1 based on truthiness)
- `&` — bitwise AND (computes bit by bit)
- `||` — logical OR
- `|` — bitwise OR
- `!` — logical NOT
- `~` — bitwise NOT

Mistaking `&` for `&&` (or vice versa) is a classic C bug.

### The three canonical bit operations

Almost every bit operation boils down to one of these patterns:

**Test a bit** — is bit N set?

```c
if (flags & MASK)   // non-zero if any bit in MASK is also set in flags
```

**Set a bit** — turn on the bits in MASK:

```c
flags |= MASK
```

**Clear a bit** — turn off the bits in MASK:

```c
flags &= ~MASK
```

**Toggle a bit**:

```c
flags ^= MASK
```

### Shifting

`<<` and `>>` slide bits left or right by N positions. Empty slots are filled with zeros:

```c
0b00000001 << 3 == 0b00001000  // 1 << 3 == 8
0b10100000 >> 4 == 0b00001010  // top nibble → bottom nibble
```

Left-shifting by N is equivalent to multiplying by 2^N. Right-shifting by N is dividing by 2^N (for unsigned values — for signed, it's subtler).

### Reading packed fields

The `Elf64_Sym.st_info` field is **one byte containing two 4-bit fields**:

```
bit:   7  6  5  4   3  2  1  0
       |_________|  |_________|
         BIND (4 bits)  TYPE (4 bits)
         top nibble     bottom nibble
```

To extract the top nibble (BIND), shift right by 4:

```c
bind = info >> 4;
```

This moves bits 4-7 down to positions 0-3 and fills the top with zeros.

To extract the bottom nibble (TYPE), AND with `0x0f`:

```c
type = info & 0x0f;
```

`0x0f` is `0b00001111` — a mask that keeps only the bottom four bits.

Concrete example: `st_info = 0x12` (binary `0001 0010`).

- `0x12 >> 4 == 0x01` → STB_GLOBAL
- `0x12 & 0xf == 0x02` → STT_FUNC

So a symbol with `st_info = 0x12` is a **global function** — like `puts`.

### Reading flag bits

The program header `p_flags` is a 32-bit integer where each bit means a permission:

```c
#define PF_X 0x1  // 0b001 — executable
#define PF_W 0x2  // 0b010 — writable
#define PF_R 0x4  // 0b100 — readable
```

A segment with R+X has `p_flags = PF_R | PF_X = 0x4 | 0x1 = 0x5`.

To test a bit:

```c
if (flags & PF_R) { /* readable */ }
if (flags & PF_W) { /* writable */ }
if (flags & PF_X) { /* executable */ }
```

Note that `flags & PF_R` is not 0 or 1 — it's either 0 or PF_R itself. The `if` just treats zero as false. If you need a clean 0/1, write `(flags & PF_R) != 0` or `!!(flags & PF_R)`.

### Building masks with shifts

You often construct masks by shifting `1` into place:

```c
#define BIT(n)  (1u << (n))
BIT(0) == 0b00000001
BIT(3) == 0b00001000
BIT(7) == 0b10000000
```

Useful for defining per-bit flags without writing out the hex values.

### Lab 6: pack and unpack `st_info`

```c
// bits.c
#include <stdio.h>
#include <stdint.h>

#define BIND_LOCAL  0
#define BIND_GLOBAL 1
#define BIND_WEAK   2

#define TYPE_NOTYPE 0
#define TYPE_OBJECT 1
#define TYPE_FUNC   2

#define PACK(bind, type)    (((bind) << 4) | ((type) & 0xf))
#define UNPACK_BIND(info)   ((info) >> 4)
#define UNPACK_TYPE(info)   ((info) & 0xf)

int main(void) {
    uint8_t info = PACK(BIND_GLOBAL, TYPE_FUNC);
    printf("packed: 0x%02x\n", info);
    printf("bind:   %u\n", UNPACK_BIND(info));
    printf("type:   %u\n", UNPACK_TYPE(info));

    // Iterate all 16 combinations:
    printf("\nall combinations:\n");
    for (int b = 0; b < 3; b++) {
        for (int t = 0; t < 3; t++) {
            uint8_t x = PACK(b, t);
            printf("  bind=%d type=%d packed=0x%02x\n", b, t, x);
        }
    }
    return 0;
}
```

### Key takeaway

> `&` tests or masks bits. `|` sets. `>>` extracts upper bits. Binary formats pack small fields into bytes to save space; bitwise operators take them apart. Don't confuse `&` with `&&`.

---

## 6. Integer types and the `printf` format language

### What you'll see

```c
printf("  Idx %3d %-13s ", i, p_type_str(ph[i].p_type));
printf(" 0x%012" PRIx64 " 0x%012" PRIx64 " ...\n", ph[i].p_offset, ph[i].p_vaddr, ...);
printf("  Entry point address:   0x%" PRIx64 "\n", eh->e_entry);
```

### The `printf` format string

`printf` parses the format string looking for `%` followed by a **conversion specifier**. The general form is:

```
%[flags][width][.precision][length]specifier
```

Examples:

| Format | Meaning |
|---|---|
| `%d` | signed int as decimal |
| `%u` | unsigned int as decimal |
| `%x` | unsigned int as hex (lowercase) |
| `%X` | unsigned int as hex (uppercase) |
| `%o` | unsigned int as octal |
| `%c` | single character |
| `%s` | NUL-terminated string |
| `%p` | pointer, printed as `0x...` |
| `%f` | float/double |
| `%zu` | `size_t` as decimal (the `z` length modifier) |
| `%%` | a literal `%` |

### Width and padding

The number between `%` and the specifier is the **minimum width**:

```c
printf("|%5d|\n",  42);    // |   42|   (right-aligned, padded to width 5)
printf("|%-5d|\n", 42);    // |42   |   (left-aligned with -)
printf("|%05d|\n", 42);    // |00042|   (zero-padded with 0)
```

For strings:

```c
printf("|%10s|\n",  "hi");  // |        hi|
printf("|%-10s|\n", "hi");  // |hi        |
```

You'll see these in `mini-readelf.c`'s table-formatted output:

```c
printf("  %3d %-13s ", i, p_type_str(ph[i].p_type));
//       ^^^  ^^^^^
//       3-wide  13-wide, left-aligned
```

The index gets 3 columns; the type name gets 13 columns left-aligned. That's how we get table columns that line up.

### Printing hex with leading zeros

`%012lx` means "hex, lowercase, 12 digits wide, zero-padded":

```c
printf("%012lx\n", 0x1234UL);  // 000000001234
```

Used throughout `mini-readelf.c` to keep hex columns aligned.

### The length modifier — why 64-bit numbers need `PRIx64`

Before `<inttypes.h>`, you had to spell out the length explicitly:

```c
printf("%d\n",   (int)x);          // int
printf("%ld\n",  (long)x);         // long (8 bytes on Linux x86-64)
printf("%lld\n", (long long)x);    // long long (8 bytes)
printf("%zu\n",  sizeof(x));       // size_t
```

The problem: `uint64_t` is typedef'd differently across platforms. On Linux x86-64 it's `unsigned long` (so `%lx` works), but on Windows it's `unsigned long long` (so `%llx`). If you write `%lx` explicitly, your code breaks on Windows.

Solution: `<inttypes.h>` defines **format macros** that expand to the correct letter for the platform:

| Macro | Expands to (Linux x86-64) |
|---|---|
| `PRId64` | `"ld"` |
| `PRIu64` | `"lu"` |
| `PRIx64` | `"lx"` |
| `PRIX64` | `"lX"` |
| `PRIo64` | `"lo"` |

And C's **adjacent string literal concatenation** at compile time makes the result clean:

```c
printf("0x%" PRIx64 "\n", eh->e_entry);
// becomes:
printf("0x%lx\n", eh->e_entry);  // on this platform
```

That's why the format strings look ugly. You're stapling three string literals together at compile time: `"0x%"`, then `"lx"` (from the macro expansion), then `"\n"`.

### `fprintf`, `putchar`, `puts`

`printf` writes to stdout (`stdout`, file descriptor 1). For other destinations you use:

- **`fprintf(stream, fmt, ...)`** — write to a specific `FILE *`. Use `fprintf(stderr, ...)` for errors.
- **`putchar(c)`** — write one character to stdout. Used in `print_pflags`.
- **`puts(s)`** — write a string plus a newline. Faster than `printf("%s\n", s)`.

The distinction between stdout and stderr matters: when you pipe the output, stdout goes through the pipe but stderr goes to the terminal. That's why `mini-readelf.c` uses `fprintf(stderr, ...)` for errors — so redirecting `./mini-readelf foo > out.txt` still shows errors on screen.

### Lab 7: printf format playground

```c
// printfs.c
#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>

int main(void) {
    printf("decimal:         %d\n",     42);
    printf("hex:             0x%x\n",   0xdeadbeef);
    printf("hex padded:      0x%08x\n", 0x42);
    printf("string:          [%s]\n",   "hi");
    printf("right-aligned:   [%10s]\n", "hi");
    printf("left-aligned:    [%-10s]\n","hi");
    printf("zero-padded:     [%05d]\n", 42);

    uint64_t big = 0x123456789abcdef0ULL;
    printf("uint64 hex:      0x%" PRIx64 "\n", big);
    printf("uint64 padded:   0x%016" PRIx64 "\n", big);

    size_t sz = sizeof(int);
    printf("size_t:          %zu\n", sz);

    fprintf(stderr, "this goes to stderr\n");
    return 0;
}
```

Run it, then try `./printfs > out.txt`. You should see the `stderr` line on the terminal while everything else ends up in `out.txt`.

### Key takeaway

> `printf` has a rich format language: `%[flags][width]specifier`. For `uint64_t`, use `PRIx64` / `PRIu64` from `<inttypes.h>` to stay portable. `fprintf(stderr, ...)` for errors, `putchar` for single bytes.

---

## 7. The preprocessor in depth

### What you'll see

```c
#define _GNU_SOURCE
#include <elf.h>
#include <stdint.h>
...
#ifdef PT_GNU_PROPERTY
    case PT_GNU_PROPERTY: return "GNU_PROPERTY";
#endif
```

### Concept: the preprocessor runs before the compiler

Every C file goes through three stages:

1. **Preprocessing** — textual manipulation of the source: `#include`s are pasted in, `#define`s are substituted, `#if` branches are chosen. Produces a single expanded C file.
2. **Compilation** — the expanded file is parsed and compiled into machine code.
3. **Linking** — object files are combined into an executable.

The preprocessor knows nothing about C types, scopes, or functions. It's a smart text substitutor. You can see its output with `gcc -E`.

### `#include` — paste another file here

```c
#include <stdio.h>      // search system include paths
#include "my_header.h"  // search current directory first
```

`#include` literally pastes the contents of the named file into your source. A typical program's expanded source is tens of thousands of lines even if your own file is small.

### `#define` — text substitution

```c
#define MAX_USERS 100
...
int users[MAX_USERS];   // expands to: int users[100];
```

No type checking. No scope. The substitution happens on every token-level occurrence of `MAX_USERS` after the `#define`.

### Function-like macros

```c
#define SQUARE(x) ((x) * (x))
SQUARE(a + b)
// expands to: ((a + b) * (a + b))
```

The parentheses around `(x)` and around the whole expansion `((x) * (x))` are **defensive parentheses**: without them, you'd get operator precedence bugs. Common macro bug:

```c
#define SQUARE_BUG(x) x * x
SQUARE_BUG(a + b)       // expands to: a + b * a + b
//                      // which is:   a + (b * a) + b   (NOT (a+b)^2)
```

Rule: **always wrap macro parameters and the entire macro body in parentheses**.

`<elf.h>`'s `ELF64_ST_BIND(info)` macro is a real example that does this correctly:

```c
#define ELF64_ST_BIND(info)  ((info) >> 4)
```

### Macros with multiple uses of a parameter: beware side effects

A sneaky bug class. What's wrong here?

```c
#define MAX(a, b) ((a) > (b) ? (a) : (b))
int x = 5;
int y = MAX(x++, 10);
```

Expansion:

```c
int y = ((x++) > (10) ? (x++) : (10));
```

Now `x++` evaluates *twice* — `x` gets incremented twice, not once. For this reason, avoid side effects in macro arguments, or use `inline` functions instead.

### `#if`, `#ifdef`, `#ifndef`, `#else`, `#endif`

Conditional compilation. Parts of the source are included or excluded based on preprocessor conditions.

```c
#ifdef _WIN32
    // Windows-specific code
#else
    // Everything else
#endif
```

`#ifdef NAME` is true if `NAME` is defined (with any value). `#ifndef NAME` is true if it's not. `#if expression` evaluates a constant expression.

In `mini-readelf.c`:

```c
#ifdef PT_GNU_PROPERTY
    case PT_GNU_PROPERTY: return "GNU_PROPERTY";
#endif
```

This says: "If `PT_GNU_PROPERTY` is defined (meaning our `<elf.h>` is new enough to know about it), include this `case`. If not, skip it." Older `<elf.h>` headers don't define `PT_GNU_PROPERTY`, and without this `#ifdef` the code would fail to compile on old systems.

### Header guards (for when you write your own .h files)

To prevent a header from being included twice in the same compilation unit (which would cause "duplicate definition" errors), use header guards:

```c
// my_header.h
#ifndef MY_HEADER_H
#define MY_HEADER_H

// ... header contents ...

#endif /* MY_HEADER_H */
```

First include: `MY_HEADER_H` isn't defined, the body gets processed, and `MY_HEADER_H` is now defined. Second include in the same file: it's already defined, so the body is skipped. You don't need this in `mini-readelf.c` because it doesn't include any of its own headers, but every `.h` file in the world uses this pattern.

### Feature test macros: `_GNU_SOURCE`

Some libc functions and constants are disabled unless you opt in by defining a **feature test macro** *before* including any headers:

```c
#define _GNU_SOURCE      // must come BEFORE any #include
#include <string.h>      // now strdup, strerror_r, etc. are available
```

The glibc headers check for these macros at the very top and conditionally expose things. Common ones:

- `_GNU_SOURCE` — enable all GNU extensions (the maximum)
- `_POSIX_C_SOURCE = 200809L` — enable POSIX.1-2008 features
- `_XOPEN_SOURCE = 700` — enable X/Open features

For a learning project, `_GNU_SOURCE` is the easy button. For production code you might be more specific to improve portability.

### Compile-time printing: `$(info)`, `#pragma message`, and `#error`

If you want to see values or conditions at compile time:

```c
#pragma message("compiling mini-readelf")
#error "this architecture is not supported"   // halts compilation
```

These are useful for debugging weird preprocessor issues. Rarely needed in normal code.

### Lab 8: preprocessor in action

```c
// preproc.c
#define GREETING "hello"
#define SQUARE(x) ((x) * (x))

#ifdef DEBUG
#define LOG(fmt, ...) fprintf(stderr, "[debug] " fmt "\n", __VA_ARGS__)
#else
#define LOG(fmt, ...) ((void)0)   // no-op
#endif

#include <stdio.h>

int main(void) {
    printf("%s\n", GREETING);
    printf("5 squared = %d\n", SQUARE(5));
    LOG("x = %d", 42);
    return 0;
}
```

Build once without DEBUG and once with:

```sh
gcc -std=c11 -O2 preproc.c -o preproc      && ./preproc
gcc -std=c11 -O2 -DDEBUG preproc.c -o preproc_d && ./preproc_d
```

The second build will print the `[debug] x = 42` line; the first won't. Same source file, two different binaries, controlled entirely by a `#define`.

See the expanded source:

```sh
gcc -E preproc.c | tail -30
```

You'll see the `LOG(...)` expanded to an `fprintf` call (or to `(void)0`) depending on `DEBUG`.

### Key takeaway

> The preprocessor does text manipulation before compilation. `#define` substitutes text, `#if`/`#ifdef` includes/excludes blocks, feature-test macros like `_GNU_SOURCE` unlock library features. Always parenthesize macro parameters.

---

## 8. Storage classes: `const`, `static`, `extern`

### What you'll see

```c
static const char *e_type_str(uint16_t t) { ... }
const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
const int limit = 40;
```

### `const` — I promise not to modify this

`const` makes a binding **read-only**. The compiler rejects writes through it.

```c
const int x = 5;
x = 6;    // error: assignment of read-only variable
```

For pointers, `const` can apply to the pointer itself or to what it points at, and the position matters:

```c
const int *p;          // pointer to const int — can't write *p
int *const p = &x;     // const pointer to int — can't change p
const int *const p;    // both const
```

Rule of thumb: read right to left. `const int *p` = "`p` is a pointer to an int that's const". `int *const p` = "`p` is a const pointer to an int".

Why bother? Two reasons:

1. **The compiler catches bugs.** If you try to write into a `const char *` that actually points into `.rodata`, the compiler stops you at compile time instead of crashing at run time.
2. **It documents intent.** A function signature with `const` parameters tells the caller "I won't mutate your data."

In `mini-readelf.c` the whole mmapped region is `const unsigned char *base` because we only read from it. The mapping itself was `PROT_READ`, so writing would crash anyway — `const` just moves the detection to compile time.

### `static` at file scope — private to this .c file

```c
static const char *e_type_str(uint16_t t) { ... }
```

`static` at file scope means "this name is not exported to the linker". Other `.c` files can't call this function (even if they forward-declare it). Good practice because:

1. You can't accidentally collide with a same-name function in another file.
2. The compiler can inline and optimize it more aggressively (no external caller).
3. It makes the public surface of your file explicit.

Contrast with the default (`extern` — external linkage): any function or global defined without `static` is visible to the linker and can be called from any other `.c` file in the project.

### `static` at function scope — completely different meaning

Inside a function, `static` means "this variable persists across function calls, like a global, but only visible inside this function":

```c
int next_id(void) {
    static int counter = 0;   // initialized once, persists between calls
    return ++counter;
}
```

First call returns 1, second returns 2, and so on. The initialization `= 0` happens only once, before `main` runs.

**These two meanings of `static` have nothing in common besides the keyword.** Ridiculous, but historical.

### `extern` — declared here, defined elsewhere

When you have global variables or functions spread across multiple `.c` files, you `extern`-declare them in a header and define them in one specific `.c` file:

```c
// file1.h
extern int global_counter;   // declaration — tells compiler "exists somewhere"

// file1.c
#include "file1.h"
int global_counter = 0;      // definition — allocates actual storage
```

`extern` without a value says "just trust me that this exists". The linker will fail if nothing actually defines it. `mini-readelf.c` doesn't use `extern` because it's a single file with no cross-file references.

### Storage classes summary

| Keyword | At file scope | At function scope |
|---|---|---|
| (default) | External linkage — visible to linker | Automatic — on stack, discarded at function exit |
| `static` | No linkage — private to this .c file | Lifetime = entire program, initialized once |
| `extern` | Declaration only — defined elsewhere | (unusual) declaration only |
| `const` | Read-only | Read-only |
| `register` | n/a | Hint: keep in register (obsolete, ignored by compilers) |
| `auto` | n/a | The default — obsolete keyword |

### Key takeaway

> `const` = read-only. `static` at file scope = file-private. `static` inside a function = lifetime is the whole program. `extern` = "defined elsewhere".

---

## 9. Operators and control flow you'll see in the code

### The ternary operator `? :`

```c
putchar(flags & PF_R ? 'R' : ' ');
```

Read as: "if `flags & PF_R` is non-zero, evaluate to `'R'`, else `' '`". It's an expression, not a statement — it produces a value. The equivalent `if`:

```c
char c;
if (flags & PF_R) c = 'R';
else              c = ' ';
putchar(c);
```

Ternary is idiomatic in C for short, value-producing conditionals. Don't overuse it — nested ternaries become unreadable fast.

### Increment and decrement: `++` and `--`

Two forms each:

```c
int a = 5;
int b = a++;   // post-increment: b = 5, then a = 6
int c = ++a;   // pre-increment: a = 7, then c = 7
```

Post form: "use the value, then bump it". Pre form: "bump it, then use the value".

In `mini-readelf.c`:

```c
if (++shown >= limit && i + 1 < n) { ... }
```

`++shown` increments `shown` and then tests the new value against `limit`. If `shown` was 39, it becomes 40 and the test is `40 >= 40` → true.

### Short-circuit evaluation: `&&` and `||`

```c
if (a && b)   // evaluates b only if a is true
if (a || b)   // evaluates b only if a is false
```

This isn't just a performance optimization — it's a **correctness feature**. You can write:

```c
if (p != NULL && p->field > 0) { ... }
```

and rely on `p != NULL` being checked before `p->field` is accessed, avoiding a NULL dereference.

In `mini-readelf.c`:

```c
if (eh->e_phoff && eh->e_phnum) { ... }
```

"If both are non-zero" — but short-circuit means if `e_phoff` is 0, `e_phnum` isn't even read.

### `switch` / `case`

C's multi-way branch. Each `case` corresponds to a constant value:

```c
switch (d[i].d_tag) {
case DT_NEEDED:
    printf("needed\n");
    break;                  // important: without break, flow falls through!
case DT_SONAME:
    printf("soname\n");
    break;
default:
    printf("something else\n");
    break;
}
```

**Fall-through** is C's default behavior: without `break`, execution continues into the next `case`. This is sometimes intentional:

```c
case DT_RPATH:
case DT_RUNPATH:              // two cases, one body — intentional fall-through
    printf("search path\n");
    break;
```

This says "both `DT_RPATH` and `DT_RUNPATH` should print 'search path'". In `mini-readelf.c` we use intentional fall-through in several places to group tags with similar formatting.

The `default` case handles everything not matched explicitly. It's optional but good practice.

Gotcha: forgetting a `break` at the end of a case is a common bug. Enable `-Wimplicit-fallthrough` (included in `-Wextra`) to have the compiler warn.

### `continue` and `break` in loops

Already familiar from most languages:

- `continue` — skip to the next iteration
- `break` — exit the loop entirely

In `mini-readelf.c`:

```c
for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type == SHT_DYNSYM) dynsym = &sh[i];
    ...
}
```

No `continue` or `break` here — just ordinary loop body. But in `vaddr_to_filebuf`:

```c
for (int i = 0; i < phnum; i++) {
    if (ph[i].p_type != PT_LOAD) continue;
    ...
}
```

`continue` skips non-LOAD segments. Saves indentation vs nested `if`.

### The comma operator

A rarely-seen but legal C operator:

```c
for (int i = 0, j = 10; i < j; i++, j--) { ... }
```

`i++, j--` is two expressions glued into one — "do `i++` then `j--`". Useful only in `for` loop init/update slots. You probably won't write it, but you'll encounter it.

### Key takeaway

> Ternary `? :` is a value-producing if-else. `++` / `--` have pre- and post- forms. Short-circuit `&&` / `||` lets you safely combine null checks and dereferences. `switch` falls through without `break` — don't forget it.

---

## 10. POSIX file I/O: `open`, `fstat`, `close`, `perror`

### What you'll see

```c
int fd = open(argv[1], O_RDONLY);
if (fd < 0) { perror("open"); return 1; }

struct stat st;
if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }
...
close(fd);
```

### stdio vs POSIX I/O

There are two I/O APIs on Unix:

- **stdio** (`fopen`, `fread`, `fprintf`, `FILE *`) — higher-level, buffered, portable across operating systems.
- **POSIX** (`open`, `read`, `write`, integer file descriptors) — lower-level, unbuffered, the raw syscall interface.

`mini-readelf.c` uses POSIX for file opening because we need the file descriptor for `mmap`. You can't pass a `FILE *` to `mmap`.

### File descriptors

Every open file, pipe, socket, or device in a process is represented by a non-negative integer called a **file descriptor**. The kernel maintains a table of "open files" per process; your fd is an index into that table.

Three fds are special: `0` = stdin, `1` = stdout, `2` = stderr. Everything `open`ed by your code gets the next available integer (typically starting at 3).

### `open()` — get a file descriptor

```c
int open(const char *pathname, int flags);
int open(const char *pathname, int flags, mode_t mode);  // if creating
```

Flags (bitwise OR-ed):

- `O_RDONLY` — read only (value 0)
- `O_WRONLY` — write only
- `O_RDWR` — read and write
- `O_CREAT` — create if doesn't exist (requires `mode`)
- `O_TRUNC` — truncate to zero on open
- `O_APPEND` — writes go to end of file
- `O_EXCL` — with `O_CREAT`, fail if file exists

`mini-readelf.c` just wants `O_RDONLY`.

Return value: the fd on success, `-1` on failure. **Always check for `-1`.**

### `errno` and `perror`

When a POSIX function fails, it returns `-1` (or `NULL`, or `MAP_FAILED`, depending on the function) and sets a global variable `errno` to indicate the reason. `errno` is defined in `<errno.h>` and is thread-local in modern glibc.

Common errno values:

| Constant | Meaning |
|---|---|
| `ENOENT` | No such file or directory |
| `EACCES` | Permission denied |
| `ENOMEM` | Out of memory |
| `EINVAL` | Invalid argument |
| `EMFILE` | Too many open files (per-process limit) |
| `EBADF` | Bad file descriptor |

To print a human-readable error:

```c
fprintf(stderr, "open failed: %s\n", strerror(errno));
```

Or the shorter idiom:

```c
perror("open");    // prints "open: <description of errno>\n" to stderr
```

`perror` is almost always the right choice. It prepends your context string and appends the error description from `strerror(errno)`.

### `fstat()` — get file metadata

```c
int fstat(int fd, struct stat *st);
```

Fills `*st` with information about the file. The caller allocates the `struct stat` (on the stack is fine) and passes its address.

`struct stat` is large. The field `mini-readelf.c` actually uses is:

```c
off_t st_size;   // size in bytes
```

Other fields include `st_mode` (type and permissions), `st_mtime` (modification time), `st_ino` (inode), `st_uid`/`st_gid` (owner). You'll use more of them in other tools.

The `&st` in `fstat(fd, &st)` is passing the **address** of the local `struct stat` so the function can fill it in. This is how C simulates "out parameters".

### `close()` — release the file descriptor

```c
int close(int fd);
```

Releases kernel resources. Any subsequent operation on the fd fails with `EBADF`. The kernel also cleans up at process exit, but it's good hygiene to close explicitly.

In `mini-readelf.c`, we `close(fd)` **immediately after `mmap`**. That's counterintuitive ("don't we still need the file?") but correct: `mmap` keeps its own reference internally. Closing the fd just releases our side.

### Lab 9: open, stat, and close

```c
// file.c
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }

    printf("size     = %" PRId64 " bytes\n", (int64_t)st.st_size);
    printf("mode     = 0%o\n",  st.st_mode);
    printf("uid      = %u\n",   st.st_uid);
    printf("inode    = %lu\n",  (unsigned long)st.st_ino);

    close(fd);
    return 0;
}
```

Try running it on `/etc/passwd`, then on a nonexistent file. The failing run will print `open: No such file or directory` — `perror` in action.

### Key takeaway

> POSIX I/O: `open` returns a file descriptor or `-1`. Check `errno` via `perror`. `fstat` gets metadata via an out-parameter. `close` releases. `mmap` keeps the file alive internally, so you can close the fd after mmap'ing.

---

## 11. `mmap` — deep dive

### What you'll see

```c
void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
close(fd);
const unsigned char *base = map;
```

### The big idea

`mmap` asks the kernel to **project a file into your address space**. After the call, the file's contents are accessible as ordinary memory — you read them with normal load instructions, not with `read()` syscalls. It's conceptually the same thing the kernel does with your binary at `execve()` time.

### Minimal model: virtual memory 101

Every modern OS gives each process its own **virtual address space** — a flat 64-bit view of memory that the CPU's MMU translates into physical RAM (or disk) under the hood.

The translation works at **page** granularity (typically 4 KB). The MMU keeps a table that says "virtual page X maps to physical page Y (or: not loaded, fault!)". When you access a virtual address:

1. The MMU consults the page table.
2. If the page is present in RAM, the access succeeds (fast).
3. If the page is not present, the CPU raises a **page fault**.
4. The kernel's page fault handler decides what to do: read the page from disk, kill the process, whatever.

`mmap` creates mappings: "virtual address range V has data from file F starting at offset O". When you touch those virtual addresses, the kernel lazily reads the corresponding file data from disk into the page cache.

### The arguments

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

- **`addr`** — where in your address space to put the mapping. `NULL` = "kernel, pick for me".
- **`length`** — how many bytes to map.
- **`prot`** — access protection: combination of `PROT_READ`, `PROT_WRITE`, `PROT_EXEC`, or `PROT_NONE`. The kernel enforces this; violations crash with SIGSEGV.
- **`flags`** — mapping kind (more below).
- **`fd`** — file descriptor to map (or `-1` for anonymous mappings).
- **`offset`** — where in the file to start (must be page-aligned).

### `MAP_PRIVATE` vs `MAP_SHARED` vs `MAP_ANONYMOUS`

- **`MAP_PRIVATE`** — copy-on-write. Reads see the file's current contents; writes (if permitted by `prot`) go to a private copy, not back to the file. Ideal for parsing: "I want to read this file, but don't save my changes."
- **`MAP_SHARED`** — writes go back to the file, and other processes mapping the same file see them. Used by databases, IPC, and anything that wants to share memory.
- **`MAP_ANONYMOUS`** — not backed by any file. The mapping is initialized to zeros. Used for large allocations; `malloc` uses this under the hood for big blocks.

`mini-readelf.c` uses `MAP_PRIVATE` because we're read-only anyway, and we want to avoid any risk of accidentally writing back to the file.

### Return value

`mmap` returns either:

- The starting virtual address of the mapping, or
- **`MAP_FAILED`** (which is `(void *)-1`, not `NULL`!) on error.

Checking with `if (map == MAP_FAILED)` is mandatory. A common bug is `if (!map)` — which would be wrong, because `(void *)-1` is non-zero and truthy.

### Why mmap for ELF parsing?

Three reasons:

1. **Random access is free.** An ELF parser jumps to `e_phoff`, `e_shoff`, each section's `sh_offset`, each segment's `p_offset`, and various vaddr-to-offset translations. With `read()`, you'd need an `lseek()` before each, or you'd buffer the whole file. With `mmap`, every byte is just pointer arithmetic.
2. **Zero copy.** The kernel doesn't copy file data into a separate buffer. You access the page cache pages directly.
3. **Struct casting works.** Once the file is a flat memory region, you can `(Elf64_Ehdr *)base` and read fields directly.

The downside of `mmap`: for very small files or purely sequential reads, the setup cost (a syscall, page table edits) exceeds what you'd save over a simple `read`. For ELF parsing, the tradeoff is hugely in mmap's favor.

### `munmap` — release the mapping

```c
int munmap(void *addr, size_t length);
```

Tells the kernel "I'm done; you can take this back". At process exit, the kernel cleans up anyway, but explicit `munmap` is good hygiene for long-running programs.

### Lab 10: mmap a file and hex-dump its first bytes

```c
// mmap_dump.c
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <file>\n", argv[0]); return 2; }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }

    uint8_t *buf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    close(fd);

    size_t n = st.st_size < 32 ? (size_t)st.st_size : 32;
    printf("first %zu bytes of %s:\n", n, argv[1]);
    for (size_t i = 0; i < n; i++) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");

    munmap(buf, st.st_size);
    return 0;
}
```

Try it on an ELF binary:

```sh
./mmap_dump /bin/ls
```

You should see `7f 45 4c 46` (the ELF magic) as the first four bytes, followed by `02 01 01` (ELF64, little-endian, version 1), and so on. You just hand-parsed the start of an ELF header without a single `read()` call.

### Key takeaway

> `mmap` projects a file into your virtual address space. After that, file access is just memory access. The kernel does demand paging behind the scenes. Check for `MAP_FAILED` (not `NULL`). Match with `munmap`.

---

## 12. C strings and string tables

### What you'll see

```c
const char *interp_path = (const char *)(base + pt_interp->p_offset);
printf("  path: %s\n", interp_path);

// and
printf("  %s\n", strs + syms[i].st_name);

// and
if (strcmp(shstrtab + sh[i].sh_name, ".dynstr") == 0) { ... }
```

### C strings = NUL-terminated byte arrays

A C string is nothing more than a sequence of non-zero bytes followed by a single zero byte (`'\0'`). There is no length field, no string object, no length() method.

```c
char s[] = {'h', 'i', '\0'};   // 3 bytes in memory
char s[] = "hi";               // identical — the compiler adds '\0'
```

The "length" of a C string is discovered by scanning forward until NUL:

```c
size_t len = 0;
while (s[len] != '\0') len++;
```

`strlen` does this. `printf("%s", s)` does this. Everything that says "a string" means "a pointer to the first character; the string ends at the first `'\0'`".

### String literals and their storage

A literal like `"hello"` becomes 6 bytes (`h`, `e`, `l`, `l`, `o`, `\0`) stored in `.rodata` — the read-only section of the binary. When you write:

```c
const char *p = "hello";
```

...`p` becomes a pointer into `.rodata`. Attempting to write through `p` crashes with a segfault:

```c
char *p = "hello";
p[0] = 'H';       // SIGSEGV — .rodata is read-only
```

This is why we put `const` on `char *` parameters whenever the function doesn't modify the string.

### `strcmp`, `strlen`, and friends

```c
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);    // dangerous, no bounds check
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);          // find first occurrence of c
char  *strstr(const char *haystack, const char *needle);
```

`strcmp(a, b)` returns:

- `0` if the strings are **equal**
- negative if `a < b` lexicographically
- positive if `a > b`

The `== 0` idiom for equality looks backwards (`strcmp(a, b) == 0` instead of `a == b`) but becomes natural once you've written it a few times. Think of `strcmp` as "the *difference* between two strings" — zero means "no difference".

### Dangers: no bounds checking

C strings have no length metadata. Classic functions like `strcpy`, `strcat`, `sprintf`, and `gets` write into their destination without checking its size. This is the historical root of most buffer overflow bugs.

Safer alternatives use an explicit size: `strncpy`, `snprintf`, `fgets`. In modern code, always prefer the length-aware versions.

`mini-readelf.c` only *reads* strings — it never constructs or copies them — so none of these concerns apply.

### String tables — the ELF pattern

An ELF file might need to store thousands of strings: symbol names, section names, library names. Storing each one as a separate buffer would be wasteful (each needs its own allocation, each struct needs a pointer field). Instead, ELF uses a **flat string table**: one big buffer full of NUL-separated strings, indexed by **byte offset**.

Schematic:

```
offset: 0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
bytes:  \0  l   i   b   c   .   s   o   .   6  \0  p   u   t   s  \0
        ^                                         ^
        offset 0 = empty string                   offset 11 = "puts"
        (idiomatic first byte)
                                                  offset 1 = "libc.so.6"
```

Every struct entry that wants to "contain" a string stores a 4-byte offset (`uint32_t`) instead of a variable-length string. To retrieve the string: `stringtable + offset`.

### Multiple string tables in one ELF

A typical ELF has *three* string tables, each for a different purpose:

1. **`.dynstr`** — names of dynamic symbols and libraries (referenced by `.dynsym` and `.dynamic`)
2. **`.strtab`** — names for the full (non-dynamic) symbol table (referenced by `.symtab`, stripped from most binaries)
3. **`.shstrtab`** — names of sections themselves (referenced by section headers' `sh_name`)

The `e_shstrndx` field in the ELF header tells you which section holds `.shstrtab`. It's a chicken-and-egg problem: section names are stored in a section, and you have to find that section first. The ELF header solves it by recording the index directly.

`mini-readelf.c` uses all three patterns:

```c
// Pattern 1: a whole segment IS a single string (PT_INTERP)
interp_path = (const char *)(base + pt_interp->p_offset);

// Pattern 2: resolve a DT_NEEDED offset through the .dynstr table
printf("  shared library -> %s\n", strtab + d[i].d_un.d_val);

// Pattern 3: resolve a section name offset through .shstrtab
if (strcmp(shstrtab + sh[i].sh_name, ".dynstr") == 0) { ... }

// Pattern 4: resolve a dynamic symbol name offset through .dynstr
printf("  %s\n", strs + syms[i].st_name);
```

Same pattern every time: `tableBase + offset` gives you a `const char *` pointing into the middle of the string table; `printf("%s")` walks from there to the next NUL.

### Lab 11: build a string table and look up names

```c
// strtab.c
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void) {
    // Our mini string table, exactly like ELF's .dynstr:
    const char strtab[] = "\0libc.so.6\0puts\0malloc\0";
    //                     ^1         ^11  ^16    ^?

    // Entries pointing into the table by offset:
    uint32_t entries[] = { 1, 11, 16 };

    printf("entry 0 -> '%s'\n", strtab + entries[0]);  // libc.so.6
    printf("entry 1 -> '%s'\n", strtab + entries[1]);  // puts
    printf("entry 2 -> '%s'\n", strtab + entries[2]);  // malloc

    // Lookup by name using strcmp:
    const char *target = "puts";
    for (int i = 0; i < 3; i++) {
        if (strcmp(strtab + entries[i], target) == 0) {
            printf("found '%s' at entry %d (offset %u)\n",
                   target, i, entries[i]);
            break;
        }
    }
    return 0;
}
```

### Key takeaway

> C strings are NUL-terminated byte arrays with no length field. String tables store all strings in one flat buffer indexed by byte offset; structs reference strings as offsets, not pointers. `tableBase + offset` + `%s` printf walks until NUL.

---

## 13. Common pitfalls and undefined behavior

C gives you raw access to memory. That means you can also shoot yourself in the foot in ways higher-level languages simply don't allow. This section covers the bug categories you're most likely to hit, and how `mini-readelf.c` deliberately avoids or mitigates them.

### Reading past the end of a buffer

```c
Elf64_Phdr *ph = (Elf64_Phdr *)(base + eh->e_phoff);
for (int i = 0; i < eh->e_phnum; i++) {
    // reading ph[i] — are we still inside the mapped file?
}
```

If `eh->e_phnum` is tampered with (say, a malicious ELF file with `e_phnum = 1000000`), we'd read far past the end of the mapped region and crash with SIGBUS or SIGSEGV.

**`mini-readelf.c` does not bounds-check this**. It trusts the file's header to be honest. This is a deliberate simplification for a learning project — production-grade ELF parsers (binutils, LLVM) do rigorous bounds checking on every field. If you wanted to harden the tool, you'd add a helper like:

```c
static int in_bounds(const unsigned char *base, size_t file_size,
                     const void *p, size_t len) {
    const unsigned char *pp = p;
    return pp >= base && pp + len <= base + file_size;
}
```

and check every pointer before dereferencing.

### Null pointer dereference

```c
const char *name = d_tag_str(tag);
printf("%s\n", name);       // crashes if d_tag_str returned NULL
```

`d_tag_str` can return NULL for unknown tags. If we blindly passed it to `printf("%s")`, we'd SIGSEGV (actually, on glibc `printf` prints "(null)" — but relying on that is a trap). We guard against this:

```c
if (tag_name) {
    printf("%-15s ", tag_name);
} else {
    printf("0x%-13" PRIx64 " ", (uint64_t)d[i].d_tag);
}
```

### Use-after-free (not applicable here)

We don't use `malloc`/`free` at all, so there's nothing to use after freeing. But in general, accessing memory after `free()`ing it is undefined behavior and a common source of exploits.

Equivalent for us: accessing the mmap region after `munmap`. We only `munmap` right before `return`, so no subsequent access can happen.

### Uninitialized variables

```c
int x;
printf("%d\n", x);   // undefined behavior — x holds whatever was on the stack
```

Stack-allocated local variables start with **indeterminate values**. Reading them before assigning produces undefined behavior. gcc's `-Wuninitialized` (part of `-Wall`) catches many cases at compile time.

`mini-readelf.c` initializes everything explicitly: `const Elf64_Phdr *pt_interp = NULL;`, `uint64_t strtab_vaddr = 0;`, and so on. Good habit.

### Integer overflow

For **signed** integers, overflow is undefined behavior in C. Compilers are allowed to assume it never happens and optimize accordingly:

```c
int x = INT_MAX;
if (x + 1 < x) { /* compiler may delete this — it's "impossible" */ }
```

For **unsigned** integers, overflow is defined to wrap around (modulo 2^N). Both behaviors can bite you: signed overflow can introduce surprising bugs; unsigned overflow can turn large negative sizes into massive positive ones.

`mini-readelf.c` doesn't do any arithmetic that would realistically overflow a 64-bit integer on ELF files that fit in RAM, but careful code would validate sizes before multiplying.

### Comparing signed to unsigned

```c
int i = -1;
unsigned u = 1;
if (i < u) { ... }   // false! i is converted to unsigned, becomes huge
```

In C, mixing signed and unsigned in a comparison causes the signed value to be converted to unsigned first. If the signed value is negative, it becomes a huge unsigned number, and the comparison silently does the wrong thing.

gcc's `-Wsign-compare` (part of `-Wextra`) warns about this. In `mini-readelf.c` we use the explicit `(off_t)sizeof(...)` cast to avoid the warning:

```c
if (st.st_size < (off_t)sizeof(Elf64_Ehdr)) { ... }
```

### Buffer overflows with string functions

```c
char small[8];
strcpy(small, "this string is way too long");   // writes past the end of small
```

`strcpy` has no bounds check. It happily walks off the end of `small`, corrupting whatever is next on the stack (including return addresses — the classic stack buffer overflow exploit).

Fixes: use `snprintf` instead of `sprintf`, `strncpy` instead of `strcpy` (with care — it doesn't always NUL-terminate), `fgets` instead of `gets`, etc. Or better, use lengths explicitly: `memcpy(dst, src, len)`.

`mini-readelf.c` never writes strings, so this doesn't apply.

### Format string bugs

```c
char *user_input = /* untrusted */;
printf(user_input);          // EXPLOITABLE
printf("%s", user_input);    // safe
```

If an attacker controls the format string, they can read from and write to arbitrary memory using format specifiers like `%x`, `%n`, etc. Always pass untrusted strings as **arguments**, never as the format string itself.

### Pointer type confusion and alignment

On x86-64, unaligned loads of `uint64_t` are allowed but may be slightly slower. On some RISC architectures (older ARM, MIPS), unaligned loads **crash with SIGBUS**. Code that relies on struct casting from arbitrary offsets can work on one arch and fail on another.

For `mini-readelf.c`, `mmap` returns page-aligned pointers (4 KB alignment), and all the offsets we cast to (e.g., `base + eh->e_phoff`) happen to be naturally aligned in any well-formed ELF. So we're safe on x86-64. On a crazy architecture, you'd use `memcpy` into a local struct to sidestep the issue.

### Signed shifts

```c
int x = -4;
int y = x >> 1;     // implementation-defined on signed types
```

Right-shifting a negative integer is implementation-defined. On virtually every modern compiler it does arithmetic shift (preserving sign), but the standard doesn't guarantee it. **Always shift unsigned types** to avoid ambiguity.

### Enabling compiler warnings

All of the bugs above are catchable to varying degrees by turning on warning flags. `mini-readelf.c` is built with:

```
-Wall -Wextra -Wpedantic
```

Breakdown:

- `-Wall` — "all the common warnings" (not literally all, despite the name)
- `-Wextra` — additional warnings like `-Wsign-compare`, `-Wimplicit-fallthrough`
- `-Wpedantic` — strict ISO C conformance

For maximum paranoia, add `-Wshadow -Wconversion -Wundef -Wcast-align` — but these are noisy on many codebases.

### Running the tool through valgrind

```sh
valgrind ./src/mini-readelf ./src/mini-readelf
```

`valgrind` instruments every memory access and reports uninitialized reads, use-after-free, out-of-bounds accesses, and leaks. For a learning project, it's worth running once to confirm the tool is clean. `mini-readelf.c` currently has no leaks and no undefined reads — try it and you should see `All heap blocks were freed -- no leaks are possible`.

### Key takeaway

> C puts you in charge of memory safety. The main hazards are reading past buffers, NULL dereferences, uninitialized variables, integer overflow, and mixing signed/unsigned. Use `-Wall -Wextra -Wpedantic` and run `valgrind` periodically.

---

## 14. Putting it all together — re-read `main()`

Here's the top of `mini-readelf.c`'s `main()`, annotated with the section of this doc that covers each piece:

```c
int main(int argc, char **argv) {                        // §9 control flow
    if (argc != 2) {                                     // §6 printf + §10 stderr
        fprintf(stderr, "usage: %s <elf-file>\n",
                argv[0]);
        return 2;                                        // §9 exit codes
    }

    int fd = open(argv[1], O_RDONLY);                    // §10 POSIX I/O
    if (fd < 0) { perror("open"); return 1; }            // §10 errno + perror

    struct stat st;                                      // §3 struct
    if (fstat(fd, &st) < 0) {                            // §2 & operator (address-of)
        perror("fstat"); close(fd); return 1;
    }
    if (st.st_size < (off_t)sizeof(Elf64_Ehdr)) {        // §1 sizeof + off_t cast
        fprintf(stderr, "file too small to be an ELF\n");
        close(fd); return 1;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ,        // §11 mmap
                     MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {                             // §11 MAP_FAILED check
        perror("mmap"); close(fd); return 1;
    }
    close(fd);                                           // §11 mmap keeps file alive

    const unsigned char *base = map;                     // §2 byte pointer + §8 const

    if (base[EI_MAG0] != ELFMAG0 ||                      // §2 byte indexing
        base[EI_MAG1] != ELFMAG1 ||                      // §7 ELFMAG* are #defines
        base[EI_MAG2] != ELFMAG2 ||
        base[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "not an ELF file (bad magic)\n");
        munmap(map, st.st_size);                         // §11 cleanup
        return 1;
    }

    if (base[EI_CLASS] != ELFCLASS64) { ... }            // §5 byte value compare
    if (base[EI_DATA] != ELFDATA2LSB) { ... }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;     // §3 the reinterpret cast

    printf("  Type:   0x%x  %s\n",                       // §6 printf
           eh->e_type,                                   // §3 -> field access
           e_type_str(eh->e_type));                      // §8 static helper

    printf("  Entry point address:   0x%" PRIx64 "\n",   // §6 PRIx64
           eh->e_entry);

    // ... and so on
}
```

**Every single line is one of the patterns from this doc.** The rest of `main()` — walking program headers, reading PT_INTERP, parsing .dynamic, finding .dynsym via section headers — is just more of the same patterns applied to different ELF structures.

Once these thirteen patterns are second nature, **any** C systems code becomes readable: ptrace-based tracers, shared-library shims, syscall wrappers, kernel modules. You're building the base skill.

---

## 15. Lab index

All the labs from this doc, in one place. Work through them in order for maximum effect, or pick and choose based on where you're stuck:

| Lab | Concept | File |
|---|---|---|
| Setup | Toolchain smoke test | `smoke.c` |
| 1 | Types, sizes, addresses, memory layout | `sizes.c` |
| 2 | Pointer basics: `&`, `*`, writing through a pointer | `ptrs.c` |
| 3 | Pointer arithmetic on a byte buffer | `arith.c` |
| 4 | Casting raw bytes to a struct | `castbytes.c` |
| 5 | Union type punning | `union.c` |
| 6 | Bit packing and unpacking | `bits.c` |
| 7 | `printf` format language | `printfs.c` |
| 8 | Preprocessor: `#define`, `#ifdef`, compile-time debug | `preproc.c` |
| 9 | POSIX file I/O: open/fstat/close/perror | `file.c` |
| 10 | `mmap` a file and hex-dump it | `mmap_dump.c` |
| 11 | Build and use a string table | `strtab.c` |

Each lab takes 5-10 minutes. Running them is worth more than a hundred more pages of prose.

---

## Epilogue: what this doc didn't cover

C is a big language. Things deliberately left out because `mini-readelf.c` doesn't use them:

- **`malloc` / `free`** — dynamic memory. Steps 2 and 3 will probably need them.
- **Function pointers and callbacks** — `int (*f)(int, int)` syntax. Useful for syscall-handler dispatch tables.
- **`va_list` / variadic functions** — what `printf` uses internally.
- **Multi-file projects and header files** — split a program across several `.c` and `.h` files. Step 2 might.
- **Threads and atomics** — `pthread_create`, `_Atomic`. Not needed for a single-threaded tracer.
- **Signal handlers** — `signal`, `sigaction`. Step 2's ptrace uses signals heavily but in a different way.
- **`setjmp`/`longjmp`** — non-local exits. Rare.
- **`volatile`** — for memory-mapped I/O and signal handlers. Not needed here.
- **`_Generic`** — C11's type-based dispatch. Niche.
- **Array-pointer decay** — the quirk where array parameters silently become pointers. Mostly a footnote.

If you hit any of these in steps 2 or 3, tell me and I'll add a doc or extend this one.

---

## If a section didn't click

- **§1 (types/memory)** — confused about the difference between stack/heap/data/text? Run Lab 1 and look at the actual addresses. They should cluster visibly.
- **§2 (pointers)** — the key exercise is Lab 2: convince yourself that `p` and `&x` really do hold the same value, and that `*p = 99` really does change `x`.
- **§3 (structs & casts)** — Lab 4 is the most important lab in the whole doc. If the cast feels magical, run it, print the field values, and confirm they match the bytes you hand-built.
- **§4 (unions)** — Lab 5. Write 3.14 as a float, read the same 4 bytes as an integer, watch the bit pattern emerge.
- **§5 (bit packing)** — Lab 6. Pack a byte, unpack it, verify round-trip.
- **§6 (printf)** — Lab 7. Play with widths and padding until you get predictable column alignment.
- **§7 (preprocessor)** — Lab 8 + the `gcc -E` trick to see expanded source.
- **§10 (POSIX I/O)** — Lab 9 on a few real files; deliberately try a missing file to see `perror` in action.
- **§11 (mmap)** — Lab 10, run it on `/bin/ls`, match the first four bytes with the ELF magic.
- **§12 (strings)** — Lab 11, then reopen `mini-readelf.c` and count how many times you see the `strs + offset` pattern.

Tell me which one doesn't click and I'll zoom in.
