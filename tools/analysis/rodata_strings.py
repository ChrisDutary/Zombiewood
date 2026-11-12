#!/usr/bin/env python3
"""Dump printable C strings from an ELF section with their virtual addresses, optionally filtered by regex.

Usage: python rodata_strings.py <lib.so> [--section .rodata] [--min 4] [--grep REGEX] [--out file.tsv]
Output: one line per string: <hex vaddr>\t<string>
"""
import sys, re, argparse
from elftools.elf.elffile import ELFFile

def strings(path, section='.rodata', minlen=4):
    e = ELFFile(open(path, 'rb')); s = e.get_section_by_name(section)
    d = s.data(); base = s['sh_addr']
    for m in re.finditer(rb'[\x09\x0a\x0d\x20-\x7e]{%d,}(?=\x00)' % minlen, d):
        yield base + m.start(), m.group().decode('latin1')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('lib'); ap.add_argument('--section', default='.rodata'); ap.add_argument('--min', type=int, default=4)
    ap.add_argument('--grep'); ap.add_argument('--out')
    a = ap.parse_args()
    rx = re.compile(a.grep) if a.grep else None
    out = open(a.out, 'w', encoding='utf-8') if a.out else sys.stdout
    for addr, s in strings(a.lib, a.section, a.min):
        if rx is None or rx.search(s):
            out.write('%08x\t%s\n' % (addr, s.replace('\n', '\n').replace('\r', '\r')))
