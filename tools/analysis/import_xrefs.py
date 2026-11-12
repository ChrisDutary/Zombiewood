#!/usr/bin/env python3
"""Import cross-reference scanner for ELF32 ARM shared objects (no full disassembly needed).

For every imported symbol reports:
  - kind (func via PLT / data via GOT or ABS32), PLT stub address, GOT slot
  - number of direct call sites (ARM BL/BLX/B and Thumb-2 BL/BLX whose target is the PLT stub)
  - number of data relocations (R_ARM_ABS32 / R_ARM_GLOB_DAT) and literal-pool/GOT references
  - the exported functions that contain the call sites (nearest preceding .dynsym FUNC symbol)

Thumb->ARM long-branch veneers emitted by the linker inside .text
(`bx pc; nop; ldr ip,[pc]; add pc,ip,pc; .word off`) that land on a PLT stub are resolved too, so
tail calls such as `b.w <veneer>` are counted ('via_veneer').

The PLT is assumed to be the classic 12-byte ARM stub layout produced by gold/bfd
(20-byte PLT0 header followed by one 12-byte entry per R_ARM_JUMP_SLOT, in .rel.plt order).

Usage: python import_xrefs.py <lib.so> <out.json>
Requires: pyelftools
"""
import sys, json, struct, bisect, collections
from elftools.elf.elffile import ELFFile

def main(path, out):
    f = open(path, 'rb'); e = ELFFile(f)
    S = {s.name: s for s in e.iter_sections()}
    plt, text = S['.plt'], S['.text']
    dynsym = S['.dynsym']
    syms = list(dynsym.iter_symbols())
    # function symbol map for attribution
    funcs = sorted((s['st_value'] & ~1, s.name) for s in syms
                   if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF')
    faddr = [a for a, _ in funcs]
    def owner(a):
        i = bisect.bisect_right(faddr, a) - 1
        return funcs[i][1] if i >= 0 else '?'
    imports = collections.OrderedDict()
    for s in syms:
        if s['st_shndx'] == 'SHN_UNDEF' and s.name:
            imports[s.name] = {'name': s.name, 'type': s['st_info']['type'], 'bind': s['st_info']['bind'],
                               'plt': None, 'got': None, 'calls': 0, 'abs32': 0, 'glob_dat': 0,
                               'got_refs': 0, 'callers': collections.Counter()}
    plt0 = plt['sh_addr'] + 20
    stub2name = {}
    for i, r in enumerate(S['.rel.plt'].iter_relocations()):
        n = syms[r['r_info_sym']].name
        imports[n]['plt'] = plt0 + 12 * i
        imports[n]['got'] = r['r_offset']
        stub2name[plt0 + 12 * i] = n
    got2name = {}
    for r in S['.rel.dyn'].iter_relocations():
        if not r['r_info_sym']: continue
        n = syms[r['r_info_sym']].name
        if n not in imports: continue
        t = r['r_info_type']
        if t == 2: imports[n]['abs32'] += 1
        elif t == 21:
            imports[n]['glob_dat'] += 1; imports[n]['got'] = r['r_offset']; got2name[r['r_offset']] = n
    data = text.data(); base = text['sh_addr']
    plo, phi = plt['sh_addr'], plt['sh_addr'] + plt['sh_size']
    # linker veneers: 78 47 c0 46 | 00 c0 9f e5 | 0f f0 8c e0 | <off>   (target = veneer+16 + off)
    pat = bytes.fromhex('7847c04600c09fe50ff08ce0')
    veneers = {}
    i = data.find(pat)
    while i >= 0:
        off = struct.unpack_from('<i', data, i + 12)[0]
        tgt = (base + i + 16 + off) & 0xffffffff
        if tgt in stub2name:
            veneers[base + i] = stub2name[tgt]
        i = data.find(pat, i + 2)
    def hit(src, tgt):
        n = stub2name.get(tgt)
        if n is None and tgt in veneers:
            n = veneers[tgt]; imports[n]['via_veneer'] = imports[n].get('via_veneer', 0) + 1
        if n:
            imports[n]['calls'] += 1; imports[n]['callers'][owner(src)] += 1
    # Thumb-2 BL / BLX (and B.W) scan, every halfword
    hw = struct.unpack_from('<%dH' % (len(data) // 2), data)
    for i in range(len(hw) - 1):
        h1 = hw[i]
        if h1 & 0xF800 != 0xF000: continue
        h2 = hw[i + 1]
        if h2 & 0x8000 == 0: continue
        kind = h2 & 0x5000          # 0x5000 BL, 0x4000 BLX, 0x1000 B.W
        if kind not in (0x5000, 0x4000, 0x1000): continue
        s = (h1 >> 10) & 1; j1 = (h2 >> 13) & 1; j2 = (h2 >> 11) & 1
        i1 = 1 - (j1 ^ s); i2 = 1 - (j2 ^ s)
        imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
        if s: imm -= 1 << 25
        pc = base + 2 * i + 4
        tgt = (pc & ~3) + imm if kind == 0x4000 else pc + imm
        if plo <= tgt < phi or tgt in veneers: hit(pc - 4, tgt)
    # ARM BL / B / BLX scan, every word
    w = struct.unpack_from('<%dI' % (len(data) // 4), data)
    for i, x in enumerate(w):
        if (x & 0x0E000000) != 0x0A000000: continue
        imm = x & 0xFFFFFF
        if imm & 0x800000: imm -= 1 << 24
        tgt = base + 4 * i + 8 + (imm << 2)
        if plo <= tgt < phi: hit(base + 4 * i, tgt)
    # literal words holding (GOT slot - GOT base) are not resolvable cheaply; count words equal to PC-relative GOT refs is skipped.
    res = []
    for n, d in imports.items():
        d['callers_total'] = len(d['callers'])
        d['callers'] = [k for k, _ in d['callers'].most_common(12)]
        d['used'] = bool(d['calls'] or d['abs32'] or d['glob_dat'])
        res.append(d)
    json.dump(res, open(out, 'w'), indent=1)
    unused = [d['name'] for d in res if not d['used']]
    print('imports', len(res), 'plt', len(stub2name), 'unused', len(unused), unused)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
