# 03 — The entry point: `_start`, `__libc_start_main`, and `main`

> You think your program starts at `main`. It doesn't. Here's what actually runs first, why, and how to see it with your own eyes.

## The uncomfortable truth about `main`

If you've written any C, `main` feels like the beginning of the universe. The program "starts at main". Execution "enters main". Every intro-to-C book starts there.

That's a lie — a useful one, but a lie. When you run a C program on Linux, `main` is the *third* function that runs, and by the time it's called, a non-trivial amount of setup has already happened.

The actual call chain for a dynamically-linked program is:

```
kernel execve()
    └── ld.so's _start
            └── dynamic linking: load libs, apply relocs, run .init_array
                └── AT_ENTRY: jump to the program's own _start
                        └── __libc_start_main  (in libc.so)
                                └── main
                                        └── (your code)
                                └── exit()
```

For a statically-linked program it's almost the same, just without the `ld.so` steps at the top — you go straight from `execve()` into `_start`, because `_start` and `__libc_start_main` are baked into the static binary.

Let's walk each layer.

## `e_entry` points at `_start`

The ELF header field `e_entry` is the virtual address where execution begins — but "begins" here means "after the kernel and `ld.so` have done everything else". It's a virtual address inside your program (or, for a static binary, the same). It is *not* the address of `main`.

Verify with `mini-readelf`:

```
  Entry point address:   0x2190
```

Now match that to a symbol:

```sh
nm hello-dynamic | grep -i ' _start$'
# 0000000000002190 T _start
```

There it is. `e_entry` == address of `_start`.

Who wrote `_start`? It came from `crt1.o` ("C runtime 1") which the compiler silently links into every program. On glibc systems the source is in `sysdeps/x86_64/start.S`, an actual hand-written assembly file. Here's what it does (paraphrased):

```asm
_start:
    endbr64                     # Intel CET landing pad — new hardening
    xor  ebp, ebp               # zero the frame pointer: marks end of stack chain
    mov  r9,  rdx               # r9 = rtld_fini (destructor from ld.so)
    pop  rsi                    # rsi = argc
    mov  rdx, rsp               # rdx = argv
    and  rsp, -16               # 16-byte align the stack
    push rax                    # padding
    push rsp                    # stack_end
    xor  r8d, r8d               # r8 = 0 (fini, legacy)
    xor  ecx, ecx               # rcx = 0 (init, legacy)
    lea  rdi, [rip + main]      # rdi = pointer to main
    call __libc_start_main
    hlt                         # never reached
```

A few things worth noticing:

- **It's tiny.** About 15 instructions. Its job is marshaling arguments onto the stack in the right order and handing off to libc.
- **It puts a pointer to `main` in `rdi`.** That's the first C argument in the System V AMD64 calling convention. So `__libc_start_main` sees `main` as its first parameter.
- **It ends with `hlt`, which is unreachable.** Because `__libc_start_main` never returns — it calls `exit()` internally.
- **The `endbr64` at the top is a security feature** (Intel CET, "indirect branch tracking"). Recent CPUs and kernels use it to reject indirect calls that don't land on a marked instruction.

## `__libc_start_main` — where the real setup happens

`__libc_start_main` lives in `libc.so.6` (or is bundled into static binaries). It's the single biggest piece of program-startup magic on Linux, and its signature looks like this:

```c
int __libc_start_main(
    int (*main)(int, char **, char **),
    int argc,
    char **argv,
    void (*init)(void),     // obsolete — pre-init_array ctor
    void (*fini)(void),     // obsolete — pre-fini_array dtor
    void (*rtld_fini)(void),// unwind ld.so's own cleanup
    void *stack_end);
```

What it does, in order:

1. **Set up the program environment.** Pulls `argv[argc+1]` (that's where `envp` starts on the stack, thanks to the kernel's initial layout) and `environ`, which is where libc stashes the pointer that `getenv(3)` later reads.
2. **Walk the auxiliary vector** to find values like `AT_PAGESIZE`, `AT_RANDOM`, `AT_PLATFORM`.
3. **Set up stack canaries** using the random bytes from `AT_RANDOM`, if `-fstack-protector` is enabled.
4. **Initialize thread-local storage** for the main thread. This is non-trivial — TLS uses `fs` base register setup via `arch_prctl(ARCH_SET_FS, ...)` on x86-64, and `__libc_start_main` (or code it calls) does that via an `arch_prctl` syscall if `ld.so` didn't already.
5. **Register the `rtld_fini` destructor** with `atexit()`, so that `ld.so`'s own cleanup runs when the program exits.
6. **Run constructors.** Anything in `.init_array` (which includes C++ global ctors, `__attribute__((constructor))` functions, and libc's own initializers) runs here. These can even call `printf` — by now libc is fully alive.
7. **Call `main(argc, argv, envp)`.** This is where your code finally runs.
8. **Call `exit(return_value_from_main)`.** Which runs `atexit` handlers, `.fini_array`, flushes stdio, and finally calls the `exit_group` syscall.

Step 6 is when `__attribute__((constructor))` functions fire, which is a useful hook — that's how `LD_PRELOAD` libraries that want to "run before `main`" work.

You can see `__libc_start_main` in your binary's dynamic symbol table — look at your `mini-readelf` output:

```
Dynamic symbol table (.dynsym, 19 entries):
  Idx Bind   Type     Value              Name
    1 GLOBAL FUNC     0x0000000000000000 __libc_start_main
```

Value is `0x0` because it's an *undefined* import — its real address gets filled in by `ld.so` at load time, when `libc.so.6` is mapped.

## What does static look like?

In a static binary, `_start`, `__libc_start_main`, and all of libc are baked into the file itself. So:

- `e_entry` still points at `_start`, just at a fixed address like `0x401650` (classic `ET_EXEC` layout) instead of a small offset like `0x2190`.
- `__libc_start_main` is a regular local symbol, not an import.
- There's no `.dynsym` (nothing to import).
- There's no `ld.so` step at the beginning — the kernel jumps straight to `_start`.

Everything else is the same. The call chain is identical; only the linking of the functions changes.

## Things that run *before* `_start`

For a dynamic program there are two things that run before your binary's `_start`:

1. **`ld.so`'s own `_start`** — the dynamic linker bootstraps itself. It has to, because when the kernel first hands control to it, nothing is resolved yet. Its initial code is typically in `sysdeps/x86_64/dl-machine.h` (glibc) and has to avoid any cross-function calls that would need relocations.
2. **`ld.so`'s main loop** — `_dl_start` → `_dl_start_final` → `dl_main`, which loads all the dependencies, applies relocations, and runs the dependency tree's init functions before finally jumping to `AT_ENTRY` (your program's `_start`).

If you run `LD_DEBUG=all ./hello-dynamic` you'll see all of this narrated in real time. It's worth doing at least once.

## See it for yourself

Run the tool on your hello binaries and match `e_entry` to the disassembly. If you have `objdump`:

```sh
# get the entry point address from mini-readelf
./src/mini-readelf examples/hello-dynamic | grep "Entry point"
# say it's 0x2190

# disassemble around that address
objdump -d examples/hello-dynamic | grep -A 15 "<_start>:"
```

You'll see the exact assembly above, the `lea rdi, [rip+main]`, and the `call` into `__libc_start_main@plt`.

For a static binary:

```sh
./src/mini-readelf examples/hello-static | grep "Entry point"
objdump -d examples/hello-static | grep -A 15 "<_start>:"
```

The assembly is virtually identical — just that `__libc_start_main` is now a local call instead of a PLT call.

## A fun experiment: a program without libc

You can actually write a binary that has no `__libc_start_main`, no libc, just your own `_start`:

```c
// hello-freestanding.c
void _start(void) {
    const char msg[] = "hello, no libc\n";
    // x86-64 write() syscall: rax=1, rdi=fd, rsi=buf, rdx=len
    asm volatile (
        "mov $1, %%rax\n"
        "mov $1, %%rdi\n"
        "mov %0, %%rsi\n"
        "mov $15, %%rdx\n"
        "syscall\n"
        // exit_group(0): rax=231, rdi=0
        "mov $231, %%rax\n"
        "xor %%rdi, %%rdi\n"
        "syscall\n"
        :
        : "r"(msg)
        : "rax", "rdi", "rsi", "rdx"
    );
}

// build: gcc -nostdlib -static hello-freestanding.c -o hello-freestanding
```

This builds to about 13 KB (all boilerplate) and prints "hello, no libc". No `__libc_start_main`, no `main`, no anything. Just `_start`, two inline syscalls, and a `hlt`. The point being: **there is nothing sacred about `main`**. It's just a name that glibc's `__libc_start_main` calls because that's what everyone agreed on. You can name your entry point anything by telling the linker (`ld -e myentry`).

## Key takeaways

- `e_entry` points at `_start`, from `crt1.o`. That's your *first* function.
- `_start` calls `__libc_start_main` (in libc), which calls `main`.
- Before `_start` even runs, for dynamic programs, `ld.so` has already loaded all your libraries, applied relocations, and run any constructors.
- `main` is ordinary. It's the last link in a long chain, not the first.
- You can verify all of this on any binary with `nm`, `objdump -d`, and `strace`.

## Further reading

- glibc source: `sysdeps/x86_64/start.S` (`_start`) and `csu/libc-start.c` (`__libc_start_main`)
- `man 7 feature_test_macros` — background on `__libc_start_main`'s prototype
- "The 101 of ELF files on Linux: Understanding and Analysis" — Michael Boelen
- "Hello from a libc-free world" — Jessica McKellar's classic talk
- Eli Bendersky, "Life of binaries" and "How statically linked programs run on Linux"
