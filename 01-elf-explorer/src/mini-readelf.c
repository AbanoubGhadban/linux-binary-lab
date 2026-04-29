/*
 * mini-readelf.c — a minimal ELF64 parser for learning the ELF format.
 *
 * This is NOT a replacement for binutils `readelf`. It is intentionally
 * small and heavily commented so that reading this file teaches you the
 * format itself. It supports only:
 *
 *   - ELF class:    ELF64  (the `Elf64_*` structs from <elf.h>)
 *   - Data order:   little-endian
 *   - Machine:      x86-64 (EM_X86_64) — the validation is soft; we'll
 *                   happily parse other architectures but we only decode
 *                   the machine name for x86-64.
 *
 * What it prints, in order:
 *
 *   1. The ELF header  (what kind of file is this, where is the entry?)
 *   2. The program headers  (what does the kernel need to know to load it?)
 *   3. The PT_INTERP string if present  (which dynamic linker runs it?)
 *   4. The .dynamic section  (what libraries does it need, and where is
 *      the string/symbol table?)
 *   5. The dynamic symbol table  (what symbols are resolved at run time?)
 *
 * Build:   gcc -std=c11 -O2 -Wall -Wextra mini-readelf.c -o mini-readelf
 * Usage:   ./mini-readelf <elf-file>
 *
 * Layout of an ELF file as we'll see it:
 *
 *   +---------------------+  <- offset 0
 *   |   Elf64_Ehdr        |      "What kind of ELF is this? Where's everything?"
 *   +---------------------+
 *   |   Elf64_Phdr[]      |      Program headers (runtime view) at e_phoff
 *   +---------------------+
 *   |   ... file body ...  |      segments (PT_LOAD), the interpreter string,
 *   |                      |      the .dynamic array, string tables, code, data
 *   +---------------------+
 *   |   Elf64_Shdr[]      |      Section headers (link-time view) at e_shoff
 *   +---------------------+
 *
 * Two views of the same bytes:
 *   - Program headers describe *segments*: what the KERNEL maps into memory.
 *   - Section headers describe *sections*: what the LINKER cared about when
 *     it built the file. They can be stripped without breaking execution.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Decoding helpers — turn raw numeric constants into readable names */
/* ------------------------------------------------------------------ */

static const char *e_type_str(uint16_t t) {
    switch (t) {
    case ET_NONE: return "NONE";
    case ET_REL:  return "REL (relocatable object file, .o)";
    case ET_EXEC: return "EXEC (classic non-PIE executable)";
    case ET_DYN:  return "DYN  (shared object OR position-independent executable)";
    case ET_CORE: return "CORE (core dump)";
    default:      return "?";
    }
}

static const char *e_machine_str(uint16_t m) {
    switch (m) {
    case EM_NONE:    return "none";
    case EM_386:     return "i386";
    case EM_X86_64:  return "x86-64";
    case EM_ARM:     return "arm";
    case EM_AARCH64: return "aarch64";
    case EM_RISCV:   return "risc-v";
    default:         return "?";
    }
}

static const char *p_type_str(uint32_t t) {
    switch (t) {
    case PT_NULL:         return "NULL";
    case PT_LOAD:         return "LOAD";          /* a segment to map into memory */
    case PT_DYNAMIC:      return "DYNAMIC";       /* points at the .dynamic array */
    case PT_INTERP:       return "INTERP";        /* path to dynamic linker */
    case PT_NOTE:         return "NOTE";          /* build-id and other metadata */
    case PT_SHLIB:        return "SHLIB";         /* reserved, unused */
    case PT_PHDR:         return "PHDR";          /* self-ref: program header table */
    case PT_TLS:          return "TLS";           /* thread-local storage template */
    case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";  /* unwind info for exceptions */
    case PT_GNU_STACK:    return "GNU_STACK";     /* stack permission marker */
    case PT_GNU_RELRO:    return "GNU_RELRO";     /* read-only-after-reloc range */
#ifdef PT_GNU_PROPERTY
    case PT_GNU_PROPERTY: return "GNU_PROPERTY";  /* CET / shadow stack markers */
#endif
    default:              return "?";
    }
}

/*
 * Tags in the .dynamic array tell the dynamic linker what to do.
 * This is a subset — enough to parse a typical glibc program.
 */
static const char *d_tag_str(int64_t tag) {
    switch (tag) {
    case DT_NULL:         return "NULL";       /* terminator */
    case DT_NEEDED:       return "NEEDED";     /* a shared library dependency */
    case DT_PLTRELSZ:     return "PLTRELSZ";
    case DT_PLTGOT:       return "PLTGOT";     /* the GOT for the PLT */
    case DT_HASH:         return "HASH";
    case DT_STRTAB:       return "STRTAB";     /* dynamic string table base */
    case DT_SYMTAB:       return "SYMTAB";     /* dynamic symbol table base */
    case DT_RELA:         return "RELA";       /* non-PLT relocations (RELA) */
    case DT_RELASZ:       return "RELASZ";
    case DT_RELAENT:      return "RELAENT";
    case DT_STRSZ:        return "STRSZ";
    case DT_SYMENT:       return "SYMENT";
    case DT_INIT:         return "INIT";       /* _init() — legacy ctor */
    case DT_FINI:         return "FINI";       /* _fini() — legacy dtor */
    case DT_SONAME:       return "SONAME";     /* this .so's own name */
    case DT_RPATH:        return "RPATH";      /* deprecated library search path */
    case DT_SYMBOLIC:     return "SYMBOLIC";
    case DT_REL:          return "REL";
    case DT_RELSZ:        return "RELSZ";
    case DT_RELENT:       return "RELENT";
    case DT_PLTREL:       return "PLTREL";
    case DT_DEBUG:        return "DEBUG";      /* filled in at runtime by ld.so */
    case DT_TEXTREL:      return "TEXTREL";
    case DT_JMPREL:       return "JMPREL";     /* PLT relocations */
    case DT_BIND_NOW:     return "BIND_NOW";
    case DT_INIT_ARRAY:   return "INIT_ARRAY"; /* modern ctor array */
    case DT_FINI_ARRAY:   return "FINI_ARRAY"; /* modern dtor array */
    case DT_INIT_ARRAYSZ: return "INIT_ARRAYSZ";
    case DT_FINI_ARRAYSZ: return "FINI_ARRAYSZ";
    case DT_RUNPATH:      return "RUNPATH";    /* library search path */
    case DT_FLAGS:        return "FLAGS";
    case DT_GNU_HASH:     return "GNU_HASH";   /* the modern hash table */
    case DT_VERSYM:       return "VERSYM";
    case DT_RELACOUNT:    return "RELACOUNT";
    case DT_RELCOUNT:     return "RELCOUNT";
    case DT_FLAGS_1:      return "FLAGS_1";
    case DT_VERDEF:       return "VERDEF";
    case DT_VERDEFNUM:    return "VERDEFNUM";
    case DT_VERNEED:      return "VERNEED";    /* symbol version requirements */
    case DT_VERNEEDNUM:   return "VERNEEDNUM";
    default:              return NULL;         /* NULL => print raw hex */
    }
}

static const char *sym_bind_str(unsigned char info) {
    switch (ELF64_ST_BIND(info)) {
    case STB_LOCAL:  return "LOCAL";
    case STB_GLOBAL: return "GLOBAL";
    case STB_WEAK:   return "WEAK";
    default:         return "?";
    }
}

static const char *sym_type_str(unsigned char info) {
    switch (ELF64_ST_TYPE(info)) {
    case STT_NOTYPE:  return "NOTYPE";
    case STT_OBJECT:  return "OBJECT";
    case STT_FUNC:    return "FUNC";
    case STT_SECTION: return "SECTION";
    case STT_FILE:    return "FILE";
    case STT_COMMON:  return "COMMON";
    case STT_TLS:     return "TLS";
    case STT_GNU_IFUNC: return "IFUNC";
    default:          return "?";
    }
}

/* Print the three permission bits as an rwx triple (capital E = execute). */
static void print_pflags(uint32_t flags) {
    putchar(flags & PF_R ? 'R' : ' ');
    putchar(flags & PF_W ? 'W' : ' ');
    putchar(flags & PF_X ? 'E' : ' ');
}

/*
 * Translate a virtual address (as it appears inside .dynamic entries like
 * DT_STRTAB) into a pointer into our mmap'd file. We do this by finding the
 * PT_LOAD segment that contains that vaddr and adding the file offset.
 *
 * This is the same job the kernel does in reverse at load time: it takes a
 * file offset (from a PT_LOAD) and maps it at a virtual address.
 */
static const unsigned char *vaddr_to_filebuf(const unsigned char *base,
                                              const Elf64_Phdr *ph, int phnum,
                                              uint64_t vaddr) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_filesz) {
            return base + ph[i].p_offset + (vaddr - ph[i].p_vaddr);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*                                main                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <elf-file>\n", argv[0]);
        return 2;
    }

    /* --- mmap the whole file read-only so we can just cast pointers --- */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return 1; }
    if (st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "file too small to be an ELF\n");
        close(fd); return 1;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    close(fd);  /* fd is no longer needed after mmap */

    const unsigned char *base = map;

    /* --- validate ELF magic --- */
    /* The first four bytes of every ELF file are 0x7f 'E' 'L' 'F'. */
    if (base[EI_MAG0] != ELFMAG0 ||
        base[EI_MAG1] != ELFMAG1 ||
        base[EI_MAG2] != ELFMAG2 ||
        base[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "not an ELF file (bad magic)\n");
        munmap(map, st.st_size);
        return 1;
    }

    if (base[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "only ELF64 is supported (file is %s)\n",
                base[EI_CLASS] == ELFCLASS32 ? "ELF32" : "unknown");
        munmap(map, st.st_size);
        return 1;
    }
    if (base[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "only little-endian ELF is supported\n");
        munmap(map, st.st_size);
        return 1;
    }

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;

    /* ------------------------------------------------------------ */
    /*                       1. ELF header                          */
    /* ------------------------------------------------------------ */
    /*
     * The header is a small fixed-size struct at offset 0. It tells the
     * loader the file's architecture, type (executable / shared / object),
     * where the program and section headers live, and — crucially — the
     * entry point: the virtual address the kernel/loader jumps to once
     * everything is mapped. For dynamically-linked programs, that entry
     * point is inside ld.so, not in your program. For static programs it's
     * `_start` in your own binary.
     */
    printf("ELF Header\n");
    printf("  Magic:                 ");
    for (int i = 0; i < EI_NIDENT; i++) printf("%02x ", eh->e_ident[i]);
    printf("\n");
    printf("  Class:                 ELF64\n");
    printf("  Data:                  little-endian\n");
    printf("  OS/ABI:                %u\n", eh->e_ident[EI_OSABI]);
    printf("  Type:                  0x%x  %s\n", eh->e_type, e_type_str(eh->e_type));
    printf("  Machine:               0x%x  %s\n", eh->e_machine, e_machine_str(eh->e_machine));
    printf("  Version:               0x%x\n", eh->e_version);
    printf("  Entry point address:   0x%" PRIx64 "\n", eh->e_entry);
    printf("  Program header offset: %" PRIu64 " (0x%" PRIx64 ")\n",
           eh->e_phoff, eh->e_phoff);
    printf("  Section header offset: %" PRIu64 " (0x%" PRIx64 ")\n",
           eh->e_shoff, eh->e_shoff);
    printf("  Flags:                 0x%x\n", eh->e_flags);
    printf("  Header size:           %u bytes\n", eh->e_ehsize);
    printf("  Program header entry:  %u bytes x %u entries\n",
           eh->e_phentsize, eh->e_phnum);
    printf("  Section header entry:  %u bytes x %u entries\n",
           eh->e_shentsize, eh->e_shnum);
    printf("  Section name strndx:   %u\n", eh->e_shstrndx);
    printf("\n");

    if (eh->e_type == ET_DYN) {
        printf("  Note: e_type == ET_DYN.\n");
        printf("        This is either a shared library (.so) or a PIE (position-\n");
        printf("        independent executable). e_entry (0x%" PRIx64 ") is a file-relative\n",
               eh->e_entry);
        printf("        offset; the real runtime address depends on where the kernel\n");
        printf("        mapped the binary (ASLR chooses a random base).\n\n");
    }

    /* ------------------------------------------------------------ */
    /*                    2. Program headers                        */
    /* ------------------------------------------------------------ */
    /*
     * The program header table is an array of Elf64_Phdr starting at
     * e_phoff. Each entry describes a *segment* — a contiguous chunk of the
     * file that the kernel should either map into memory (PT_LOAD), or
     * interpret specially (PT_INTERP, PT_DYNAMIC, PT_NOTE, ...).
     *
     * The kernel's job during execve() is essentially:
     *   for each PT_LOAD: mmap p_filesz bytes at p_vaddr with p_flags rights
     *   if PT_INTERP:    load the named dynamic linker and give it control
     *   else:            jump straight to e_entry in the binary
     */
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        printf("No program headers present.\n\n");
    }

    const Elf64_Phdr *ph = NULL;
    const Elf64_Phdr *pt_interp = NULL;
    const Elf64_Phdr *pt_dynamic = NULL;

    if (eh->e_phoff && eh->e_phnum) {
        ph = (const Elf64_Phdr *)(base + eh->e_phoff);

        printf("Program Headers (%u entries):\n", eh->e_phnum);
        printf("  Idx Type          Flags Offset         VirtAddr       FileSize       MemSize        Align\n");
        for (int i = 0; i < eh->e_phnum; i++) {
            printf("  %3d %-13s ", i, p_type_str(ph[i].p_type));
            print_pflags(ph[i].p_flags);
            printf(" 0x%012" PRIx64 " 0x%012" PRIx64 " 0x%012" PRIx64 " 0x%012" PRIx64 " 0x%" PRIx64 "\n",
                   ph[i].p_offset, ph[i].p_vaddr, ph[i].p_filesz,
                   ph[i].p_memsz, ph[i].p_align);

            if (ph[i].p_type == PT_INTERP)  pt_interp  = &ph[i];
            if (ph[i].p_type == PT_DYNAMIC) pt_dynamic = &ph[i];
        }
        printf("\n");
    }

    /* ------------------------------------------------------------ */
    /*                    3. The interpreter                        */
    /* ------------------------------------------------------------ */
    /*
     * If PT_INTERP is present, its p_offset points at a NUL-terminated
     * string: the path to the dynamic linker. On glibc/amd64 systems that
     * string is /lib64/ld-linux-x86-64.so.2. The *kernel* loads that path
     * as a fresh ELF first, and transfers control to ld.so's entry point.
     * ld.so then loads the original program and its DT_NEEDED libraries,
     * resolves symbols, and jumps to the program's own _start.
     *
     * Static binaries have no PT_INTERP. The kernel loads them directly.
     */
    if (pt_interp) {
        const char *interp_path = (const char *)(base + pt_interp->p_offset);
        printf("Interpreter (PT_INTERP):\n");
        printf("  path: %s\n", interp_path);
        printf("  -> kernel will execve this program first; it is the dynamic\n");
        printf("     linker, and it is responsible for loading our binary and\n");
        printf("     its shared libraries before running main.\n\n");
    } else if (ph) {
        printf("No PT_INTERP segment.\n");
        printf("  -> this looks like a statically-linked binary; the kernel will\n");
        printf("     jump straight to e_entry after mapping the PT_LOAD segments.\n\n");
    }

    /* ------------------------------------------------------------ */
    /*                    4. The .dynamic array                     */
    /* ------------------------------------------------------------ */
    /*
     * The .dynamic section is an array of (tag, value) pairs that tells the
     * dynamic linker everything it needs: which libraries to load, where
     * the string table is, where the symbol table is, where the relocations
     * are, and so on. It's terminated by an entry with tag DT_NULL.
     *
     * To pretty-print DT_NEEDED (the list of required shared libraries) we
     * need to resolve the string offset it stores against the dynamic
     * string table. The string table is itself pointed at by a DT_STRTAB
     * entry — as a *virtual address*, not a file offset. So we have to
     * walk .dynamic twice: first to find DT_STRTAB, then to interpret the
     * rest.
     */
    if (pt_dynamic) {
        const Elf64_Dyn *d = (const Elf64_Dyn *)(base + pt_dynamic->p_offset);
        int max_entries = pt_dynamic->p_filesz / sizeof(Elf64_Dyn);

        /* First pass: locate the dynamic string table and note its size. */
        uint64_t strtab_vaddr = 0;
        uint64_t strtab_size  = 0;
        for (int i = 0; i < max_entries; i++) {
            if (d[i].d_tag == DT_NULL) break;
            if (d[i].d_tag == DT_STRTAB) strtab_vaddr = d[i].d_un.d_ptr;
            if (d[i].d_tag == DT_STRSZ)  strtab_size  = d[i].d_un.d_val;
        }

        const char *strtab = NULL;
        if (strtab_vaddr) {
            strtab = (const char *)vaddr_to_filebuf(base, ph, eh->e_phnum,
                                                    strtab_vaddr);
        }

        printf("Dynamic section (.dynamic):\n");
        printf("  Idx Tag             Value\n");
        for (int i = 0; i < max_entries; i++) {
            const char *tag_name = d_tag_str(d[i].d_tag);
            printf("  %3d ", i);
            if (tag_name) {
                printf("%-15s ", tag_name);
            } else {
                printf("0x%-13" PRIx64 " ", (uint64_t)d[i].d_tag);
            }

            /* Pretty-print the interesting tags by looking up the string table. */
            switch (d[i].d_tag) {
            case DT_NEEDED:
                printf("shared library -> %s\n",
                       strtab ? strtab + d[i].d_un.d_val : "?");
                break;
            case DT_SONAME:
                printf("own soname     -> %s\n",
                       strtab ? strtab + d[i].d_un.d_val : "?");
                break;
            case DT_RPATH:
            case DT_RUNPATH:
                printf("search path    -> %s\n",
                       strtab ? strtab + d[i].d_un.d_val : "?");
                break;
            case DT_STRTAB:
            case DT_SYMTAB:
            case DT_HASH:
            case DT_GNU_HASH:
            case DT_PLTGOT:
            case DT_RELA:
            case DT_JMPREL:
            case DT_INIT:
            case DT_FINI:
            case DT_INIT_ARRAY:
            case DT_FINI_ARRAY:
            case DT_VERSYM:
            case DT_VERNEED:
            case DT_VERDEF:
                printf("vaddr 0x%" PRIx64 "\n", d[i].d_un.d_ptr);
                break;
            case DT_STRSZ:
            case DT_SYMENT:
            case DT_RELASZ:
            case DT_RELAENT:
            case DT_PLTRELSZ:
            case DT_INIT_ARRAYSZ:
            case DT_FINI_ARRAYSZ:
            case DT_RELACOUNT:
            case DT_RELCOUNT:
            case DT_VERDEFNUM:
            case DT_VERNEEDNUM:
                printf("%" PRIu64 "\n", d[i].d_un.d_val);
                break;
            case DT_NULL:
                printf("(end of .dynamic)\n");
                break;
            default:
                printf("0x%" PRIx64 "\n", d[i].d_un.d_val);
                break;
            }

            if (d[i].d_tag == DT_NULL) break;
        }
        if (strtab && strtab_size) {
            printf("  (string table: %" PRIu64 " bytes at vaddr 0x%" PRIx64 ")\n",
                   strtab_size, strtab_vaddr);
        }
        printf("\n");
    }

    /* ------------------------------------------------------------ */
    /*                 5. Dynamic symbol table                      */
    /* ------------------------------------------------------------ */
    /*
     * .dynsym is the table of symbols the dynamic linker cares about:
     * things you import from libraries (like `puts`) and things you export
     * for others to import. We find it via the section headers because
     * that's the simplest way to bound it — .dynsym's sh_size directly
     * tells us how many entries it has. The names come from .dynstr.
     *
     * A "stripped" binary has its section headers removed (or its regular
     * .symtab / .strtab removed), but .dynsym/.dynstr usually remain
     * because the dynamic linker needs them at run time. If both are gone,
     * we fall back to "no symbols visible".
     */
    if (eh->e_shoff && eh->e_shnum) {
        const Elf64_Shdr *sh = (const Elf64_Shdr *)(base + eh->e_shoff);
        const char *shstrtab = NULL;
        if (eh->e_shstrndx < eh->e_shnum) {
            shstrtab = (const char *)(base + sh[eh->e_shstrndx].sh_offset);
        }

        const Elf64_Shdr *dynsym = NULL;
        const Elf64_Shdr *dynstr = NULL;
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_DYNSYM) dynsym = &sh[i];
            if (sh[i].sh_type == SHT_STRTAB && shstrtab &&
                strcmp(shstrtab + sh[i].sh_name, ".dynstr") == 0) {
                dynstr = &sh[i];
            }
        }

        if (dynsym && dynstr) {
            const Elf64_Sym *syms = (const Elf64_Sym *)(base + dynsym->sh_offset);
            const char *strs = (const char *)(base + dynstr->sh_offset);
            int n = dynsym->sh_size / sizeof(Elf64_Sym);

            printf("Dynamic symbol table (.dynsym, %d entries):\n", n);
            printf("  Idx Bind   Type     Value              Name\n");
            int shown = 0;
            const int limit = 40;
            for (int i = 0; i < n; i++) {
                /* Index 0 is the reserved STN_UNDEF — always empty. */
                if (i == 0) continue;
                printf("  %3d %-6s %-8s 0x%016" PRIx64 " %s\n",
                       i,
                       sym_bind_str(syms[i].st_info),
                       sym_type_str(syms[i].st_info),
                       syms[i].st_value,
                       strs + syms[i].st_name);
                if (++shown >= limit && i + 1 < n) {
                    printf("  ... (%d more symbols not shown)\n", n - 1 - shown);
                    break;
                }
            }
            printf("\n");
        } else {
            printf("No .dynsym / .dynstr sections found.\n");
            printf("  -> either the binary is statically linked (no dynamic symbols\n");
            printf("     to resolve at run time), or the section headers have been\n");
            printf("     stripped. Static binaries still run fine without these.\n\n");
        }
    } else {
        printf("No section headers (stripped?).\n\n");
    }

    munmap(map, st.st_size);
    return 0;
}
