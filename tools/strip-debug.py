#!/usr/bin/env python3
"""Copy an ELF32 firmware image without its DWARF.

    tools/strip-debug.py IN.elf OUT.elf

Drops the .debug_* sections (and their .rel/.rela sections) and keeps the
rest — every loadable byte where it was, the symbol table the simulator
looks symbols up in, the section names.  For the browser build: the site
serves its demos' firmware without the debug information the bundled
images carry for overlays (tools/build-device-firmware.sh --debug), which
the page has no use for.  Pure Python, so it reads MSP430 images as well
as ARM ones, whatever binutils is installed.  A file that is not ELF32, or
has no DWARF, is copied unchanged.  tools/check-same-program.py IN OUT
then says the program is the same.
"""
import shutil
import struct
import sys

SHT_SYMTAB, SHT_REL, SHT_RELA, SHT_DYNSYM = 2, 9, 4, 11
SHN_LORESERVE = 0xFF00


def main(src, dst):
    data = open(src, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 1:          # ELF32 only
        shutil.copyfile(src, dst)
        return
    end = '<' if data[5] == 1 else '>'
    eh = list(struct.unpack_from(end + '16sHHIIIIIHHHHHH', data, 0))
    shoff, shentsize, shnum, shstrndx = eh[6], eh[11], eh[12], eh[13]
    shs = [list(struct.unpack_from(end + 'IIIIIIIIII', data, shoff + i * shentsize)) for i in range(shnum)]
    names = shs[shstrndx]
    def name(sh):
        o = names[4] + sh[0]
        return data[o:data.index(b'\0', o)].decode('latin-1')
    drop = set()
    for i, sh in enumerate(shs):
        n = name(sh)
        if n.startswith('.debug') or (sh[1] in (SHT_REL, SHT_RELA) and name(shs[sh[7]]).startswith('.debug')):
            drop.add(i)
    if not drop:
        shutil.copyfile(src, dst)
        return
    if shstrndx in drop:
        sys.exit("strip-debug: the section-name table is a debug section?")
    remap = {}
    for i in range(shnum):
        if i not in drop:
            remap[i] = len(remap)
    # Everything up to the first dropped section's data stays where it was
    # (the program headers and every loadable byte are there); the kept
    # sections after that point move down, packed at their alignment.
    cut = min(shs[i][4] for i in drop)
    out = bytearray(data[:cut])
    kept = []
    for i, sh in enumerate(shs):
        if i in drop:
            continue
        sh = list(sh)
        if i and sh[1] != 8 and sh[4] >= cut:            # file data past the cut: move it
            align = max(sh[8], 1)
            out += b'\0' * ((-len(out)) % align)
            body = bytearray(data[sh[4]:sh[4] + sh[5]])
            if sh[1] in (SHT_SYMTAB, SHT_DYNSYM):        # section indices in st_shndx
                for k in range(0, len(body), 16):
                    ndx = struct.unpack_from(end + 'H', body, k + 14)[0]
                    if 0 < ndx < SHN_LORESERVE:
                        struct.pack_into(end + 'H', body, k + 14, remap.get(ndx, 0))
            sh[4] = len(out)
            out += body
        if sh[6]:                                        # sh_link: always a section index
            sh[6] = remap.get(sh[6], 0)
        if sh[1] in (SHT_REL, SHT_RELA) and sh[7] in remap:
            sh[7] = remap[sh[7]]                         # sh_info: the section relocated
        kept.append(sh)
    out += b'\0' * ((-len(out)) % 4)
    eh[6] = len(out)
    eh[12] = len(kept)
    eh[13] = remap[shstrndx]
    for sh in kept:
        out += struct.pack(end + 'IIIIIIIIII', *sh)
    struct.pack_into(end + '16sHHIIIIIHHHHHH', out, 0, *eh)
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__.split('\n\n')[1])
    main(sys.argv[1], sys.argv[2])
