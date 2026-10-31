#!/usr/bin/env python3
"""Struct layouts from DWARF, for overlays that read firmware variables.

An overlay (src/services/rpl_overlay_engine.c) reads fields of a firmware
struct out of emulated memory, so it needs their offsets and sizes.  They
are not hard-coded: this script reads them from the DWARF of an unstripped
image and writes rows the overlay picks from at run time.

    tools/gen-struct-layout.py ELF STRUCT FIELD...            # print rows
    tools/gen-struct-layout.py ELF STRUCT:FIELD,FIELD STRUCT:...
    tools/gen-struct-layout.py --update HEADER [--tag TAG] [--name N] ELF [SPEC...]

STRUCT is a typedef (rpl_instance_t) or a struct/class tag (rpl_instance).
FIELD is a member path, dotted for nested members (dag.rank); base classes
are searched (C++).  Every STRUCT also gets a row for its sizeof (field "").
A row is { layout, machine, struct, field, offset, size, bit, bits }: for a
bitfield, size bytes at offset hold it, starting `bit` bits up from the
least significant bit of the first byte, `bits` wide; bits 0 = an ordinary
member.

--update merges the rows into HEADER between its rows markers; with no
SPEC it takes them from the header's "layout-spec:" comment, so the header
says which fields its overlay reads.  An image
whose layout is the same as one already there (same machine, same rows) is
only named in that layout's comment; a different layout becomes a new
group, tagged TAG (default: the image's file name).  At run time the
overlay picks the group by the image's machine and the first STRUCT's
sizeof, compared with the size the symbol table records for the variable,
so an image built with another configuration matches no group and the
overlay says "layout unknown" instead of reading the wrong bytes.

Needs pyelftools (pip install pyelftools).  Exits 3 if the image has no
DWARF (.debug_info): build it with -g, or say why it has no row.
"""
import argparse
import os
import re
import sys

try:
    from elftools.elf.elffile import ELFFile
    from elftools.dwarf import structs as dwarf_structs
except ImportError:
    sys.exit("gen-struct-layout: needs pyelftools (pip install pyelftools)")


def allow_16bit_addresses():
    """pyelftools takes 4- and 8-byte target addresses only, and MSP430's
    are 2 bytes.  Teach DWARFStructs a 2-byte address by rewriting the two
    places that decide it; if this pyelftools reads differently, leave it
    alone (an MSP430 image then fails with pyelftools' own assertion)."""
    import inspect
    import textwrap
    cls = dwarf_structs.DWARFStructs
    subs = {
        "__new__": [("assert address_size == 8 or address_size == 4",
                     "assert address_size in (2, 4, 8)")],
        "_create_structs": [("ULInt32 if self.address_size == 4 else ULInt64",
                             "{2: ULInt16, 4: ULInt32}.get(self.address_size, ULInt64)"),
                            ("UBInt32 if self.address_size == 4 else UBInt64",
                             "{2: UBInt16, 4: UBInt32}.get(self.address_size, UBInt64)")],
    }
    patched = {}
    for name, pairs in subs.items():
        src = textwrap.dedent(inspect.getsource(getattr(cls, name)))
        for old, new in pairs:
            if old not in src:
                return
            src = src.replace(old, new)
        ns = {}
        exec(compile(src.replace("super().__new__(cls)", "object.__new__(cls)"), dwarf_structs.__file__, "exec"),
             vars(dwarf_structs), ns)
        patched[name] = ns[name]
    cls.__new__ = staticmethod(patched["__new__"])
    cls._create_structs = patched["_create_structs"]


allow_16bit_addresses()

BEGIN = "/* rows:begin (tools/gen-struct-layout.py --update) */"
END = "/* rows:end */"
ROW = re.compile(r'^\s*\{ "([^"]*)", (\d+), "([^"]*)", "([^"]*)", (\d+), (\d+)(?:, (\d+), (\d+))? \},\s*$')
GROUP = re.compile(r'^\s*/\* layout (\S+): (.*) \*/\s*$')
SPEC = re.compile(r'layout-spec: (.*?)\s*\*/')


def strip_type(die):
    """Follow typedef/const/volatile to the underlying type DIE."""
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type",
                                          "DW_TAG_volatile_type", "DW_TAG_restrict_type"):
        if "DW_AT_type" not in die.attributes:
            return None
        die = die.get_DIE_from_attribute("DW_AT_type")
    return die


def type_size(die, addr_size):
    die = strip_type(die)
    if die is None:
        return 0
    if "DW_AT_byte_size" in die.attributes:
        return die.attributes["DW_AT_byte_size"].value
    if die.tag == "DW_TAG_pointer_type":
        return addr_size
    if die.tag == "DW_TAG_array_type":
        n = 1
        for sub in die.iter_children():
            if sub.tag == "DW_TAG_subrange_type":
                if "DW_AT_count" in sub.attributes:
                    n *= sub.attributes["DW_AT_count"].value
                elif "DW_AT_upper_bound" in sub.attributes:
                    n *= sub.attributes["DW_AT_upper_bound"].value + 1
        return n * type_size(die.get_DIE_from_attribute("DW_AT_type"), addr_size)
    return 0


def member_offset(die):
    attr = die.attributes.get("DW_AT_data_member_location")
    if attr is None:
        return 0                      # a union member
    if isinstance(attr.value, int):
        return attr.value
    # An expression: DW_OP_plus_uconst <uleb128> is all compilers emit here.
    v = attr.value
    if v and v[0] == 0x23:
        n, shift = 0, 0
        for b in v[1:]:
            n |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        return n
    raise ValueError(f"unsupported member location {v!r}")


def bitfield(die):
    """(byte offset, bit, bits) of a bitfield member relative to its struct,
    or None for an ordinary member.  The bit is counted from the byte's
    least significant bit, the way a little-endian target stores it."""
    a = die.attributes
    if "DW_AT_bit_size" not in a:
        return None
    bits = a["DW_AT_bit_size"].value
    if "DW_AT_data_bit_offset" in a:                 # DWARF 4 and 5
        pos = a["DW_AT_data_bit_offset"].value
    else:                                            # DWARF 2 and 3: from the unit's MSB
        unit = a["DW_AT_byte_size"].value if "DW_AT_byte_size" in a else \
            type_size(die.get_DIE_from_attribute("DW_AT_type"), 4)
        pos = member_offset(die) * 8 + unit * 8 - a["DW_AT_bit_offset"].value - bits
    return pos // 8, pos % 8, bits


def field_offset(die):
    bf = bitfield(die)
    return bf[0] if bf else member_offset(die)


def find_member(struct, name):
    """(offset, member DIE) of `name` in a struct/class DIE, looking inside
    anonymous members and base classes."""
    for child in struct.iter_children():
        if child.tag == "DW_TAG_member":
            cname = child.attributes.get("DW_AT_name")
            if cname is not None and cname.value.decode() == name:
                return field_offset(child), child
            if cname is None:            # anonymous struct/union
                inner = strip_type(child.get_DIE_from_attribute("DW_AT_type"))
                if inner is not None:
                    hit = find_member(inner, name)
                    if hit:
                        return member_offset(child) + hit[0], hit[1]
        elif child.tag == "DW_TAG_inheritance":
            base = strip_type(child.get_DIE_from_attribute("DW_AT_type"))
            if base is not None:
                hit = find_member(base, name)
                if hit:
                    return member_offset(child) + hit[0], hit[1]
    return None


def find_struct(dwarf, name):
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            n = die.attributes.get("DW_AT_name")
            if n is None or n.value.decode() != name:
                continue
            if die.tag == "DW_TAG_typedef":
                target = strip_type(die)
                if target is not None and target.tag in ("DW_TAG_structure_type", "DW_TAG_class_type") \
                        and "DW_AT_declaration" not in target.attributes:
                    return target, cu.header["address_size"]
            elif die.tag in ("DW_TAG_structure_type", "DW_TAG_class_type") \
                    and "DW_AT_declaration" not in die.attributes:
                return die, cu.header["address_size"]
    return None, 0


def layout(path, specs):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info() or elf.get_section_by_name(".debug_info") is None:
            print(f"gen-struct-layout: {path} has no DWARF (.debug_info); build it with -g", file=sys.stderr)
            sys.exit(3)
        machine = elf.header["e_machine"]
        mnum = {"EM_MSP430": 105, "EM_ARM": 40, "EM_RISCV": 243}.get(machine, machine)
        dwarf = elf.get_dwarf_info()
        rows = []
        for sname, fields in specs:
            sdie, asz = find_struct(dwarf, sname)
            if sdie is None:
                sys.exit(f"gen-struct-layout: no struct {sname} in {path}")
            rows.append((mnum, sname, "", 0, type_size(sdie, asz), 0, 0))
            for field in fields:
                off, cur = 0, sdie
                member = None
                for part in field.split("."):
                    hit = find_member(cur, part)
                    if hit is None:
                        sys.exit(f"gen-struct-layout: no member {field} in {sname} ({path})")
                    off += hit[0]
                    member = hit[1]
                    cur = strip_type(member.get_DIE_from_attribute("DW_AT_type"))
                bf = bitfield(member)
                if bf:                                   # bytes to read, the bits in them
                    rows.append((mnum, sname, field, off, (bf[1] + bf[2] + 7) // 8, bf[1], bf[2]))
                else:
                    rows.append((mnum, sname, field, off, type_size(member.get_DIE_from_attribute("DW_AT_type"), asz), 0, 0))
        return rows


def parse_specs(args):
    if any(":" in a for a in args):
        out = []
        for a in args:
            s, _, f = a.partition(":")
            out.append((s, [x for x in f.split(",") if x]))
        return out
    return [(args[0], args[1:])]


def fmt_row(tag, r):
    return f'    {{ "{tag}", {r[0]}, "{r[1]}", "{r[2]}", {r[3]}, {r[4]}, {r[5]}, {r[6]} }},'


def update(header, tag, image, rows):
    text = open(header).read()
    if BEGIN not in text or END not in text:
        sys.exit(f"gen-struct-layout: {header} has no rows markers")
    head, rest = text.split(BEGIN, 1)
    body, tail = rest.split(END, 1)
    groups = []                       # [tag, [images], [rows]]
    for line in body.splitlines():
        m = GROUP.match(line)
        if m:
            groups.append([m.group(1), m.group(2).split(", "), []])
            continue
        m = ROW.match(line)
        if m and groups:
            groups[-1][2].append((int(m.group(2)), m.group(3), m.group(4), int(m.group(5)), int(m.group(6)),
                                  int(m.group(7) or 0), int(m.group(8) or 0)))
    for g in groups:                  # the image may have moved to another layout
        if image in g[1]:
            g[1].remove(image)
    groups = [g for g in groups if g[1]]
    same = next((g for g in groups if g[2] == rows), None)
    if same:
        same[1].append(image)
        same[1].sort()
        what = f"same layout as {same[0]}"
    else:
        taken = {g[0] for g in groups}
        t, n = tag, 2
        while t in taken:
            t, n = f"{tag}-{n}", n + 1
        groups.append([t, [image], rows])
        what = f"new layout {t}"
    lines = []
    for g in groups:
        lines.append(f"    /* layout {g[0]}: {', '.join(g[1])} */")
        lines += [fmt_row(g[0], r) for r in g[2]]
    open(header, "w").write(head + BEGIN + "\n" + "\n".join(lines) + "\n    " + END + tail)
    print(f"gen-struct-layout: {image}: {what} in {header}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--update", metavar="HEADER")
    ap.add_argument("--tag")
    ap.add_argument("--name", help="the image's name in the header (default: its file name)")
    ap.add_argument("elf")
    ap.add_argument("spec", nargs="*")
    a = ap.parse_args()
    specs = a.spec
    if not specs:
        m = SPEC.search(open(a.update).read()) if a.update else None
        if not m:
            ap.error("give SPECs, or --update a header with a layout-spec: comment")
        specs = m.group(1).split()
    rows = layout(a.elf, parse_specs(specs))
    image = a.name or os.path.basename(a.elf)
    if a.update:
        update(a.update, a.tag or image, image, rows)
    else:
        for r in rows:
            print(fmt_row(a.tag or image, r))


if __name__ == "__main__":
    main()
