/*
 * hello-static.c — the same program, built fully statically linked.
 *
 * Built with `gcc -static hello-static.c -o hello-static`, this becomes a
 * classic ET_EXEC binary with every bit of libc copied into it. There is
 * no PT_INTERP segment, no DT_NEEDED list, no dynamic symbol resolution.
 * The kernel loads the PT_LOAD segments and jumps straight to `_start` in
 * the binary itself.
 *
 * Expect this file on disk to be dramatically larger than hello-dynamic
 * (~900 KB vs ~16 KB) because it carries its own full libc.
 *
 * Inspect with:
 *   ../src/mini-readelf hello-static
 *
 * Note: the source is identical to hello-dynamic.c. The only difference
 * between the two binaries is the link mode — hence the point of the
 * exercise.
 */

#include <stdio.h>

int main(void) {
    puts("hello, static");
    return 0;
}
