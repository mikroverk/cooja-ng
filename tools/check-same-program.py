#!/usr/bin/env python3
"""Do two firmware ELFs load the same program?

    tools/check-same-program.py OLD.elf NEW.elf

Compares every allocated section (name, address, size, and the bytes unless
it is NOBITS), the entry point, and the symbol table's function and object
addresses and sizes; debug sections, the section table and the ELF header
may differ.  Exit 0 if the program is the same.

What it is for: replacing a bundled image with a rebuild that carries DWARF
(tools/build-device-firmware.sh --debug, at the commit and toolchain the
image was built with) must not change any simulation.  -g changes no code,
and this checks that it did not.  Needs pyelftools.
"""
import sys
from elftools.elf.elffile import ELFFile
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
    return secs, syms, e.get_section_by_name('.debug_info') is not None, e.header['e_entry']
a, b = load(sys.argv[1]), load(sys.argv[2])
bad = [k for k in set(a[0]) | set(b[0]) if a[0].get(k) != b[0].get(k)]
diff = [k for k in set(a[1]) | set(b[1]) if a[1].get(k) != b[1].get(k)]
ok = not bad and not diff and a[3] == b[3]
print(f"{'SAME PROGRAM' if ok else 'DIFFERENT'}: {len(a[0])} alloc sections, differing {bad}; "
      f"symbols differing {len(diff)} {sorted(diff)[:5]}; entry {a[3]:#x}/{b[3]:#x}")
sys.exit(0 if ok else 1)
