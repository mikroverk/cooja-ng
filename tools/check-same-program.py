#!/usr/bin/env python3
"""Do two firmware ELFs load the same program?

    tools/check-same-program.py OLD.elf NEW.elf

Compares every allocated section (name, address, size, and the bytes unless
it is NOBITS), the entry point, the symbol table's function and object
addresses and sizes, and every loadable segment (addresses, sizes, flags
and the file bytes the simulator loads); debug sections and the section
table may differ.  Exit 0 if the program is the same.

A segment can cover the ELF header itself: an MSP430 (Sky) image's first
PT_LOAD starts at file offset 0, so its header and program headers are
loaded below the code (at 0x3f2c, under the F1611's flash, where the chip
has no memory).  Adding or dropping DWARF moves the section table, so
e_shoff, e_shnum and e_shstrndx change there.  Those bytes alone may
differ: the check names them and their load addresses, and still says
SAME PROGRAM.  Any other loaded byte that differs makes it DIFFERENT.

What it is for: replacing a bundled image with a rebuild that carries DWARF
(tools/build-device-firmware.sh --debug, at the commit and toolchain the
image was built with) must not change any simulation.  -g changes no code,
and this checks that it did not.  Needs pyelftools.
"""
import sys
from elftools.elf.elffile import ELFFile


def section_table_fields(e):
    """File offsets of e_shoff, e_shnum and e_shstrndx in the ELF header."""
    if e.elfclass == 32:
        return set(range(32, 36)) | set(range(48, 52))
    return set(range(40, 48)) | set(range(60, 64))


def load(p):
    e = ELFFile(open(p, 'rb'))
    secs = {}
    for s in e.iter_sections():
        if s['sh_flags'] & 0x2 and s['sh_size']:
            secs[s.name] = (s['sh_addr'], s['sh_size'], None if s['sh_type'] == 'SHT_NOBITS' else s.data())
    syms = {}
    st = e.get_section_by_name('.symtab')
    for y in (st.iter_symbols() if st else []):
        if y['st_info']['type'] in ('STT_OBJECT', 'STT_FUNC') and y.name:
            syms.setdefault(y.name, (y['st_value'], y['st_size']))
    segs = [(s['p_vaddr'], s['p_paddr'], s['p_filesz'], s['p_memsz'], s['p_flags'], s['p_offset'], s.data())
            for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    return secs, syms, e.header['e_entry'], segs, section_table_fields(e)


def compare_segments(a, b):
    """(differences, header notes) between two images' PT_LOAD segments."""
    bad, notes = [], []
    if len(a[3]) != len(b[3]):
        return [f"{len(a[3])} vs {len(b[3])} PT_LOAD segments"], notes
    for i, (sa, sb) in enumerate(zip(a[3], b[3])):
        if sa[:5] != sb[:5]:
            bad.append(f"segment {i} at {sa[0]:#x}: addresses/sizes/flags differ")
            continue
        da, db = sa[6], sb[6]
        hdr = []
        for k in range(len(da)):
            if da[k] == db[k]:
                continue
            if sa[5] + k in a[4] and sb[5] + k in b[4]:
                hdr.append(sa[1] + k)
            else:
                bad.append(f"segment {i}: byte at {sa[1] + k:#x} differs")
                break
        if hdr:
            notes.append(f"segment {i} loads the ELF header at {sa[1]:#x}; its section-table "
                         f"fields differ at {', '.join(f'{x:#x}' for x in hdr)}")
    return bad, notes


a, b = load(sys.argv[1]), load(sys.argv[2])
bad = [k for k in set(a[0]) | set(b[0]) if a[0].get(k) != b[0].get(k)]
diff = [k for k in set(a[1]) | set(b[1]) if a[1].get(k) != b[1].get(k)]
segbad, notes = compare_segments(a, b)
ok = not bad and not diff and not segbad and a[2] == b[2]
print(f"{'SAME PROGRAM' if ok else 'DIFFERENT'}: {len(a[0])} alloc sections, differing {bad}; "
      f"symbols differing {len(diff)} {sorted(diff)[:5]}; entry {a[2]:#x}/{b[2]:#x}; "
      f"{len(a[3])} load segments, differing {segbad}")
for n in notes:
    print(f"  note: {n}")
sys.exit(0 if ok else 1)
