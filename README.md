# linux-binary-lab

A hands-on exploration of how Linux loads, runs, and links executables — built as a learning project. Each step is an independent subdirectory with its own code, docs, and worked examples.

## Why

You can read a dozen articles about ELF, dynamic linking, and syscalls and still not *feel* how it works. The only way to internalize it is to build the tools that observe it. This repo is three small tools, built in sequence, each one illuminating a different layer of the Linux execution model.

## The three steps

| # | What | Mechanism | Issue |
|---|---|---|---|
| 1 | **ELF explorer** — a mini `readelf` + docs explaining ELF anatomy, static vs dynamic linking, and the entry point | parse ELF64 by hand, produce docs + example outputs | #1 |
| 2 | **Syscall tracer** — a mini `strace` that logs every syscall a binary makes | `ptrace` + `PTRACE_SYSCALL` | #2 |
| 3 | **Library call tracer** — a mini `ltrace` that intercepts shared-library calls | `LD_PRELOAD` (required) + PLT patching via ptrace (stretch) | #3 |

## Layout

```
.
├── README.md                  ← you are here
├── 01-elf-explorer/           ← Step 1 (done first — docs + examples, then a tiny tool)
├── 02-syscall-tracer/         ← Step 2
└── 03-library-tracer/         ← Step 3
```

## Requirements

- Linux x86-64 (kernel ≥ 3.2)
- `gcc` and `make`
- `libc6-dev` (for the static-linking example in step 1, the Ubuntu static libc)
- Knowledge assumptions: basic C, familiarity with the terminal. No prior ELF / linker / ptrace knowledge required — the docs build it up from scratch.

## How to use this repo

Each step directory has its own `README.md` explaining what's there, and its own `Makefile`. Start with step 1 and read the docs in order. The code is intentionally small (each tool is a few hundred lines of C at most) and is meant to be read, not just run.

```sh
cd 01-elf-explorer
make        # build the tool and the examples, generate output snapshots
```

## Non-goals

- Not trying to replace `readelf`, `strace`, or `ltrace`. The real tools are far more powerful and portable.
- Not trying to be a production sandbox. Step 3's library tracer can optionally add `seccomp-bpf` filtering as a stretch goal, but hardened sandboxing is out of scope.
- Not cross-platform. x86-64 Linux only. The whole point is to understand *this* platform deeply.
