# 07 — ELF security: the attacker's and defender's perspective

> The previous docs taught you what's *inside* an ELF and how the loader runs it. This doc flips the lens: how does a **security researcher**, **penetration tester**, or **reverse engineer** look at the same binary? What do the fields you already learned reveal about whether a binary is hardened or vulnerable, what attack surfaces it exposes, and how to investigate it further?
>
> Prerequisites: you should be comfortable with docs 01–05 (ELF anatomy, linking, entry point, PIE, walkthrough). Doc 06 (C idioms) helps for the code-level sections.
>
> This doc doesn't assume security experience. It builds from ELF concepts you already have.

---

## 1. The attacker's first look at a binary

When a pen tester or CTF player receives a binary they've never seen before, the first 60 seconds look like this:

```sh
file target                         # what kind of file? ELF? which arch?
checksec --file=target              # hardening at a glance (or use mini-readelf)
strings target | head -50           # any readable strings? error messages? URLs?
readelf -s target | grep FUNC      # exported/imported functions?
./target                            # what does it do? what does it print?
```

Those five commands — before even opening a disassembler — tell you:

1. **Architecture and linking** — 32-bit or 64-bit? dynamic or static? PIE or fixed-address?
2. **Hardening posture** — are modern defenses enabled or missing?
3. **Attack surface clues** — interesting strings (format strings, passwords, file paths, debug messages) that hint at vulnerable code paths.
4. **Imported functions** — does it call `gets`, `strcpy`, `system`, `execve`, `printf` (without a format argument)? Each one is a potential vulnerability class.
5. **Behavior** — what inputs does it accept? does it read from stdin? open a socket? read a file?

You can already do #1 and partial #2 with `mini-readelf`. This doc teaches you to extract #2 fully and understand #3-#5.

---

## 2. Binary hardening checklist — reading defenses from the ELF

Modern compilers and linkers insert several exploit mitigations. An attacker's first question is always: *which ones are on, and which ones are missing?*

Here's the full checklist, how each defense works, how to detect it from the ELF, and what an attacker does when it's present vs absent.

### 2.1 PIE — Position-Independent Executable

**What it does:** the entire executable is loaded at a random base address (ASLR applies to the code, not just libraries).

**How to detect:**

```sh
# With mini-readelf:
./src/mini-readelf target | grep "Type:"
#   Type: 0x3  DYN  → PIE (randomized)
#   Type: 0x2  EXEC → no PIE (fixed at 0x400000)

# With readelf:
readelf -h target | grep Type

# With file:
file target
#   "pie executable" vs "executable"
```

**Attacker impact:**
- **PIE on:** the attacker can't hardcode gadget addresses from the binary itself. They need an information leak first.
- **PIE off:** every address in the binary is fixed and known. Gadgets, strings, GOT entries — all at predictable locations. Much easier to exploit.

**Covered in detail in:** [doc 04](04-pie-and-aslr.md). Not repeated here.

### 2.2 NX — Non-Executable Stack (and heap)

**What it does:** marks the stack (and other writable regions) as non-executable. If an attacker overwrites the stack with shellcode and jumps to it, the CPU refuses to execute it.

**How to detect:**

```sh
# With mini-readelf — look for PT_GNU_STACK:
./src/mini-readelf target | grep GNU_STACK
#   GNU_STACK     RW  ...   ← NX enabled (not executable)
#   GNU_STACK     RWE ...   ← NX disabled (stack is executable!)

# If there's NO GNU_STACK segment at all, the behavior is platform-dependent
# (usually NX is on by default on modern kernels).
```

The key is the **flags column**: `RW` (no E) means non-executable stack. `RWE` means the stack is executable — a serious weakness.

**Attacker impact:**
- **NX on:** classic "jump to shellcode on the stack" doesn't work. Attacker must use techniques like ROP (return-oriented programming) that reuse existing code instead of injecting new code.
- **NX off:** attacker can place shellcode on the stack and jump to it directly. Much simpler exploitation.

**How to disable (for testing):**

```sh
gcc -z execstack vuln.c -o vuln   # produces RWE stack
```

### 2.3 RELRO — Relocation Read-Only

**What it does:** after the dynamic linker finishes applying relocations, it makes certain writable sections (notably the GOT) read-only. This prevents GOT overwrite attacks.

**Two levels:**

| Level | What's protected | Linker flags |
|---|---|---|
| **Partial RELRO** | `.init_array`, `.fini_array`, `.dynamic`, `.got` are read-only after loading. But **`.got.plt` is still writable** (lazy binding needs to write to it). | Default on most distros |
| **Full RELRO** | Everything above PLUS `.got.plt` is read-only. All symbols resolved at load time (no lazy binding). | `-Wl,-z,relro,-z,now` |

**How to detect:**

```sh
# With mini-readelf — look for PT_GNU_RELRO and DT_BIND_NOW or DT_FLAGS:
./src/mini-readelf target | grep -E "GNU_RELRO|BIND_NOW|FLAGS"
#   GNU_RELRO     R   ...  ← partial or full RELRO (segment exists)
#   BIND_NOW      ...      ← if present → full RELRO
#   FLAGS         0x8      ← BIND_NOW flag (DF_BIND_NOW = 0x8)
#   FLAGS_1       0x...1   ← DF_1_NOW = 0x1 in FLAGS_1

# With readelf:
readelf -l target | grep GNU_RELRO
readelf -d target | grep -E "BIND_NOW|FLAGS"
```

Decision tree:

```
No PT_GNU_RELRO         → No RELRO
PT_GNU_RELRO present    → at least Partial RELRO
  + BIND_NOW or DF_1_NOW → Full RELRO
  - neither              → Partial RELRO only
```

**Attacker impact:**
- **No RELRO:** attacker can overwrite any GOT entry to redirect function calls. Classic GOT overwrite.
- **Partial RELRO:** `.got.plt` is still writable. Attacker can still overwrite PLT-related GOT entries (like `puts@got.plt`) to hijack calls.
- **Full RELRO:** GOT is read-only after loading. GOT overwrite doesn't work. Attacker must find other primitives (e.g., overwrite `__malloc_hook`, `__free_hook` on older glibc, or target `.bss`/heap metadata).

### 2.4 Stack canaries (Stack Smashing Protection)

**What it does:** the compiler inserts a random value (the "canary") between the local variables and the saved return address on each stack frame. Before returning, it checks the canary. If it's been overwritten (by a buffer overflow), the program aborts instead of returning to the attacker's address.

**How to detect:**

```sh
# Check if the binary imports __stack_chk_fail (the abort function):
readelf -s target | grep stack_chk
#   __stack_chk_fail@GLIBC   ← canaries are enabled

# Or with nm:
nm -D target | grep stack_chk

# Or with mini-readelf's .dynsym output — look for __stack_chk_fail
```

If `__stack_chk_fail` appears in the dynamic symbols, the binary uses stack canaries on at least some functions.

**Attacker impact:**
- **Canaries on:** a linear stack buffer overflow will overwrite the canary before reaching the return address, and the program will abort. Attacker needs to either: (a) leak the canary value first, (b) find a non-linear overwrite that skips the canary, or (c) find a vulnerability that doesn't involve overflowing the stack.
- **Canaries off:** classic stack buffer overflow works — overwrite saved `rip`, control the return address.

**Compile flags:**

```sh
gcc -fstack-protector-strong vuln.c -o vuln   # canaries on important functions (default)
gcc -fstack-protector-all vuln.c -o vuln      # canaries on ALL functions
gcc -fno-stack-protector vuln.c -o vuln       # no canaries (for testing)
```

### 2.5 FORTIFY_SOURCE

**What it does:** replaces dangerous standard library functions (`strcpy`, `sprintf`, `memcpy`, etc.) with bounds-checked versions at compile time, when the compiler can determine the destination buffer size.

```c
// Without FORTIFY: strcpy is called directly — no bounds check
strcpy(buf, input);

// With FORTIFY: __strcpy_chk is called, which checks sizeof(buf)
__strcpy_chk(buf, input, __builtin_object_size(buf, 0));
```

**How to detect:**

```sh
# Check for _chk or _fortify variants in dynamic symbols:
readelf -s target | grep -i '_chk\|fortify'
nm -D target | grep '__.*_chk'
#   __printf_chk    ← printf was fortified
#   __memcpy_chk    ← memcpy was fortified
```

If you see `__printf_chk` instead of `printf`, `__memcpy_chk` instead of `memcpy`, etc., FORTIFY is active.

**Attacker impact:**
- **FORTIFY on:** some buffer overflows that the compiler can size-check at compile time will be caught. Format string bugs with `%n` are also blocked by `__printf_chk`.
- **FORTIFY off:** those overflows proceed unchecked.
- **Limitation:** FORTIFY only works when the compiler knows the buffer size at compile time. Heap-allocated buffers with dynamic sizes are often not protected.

### 2.6 CET — Intel Control-flow Enforcement Technology

**What it does:** two features: (a) **Indirect Branch Tracking (IBT)** — every valid indirect jump target must start with `endbr64`; jumping to any other instruction triggers #CP exception. (b) **Shadow Stack** — a hardware-managed copy of return addresses that's compared against the software stack on `ret`.

**How to detect:**

```sh
# With mini-readelf — look for PT_GNU_PROPERTY:
./src/mini-readelf target | grep GNU_PROPERTY
# If present, the binary declares CET support.

# With readelf:
readelf -n target | grep -i "IBT\|SHSTK"
#   Properties: x86 feature: IBT, SHSTK
```

**Attacker impact:**
- **CET on:** ROP is significantly harder (shadow stack catches return-address overwrites). JOP (jump-oriented programming) is also restricted by IBT.
- **CET off:** classic ROP/JOP works.
- **Caveat:** CET requires CPU support (Intel Tiger Lake+), kernel support (Linux 5.18+), and compiler support. Many binaries don't have it yet.

### 2.7 Quick hardening summary

A typical modern binary on Ubuntu 22.04 compiled with default `gcc` flags has:

```
PIE:           Yes  (gcc defaults to -pie)
NX:            Yes  (gcc defaults to non-executable stack)
Partial RELRO: Yes  (linker default)
Full RELRO:    No   (needs -Wl,-z,now — not default)
Canaries:      Yes  (gcc defaults to -fstack-protector-strong)
FORTIFY:       Depends  (needs -D_FORTIFY_SOURCE=2 or =3)
CET:           Maybe (needs newer CPU + -fcf-protection=full)
```

The most common gap on default builds is **full RELRO** and **FORTIFY_SOURCE**. An attacker looks for those gaps first.

### 2.8 Using mini-readelf for security recon

Here's a one-shot checklist you can run with the tool you already built:

```sh
./src/mini-readelf target
```

Then scan the output for:

| Look for | Field/Section | Meaning |
|---|---|---|
| `Type: EXEC` | ELF header | **No PIE** — fixed addresses, easier to exploit |
| `Type: DYN` | ELF header | **PIE** — addresses randomized |
| `GNU_STACK RWE` | Program headers | **Executable stack** — shellcode directly on stack |
| `GNU_STACK RW` | Program headers | **NX** — stack not executable |
| No `GNU_RELRO` | Program headers | **No RELRO** — GOT fully writable |
| `GNU_RELRO R` | Program headers | **At least partial RELRO** |
| `BIND_NOW` or `FLAGS 0x8` | .dynamic | **Full RELRO** — GOT is read-only |
| `GNU_PROPERTY` | Program headers | **CET** markers present |
| `__stack_chk_fail` | .dynsym | **Stack canaries** enabled |
| `__printf_chk` etc. | .dynsym | **FORTIFY_SOURCE** enabled |

---

## 3. PLT/GOT: how function calls get hijacked

This is the single most important exploitation primitive for dynamically-linked binaries. You know from [doc 02](02-linking-and-loading.md) that calls to shared-library functions go through the PLT and GOT. Here's how an attacker abuses that.

### 3.1 The PLT/GOT mechanism (attack-oriented view)

When your program calls `puts("hello")`, the actual machine code does:

```
call puts@plt          ; jump to the PLT stub
```

The PLT stub does:

```asm
puts@plt:
    jmp [puts@got.plt]     ; jump to whatever address is stored in the GOT slot
    push <reloc-index>     ; (only reached on first call, before lazy resolution)
    jmp <resolver>
```

**On first call:** the GOT slot contains the address of the `push <reloc-index>` instruction (the second line of the PLT stub). So the jump loops back into the stub, which pushes the relocation index and jumps to the dynamic linker's resolver. The resolver looks up the real address of `puts` in `libc.so.6`, writes it into the GOT slot, and then calls `puts`.

**On subsequent calls:** the GOT slot now contains the real address of `puts` in libc. The `jmp [puts@got.plt]` goes straight to `puts`. No resolver needed.

### 3.2 The GOT overwrite attack

Here's the key insight: **the GOT slot is writable memory at a known address (in non-PIE binaries) that controls where a function call goes**. If an attacker can write an arbitrary value to an arbitrary address, they can:

1. Find the address of `puts@got.plt` (fixed in a non-PIE binary, or leaked from a PIE).
2. Overwrite it with the address of `system` (or any other function).
3. The next time the program calls `puts(user_input)`, it actually calls `system(user_input)`.
4. If `user_input` is `"/bin/sh"`, the attacker gets a shell.

```
Before:  puts@got.plt → 0x7f1234abcdef (real puts in libc)
Attack:  write 0x7f1234567890 (address of system) to puts@got.plt
After:   puts@got.plt → 0x7f1234567890 (system!)
         program calls puts("something") → actually calls system("something")
```

This is why **full RELRO** exists: it makes `.got.plt` read-only after the linker resolves all symbols, so the overwrite hits a read-only page and crashes with SIGSEGV instead of succeeding.

### 3.3 See it yourself

```sh
# Compile a vulnerable binary (no PIE, no full RELRO, for simplicity):
gcc -no-pie -Wl,-z,norelro vuln.c -o vuln

# Find the GOT entry for puts:
objdump -R vuln | grep puts
#   0000000000404018 R_X86_64_JUMP_SLOT  puts@GLIBC_2.2.5

# That address (0x404018) is the GOT slot. An arbitrary-write primitive
# targeting that address lets you redirect puts() calls.

# With full RELRO the same command shows the relocation but the slot
# is mapped read-only after loading — the overwrite crashes.
```

### 3.4 Defense summary

| Protection | GOT writable? | Attacker can redirect calls? |
|---|---|---|
| No RELRO | Yes, always | Yes — classic GOT overwrite |
| Partial RELRO | `.got.plt` still writable | Yes — PLT-called functions |
| Full RELRO | No — all GOT read-only | No — must find another target |

---

## 4. ROP basics — when NX stops shellcode

When NX is on, the attacker can't put shellcode on the stack and jump to it. **Return-Oriented Programming (ROP)** is the workaround: instead of injecting new code, reuse tiny sequences of existing code ("gadgets") that are already in executable memory.

### 4.1 What a gadget is

A gadget is a short instruction sequence ending in `ret`. Example:

```asm
; Gadget at address 0x401234:
pop rdi         ; load the top of the stack into rdi
ret             ; return to the next address on the stack
```

By overflowing the stack with a chain of return addresses, each pointing to a different gadget, the attacker can:

1. `pop rdi; ret` — load a chosen value into `rdi` (the first argument register)
2. `ret` to `system@plt` — call `system(rdi)` with the attacker's argument

A complete exploit chain might look like this on the stack:

```
[overflow padding...] [canary (if leaked)] [saved rbp] →
→ [addr of pop_rdi_gadget] [addr of "/bin/sh" string] [addr of system@plt]
```

### 4.2 Finding gadgets

```sh
# If you have ROPgadget installed:
ROPgadget --binary target | grep "pop rdi"

# Or with ropper:
ropper --file target --search "pop rdi"

# Or manually with objdump:
objdump -d target | grep -B1 "ret$"
```

Every `ret` instruction is potentially the end of a gadget. The preceding 1-5 instructions are the gadget body. In a large binary (or libc), there are thousands of usable gadgets.

### 4.3 Why ASLR makes ROP harder (but not impossible)

With ASLR, libc is loaded at a random base. The attacker doesn't know where `system` or libc gadgets are. But:

- **If the binary isn't PIE**, the binary's own code is at a fixed address. Gadgets in the binary itself are usable without any leak. PLT entries (like `puts@plt`) are also at fixed addresses.
- **The "leak then pwn" pattern**: the attacker uses a vulnerability to leak a libc address (e.g., by printing a GOT entry that's already been resolved), computes libc's base from that one address, then computes every other libc address. One leak = full access.

### 4.4 The "leak + pwn" attack flow

A typical real-world exploit against a buffer-overflow vulnerable binary:

1. **Recon**: check hardening. PIE? RELRO? Canaries?
2. **Stage 1 — leak**: overflow the stack to call `puts@plt(puts@got.plt)`. This prints the runtime address of `puts` in libc to stdout.
3. **Calculate**: `libc_base = leaked_puts - known_puts_offset`. Now you know where everything in libc is.
4. **Stage 2 — exploit**: overflow again, this time building a ROP chain that calls `system("/bin/sh")` using addresses computed from `libc_base`.
5. **Shell**.

This pattern works against every non-PIE binary with partial RELRO. Full RELRO doesn't stop it (the leak reads from GOT, doesn't write). The only defenses are: PIE (so the binary's PLT addresses are also randomized), or no exploitable vulnerability in the first place.

---

## 5. Reverse engineering an ELF binary

### 5.1 The recon phase (static analysis)

Before running anything, extract all the information you can from the file on disk:

```sh
# Basic identification:
file target
./src/mini-readelf target

# All readable strings (passwords, URLs, debug messages, error paths):
strings target | less

# Symbol tables (if not stripped):
nm target                           # all symbols
nm -D target                        # dynamic symbols only
readelf -s target                   # both tables

# Disassemble:
objdump -d target | less            # full disassembly
objdump -d -M intel target | less   # Intel syntax (easier to read)

# Section contents:
readelf -x .rodata target          # hex dump of read-only data
readelf -p .rodata target          # printable strings in .rodata
```

### 5.2 Finding `main` in a stripped binary

When a binary is stripped (`strip target`), all symbols are removed. `nm` shows nothing. But you can still find `main`:

**Method 1 — via `_start`:**

`e_entry` is always preserved (it's in the ELF header, not in sections). It points to `_start`. Disassemble there:

```sh
# Get entry point:
./src/mini-readelf target | grep "Entry point"
#   Entry point address:   0x1080

# Disassemble around it:
objdump -d target | grep -A 20 "1080:"
```

In `_start`, the instruction `lea rdi, [rip + 0x...]` loads the address of `main` into `rdi` (it's the first argument to `__libc_start_main`). Follow that RIP-relative offset to find `main`.

**Method 2 — via `__libc_start_main` call:**

Even in a stripped binary, `__libc_start_main` is still in `.dynsym` (it's needed for dynamic linking). Find the call to it in `_start`; the first argument (`rdi`) is `main`.

**Method 3 — via strings:**

If the program prints anything identifiable, find that string in `.rodata`, then use cross-references (in Ghidra/radare2) to find which function references it. That function is probably near `main`.

### 5.3 Dynamic analysis

```sh
# Trace system calls:
strace ./target 2>&1 | less

# Trace library calls:
ltrace ./target 2>&1 | less

# Watch the dynamic linker work:
LD_DEBUG=all ./target 2>&1 | less

# Run under gdb:
gdb ./target
(gdb) break main         # or break *0x1080 for stripped binaries
(gdb) run
(gdb) info registers     # see register state
(gdb) x/20gx $rsp        # examine stack
(gdb) disas               # disassemble current function
```

**GDB plugins for security work** (install one of these; they add exploit-development commands):

- **pwndbg** — https://github.com/pwndbg/pwndbg (most actively maintained)
- **GEF** — https://github.com/hugsy/gef
- **PEDA** — https://github.com/longld/peda

These add commands like `checksec`, `got`, `heap`, `rop`, `pattern create`, `vmmap`, etc. — all designed for exploit development.

### 5.4 What `mini-readelf` tells a reverse engineer

Using just our tool's output, a reverse engineer can determine:

| Question | Where to look |
|---|---|
| 32-bit or 64-bit? | `Class: ELF64` or check `e_machine` |
| Dynamically or statically linked? | `PT_INTERP` present or absent |
| What libraries does it use? | `DT_NEEDED` entries |
| What functions does it import? | `.dynsym` table — undefined symbols with value `0x0` |
| What functions does it export? | `.dynsym` table — defined symbols with non-zero values |
| Is it a PIE? | `e_type == ET_DYN` with `PT_INTERP` |
| Where does execution start? | `e_entry` → `_start` → follow to `main` |
| Is there a debugger trap? | Look for `ptrace` in `.dynsym` — anti-debugging |
| Can I overwrite the GOT? | `PT_GNU_RELRO` + `BIND_NOW` check |
| Can I put shellcode on stack? | `PT_GNU_STACK` flags: `RW` vs `RWE` |

---

## 6. Format string vulnerabilities

One of the most elegant vulnerability classes, and directly related to the `printf` family you learned in [doc 06](06-c-idioms-for-this-project.md).

### 6.1 The bug

```c
char buf[100];
fgets(buf, sizeof(buf), stdin);
printf(buf);          // VULNERABLE — user input IS the format string
printf("%s", buf);    // SAFE — user input is an argument, not the format
```

If the user types `%x %x %x %x`, the first version calls `printf("%x %x %x %x")`. printf expects four arguments on the stack, but none were passed. So it reads whatever garbage happens to be on the stack — **leaking stack contents**.

### 6.2 Reading memory (information leak)

```
Input:  %p %p %p %p %p %p
Output: 0x7ffd1234abc0 0x7f9876543210 0x0 0x401234 ...
```

Each `%p` reads the next 8 bytes from the stack as a pointer and prints them. The attacker can leak: stack addresses (defeating stack ASLR), libc addresses (defeating library ASLR), canary values, and return addresses.

Direct parameter access is even more powerful:

```
Input:  %7$p
Output: 0x00007fff12345678
```

`%7$p` reads the 7th "argument" position — the attacker can surgically target specific stack slots.

### 6.3 Writing memory (arbitrary write)

The `%n` specifier writes the number of characters printed so far to an address pointed to by the corresponding argument:

```c
int count;
printf("hello%n", &count);   // count = 5 (length of "hello")
```

By controlling the format string, an attacker can use `%n` with padding (`%100c` prints 100 characters) to write an arbitrary value to an arbitrary address:

```
%<value>c%<position>$n
```

This is a **write-what-where** primitive, which is enough to overwrite a GOT entry, a return address, or any other writable memory.

### 6.4 Defenses

- **Don't pass user input as a format string.** Always `printf("%s", user_input)`, never `printf(user_input)`.
- **FORTIFY_SOURCE** replaces `printf` with `__printf_chk`, which rejects `%n` when the format string is in writable memory.
- **GCC `-Wformat-security`** (included in `-Wall`) warns about format strings that aren't string literals.

### 6.5 Detection

```sh
# Look for printf/sprintf/fprintf calls with non-literal format strings:
# (Requires source or disassembly inspection)

# In disassembly, look for:
#   lea rdi, [rbp-0x...]   ← format string comes from a local buffer
#   call printf@plt        ← BAD — buffer is the format string

# vs:
#   lea rdi, [rip+0x...]   ← format string comes from .rodata (a literal)
#   call printf@plt        ← SAFE
```

The difference: if `rdi` (the format argument) points into `.rodata`, it's a string literal and can't be controlled by the attacker. If it points to the stack or heap, the attacker might control it.

---

## 7. Binary patching — modifying an ELF on disk

Sometimes you want to change a binary's behavior without recompiling (you don't have the source, or it's a CTF challenge, or you're testing a patch).

### 7.1 NOPping out an instruction

"NOP" (no-operation, opcode `0x90` on x86) is the simplest patch. Replace an instruction with NOPs to skip it.

Example: bypass a password check:

```sh
# Find the check in the disassembly:
objdump -d target | grep -A5 "call.*check_password"
#   401234: e8 XX XX XX XX    call check_password
#   401239: 85 c0             test eax, eax
#   40123b: 74 10             je   40124d     ← jump if check passed

# The "je" (jump if equal/zero) at 0x40123b is 2 bytes: 74 10.
# NOP it out (replace with 90 90):
printf '\x90\x90' | dd of=target bs=1 seek=$((0x123b)) conv=notrunc

# Or change "je" (74) to "jmp" (eb) to always take the branch:
printf '\xeb' | dd of=target bs=1 seek=$((0x123b)) conv=notrunc
```

### 7.2 Changing a single byte

```sh
# Read the current byte at offset 0x1234:
xxd -s 0x1234 -l 1 target

# Write a new byte:
printf '\xcc' | dd of=target bs=1 seek=$((0x1234)) conv=notrunc
# 0xcc = int3 (breakpoint trap) — useful for setting persistent breakpoints
```

### 7.3 Tools for structured patching

- **`patchelf`** — change the dynamic linker path (`PT_INTERP`), add/remove `DT_NEEDED` entries, set `RPATH`/`RUNPATH`. Doesn't touch code, only ELF metadata.
- **`LIEF`** (Python library) — read, modify, and write ELF/PE/Mach-O files programmatically. Can add sections, modify headers, patch code.
- **`objcopy`** — add/remove sections, change section flags.

```sh
# Change which dynamic linker a binary uses:
patchelf --set-interpreter /path/to/my/ld.so target

# Add a library dependency:
patchelf --add-needed libcustom.so target

# Set the RPATH (where to search for libraries):
patchelf --set-rpath /my/libs target
```

### 7.4 Why this matters for security

- **Offensive**: patching out anti-debugging checks (`ptrace(PTRACE_TRACEME, ...)` that returns -1 if already traced), patching out license checks, bypassing integrity verification.
- **Defensive**: hotpatching a vulnerability without recompiling (replacing a `call gets` with `call fgets_wrapper`), adding instrumentation.
- **Forensics**: modifying a malware sample to disable self-destruction or C2 communication for safe analysis.

---

## 8. Syscall-level security — seccomp-bpf

This bridges to [step 2](../../02-syscall-tracer/) (the syscall tracer) and [step 3](../../03-library-tracer/) (library tracer / sandbox).

### 8.1 What seccomp is

**seccomp-bpf** (Secure Computing with Berkeley Packet Filter) lets a process install a filter that the kernel applies to every syscall the process makes. The filter can:

- **Allow** the syscall
- **Kill** the process (SECCOMP_RET_KILL)
- **Return an error** (SECCOMP_RET_ERRNO)
- **Trap** to a tracer (SECCOMP_RET_TRACE)
- **Log** the syscall and allow (SECCOMP_RET_LOG)

### 8.2 Why it matters

Modern sandboxes use seccomp-bpf to restrict what a program can do:

- **Chrome/Chromium** — renderer processes can only call ~20 syscalls (read, write, mmap, etc.). No network, no filesystem access, no `execve`.
- **Docker** — applies a default seccomp profile that blocks dangerous syscalls like `mount`, `reboot`, `bpf`.
- **Firejail** — wraps desktop applications in a seccomp sandbox.
- **Android** — uses seccomp on the Zygote and app processes.

### 8.3 An attacker vs seccomp

When exploiting a sandboxed program, the attacker's exploit (ROP chain, shellcode) runs *inside* the sandbox. If the sandbox blocks `execve`, the attacker can't spawn `/bin/sh`. If it blocks `open`, the attacker can't read files.

Bypass techniques:
- **Allowed syscall abuse**: if `mprotect` is allowed, make a page executable and inject shellcode. If `openat` is blocked but `open` isn't (or vice versa), use the other.
- **File descriptor reuse**: if the sandbox already has an open socket fd, the attacker might not need `socket` or `connect` — just `read`/`write` on the existing fd.
- **Kernel bugs**: exploit a kernel vulnerability from inside the sandbox to escape to kernel mode.
- **Confused deputy**: make a privileged process (the broker) do the dangerous operation on the attacker's behalf.

### 8.4 Detecting seccomp in a binary

```sh
# Check if the binary imports seccomp-related functions:
readelf -s target | grep -i seccomp
nm -D target | grep -i seccomp

# Check for prctl(PR_SET_SECCOMP, ...) in strace output:
strace ./target 2>&1 | grep -i seccomp
strace ./target 2>&1 | grep prctl
```

---

## 9. Anti-debugging and anti-analysis techniques

Malware and some commercial software actively resist analysis. Here are common techniques and how to spot them:

### 9.1 `ptrace(PTRACE_TRACEME)` self-tracing

A process can trace itself. Since only one tracer can attach at a time, a subsequent `gdb` attachment will fail.

```c
if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1) {
    // A debugger is already attached
    exit(1);
}
```

**Detection**: look for `ptrace` in `.dynsym`. **Bypass**: NOP out the `ptrace` call, or use `LD_PRELOAD` to hook `ptrace` and always return 0.

### 9.2 Timing checks

The program measures how long a block of code takes. Under a debugger (single-stepping), it takes orders of magnitude longer.

```c
struct timespec t1, t2;
clock_gettime(CLOCK_MONOTONIC, &t1);
// ... sensitive code ...
clock_gettime(CLOCK_MONOTONIC, &t2);
if (t2.tv_nsec - t1.tv_nsec > threshold) abort();  // debugger detected
```

**Bypass**: hook `clock_gettime`, or patch out the check.

### 9.3 `/proc/self/status` inspection

```c
FILE *f = fopen("/proc/self/status", "r");
// Search for "TracerPid: <nonzero>" — means a debugger is attached
```

**Bypass**: hook `fopen`/`fread`, or mount a fake `/proc`.

### 9.4 Stripped + statically linked

Removing symbols and statically linking makes reverse engineering harder: no function names, no library calls to recognize patterns from, much larger binary to analyze.

**Countermeasure**: use Ghidra or IDA's signature-matching (FLIRT for IDA, Function ID for Ghidra) to identify known library functions by their byte patterns even without symbols.

---

## 10. Security tools reference

| Tool | Purpose | Install |
|---|---|---|
| `checksec` | One-shot hardening check (PIE, NX, RELRO, canary, FORTIFY) | `apt install checksec` or from [github](https://github.com/slimm609/checksec.sh) |
| `pwntools` | Python exploit development framework (ROP chains, shellcode, I/O) | `pip install pwntools` |
| `ROPgadget` | Find ROP gadgets in a binary | `pip install ROPgadget` |
| `ropper` | Similar to ROPgadget with more search features | `pip install ropper` |
| `Ghidra` | Free disassembler / decompiler from NSA | [ghidra-sre.org](https://ghidra-sre.org) |
| `radare2` / `rizin` | Command-line disassembler / debugger | `apt install radare2` |
| `pwndbg` | GDB plugin for exploit development | [github](https://github.com/pwndbg/pwndbg) |
| `GEF` | Alternative GDB plugin | [github](https://github.com/hugsy/gef) |
| `LIEF` | Python library for parsing and patching ELF/PE/Mach-O | `pip install lief` |
| `patchelf` | Modify ELF metadata (interpreter, RPATH, NEEDED) | `apt install patchelf` |
| `seccomp-tools` | Dump and analyze seccomp-bpf filters | `gem install seccomp-tools` |
| `one_gadget` | Find "magic gadgets" in libc (one-shot `execve("/bin/sh")`) | `gem install one_gadget` |
| `strace` / `ltrace` | Trace syscalls / library calls (you're building these in steps 2/3!) | `apt install strace ltrace` |

---

## 11. Practice resources

| Resource | What it is | URL |
|---|---|---|
| **picoCTF** | Beginner-friendly CTF with binary exploitation challenges | https://picoctf.org |
| **pwnable.kr** | Classic pwn challenges, escalating difficulty | http://pwnable.kr |
| **pwnable.tw** | Harder pwn challenges | https://pwnable.tw |
| **ROP Emporium** | Focused ROP training — one technique per challenge | https://ropemporium.com |
| **Nightmare** | Guided binary exploitation course with walkthroughs | https://guyinatuxedo.github.io |
| **LiveOverflow** | YouTube channel covering binary exploitation, CTFs | https://www.youtube.com/liveoverflow |
| **Exploit Education** | VMs with progressive exploitation exercises | https://exploit.education |

---

## 12. Connecting back to this project

This doc gave you the security lens. Here's how it connects to each step:

| Project step | Security relevance |
|---|---|
| **Step 1** (this step) | You can now assess a binary's hardening posture from `mini-readelf` output alone: PIE, NX, RELRO level, canaries, CET. |
| **Step 2** (mini-strace) | Your syscall tracer will let you watch a process's interaction with the kernel — the same view a sandbox (seccomp) filters. You'll see `execve`, `open`, `mmap`, `mprotect` — each one is a potential security boundary. |
| **Step 3** (library tracer) | Your `LD_PRELOAD` shim is literally the same mechanism used for hooking/bypassing functions in offensive security (e.g., `LD_PRELOAD` to bypass `ptrace` anti-debugging). Your PLT-patching tracer uses the same technique as breakpoint-based exploitation tools. |

The tools you're building in this project are, in a real sense, the same tools security researchers use daily. `strace` is used for malware analysis. `LD_PRELOAD` is used for both hooking (offensive) and instrumentation (defensive). ELF parsing is the foundation of every reverse engineering tool. You're not building "learning toys" — you're building simplified versions of real security tools.

---

## Further reading

- **"Hacking: The Art of Exploitation" by Jon Erickson** — the classic intro to binary exploitation
- **"Practical Binary Analysis" by Dennis Andriesse** — ELF internals + reverse engineering + instrumentation
- **"The Shellcoder's Handbook"** — older but foundational on shellcode and exploitation techniques
- **Phrack magazine** — historical exploit techniques and research papers
- **Trail of Bits blog** — modern binary analysis and security research
- **System V AMD64 ABI** — the register conventions that ROP chains exploit
- **Linux kernel seccomp source** (`kernel/seccomp.c`) — how syscall filtering actually works
