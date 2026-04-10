/*
 * hello-dynamic.c — a minimal C program, built dynamically linked.
 *
 * When compiled with `gcc hello-dynamic.c -o hello-dynamic`, this becomes
 * a position-independent executable (ET_DYN) that depends on libc.so.6 at
 * run time. The dynamic linker /lib64/ld-linux-x86-64.so.2 will load libc
 * for us before main runs.
 *
 * Inspect the resulting binary with:
 *   ../src/mini-readelf hello-dynamic
 *
 * and compare against hello-static, which carries everything inside it.
 */

#include <stdio.h>

int main(void) {
    puts("hello, dynamic");
    return 0;
}
