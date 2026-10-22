/*
 * embed — turn a file into a C array, so the binary carries it.
 *
 *   embed <file> <symbol>  >  out.c
 *
 * Writes `const unsigned char <symbol>[]` (NUL-terminated, so it can be
 * used as a string) and `const unsigned int <symbol>_len` (without the
 * NUL).  Used for the web UI page (ui/index.html): the simulator then
 * serves it from any directory, and an installed binary needs no data
 * file for it.  A build tool, run on the build host; no xxd needed.
 */
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: embed <file> <symbol>\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    printf("/* Generated from %s by tools/embed/embed.c. Do not edit. */\n", argv[1]);
    printf("const unsigned char %s[] = {\n", argv[2]);
    unsigned long n = 0;
    int c;
    while ((c = fgetc(in)) != EOF) {
        printf("%s0x%02x,", (n % 16) ? "" : "\n  ", c);
        n++;
    }
    printf("\n  0x00\n};\n");
    printf("const unsigned int %s_len = %luu;\n", argv[2], n);
    if (ferror(in)) {
        perror(argv[1]);
        return 1;
    }
    fclose(in);
    return 0;
}
