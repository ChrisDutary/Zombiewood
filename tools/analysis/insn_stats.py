#!/usr/bin/env python3
"""Instruction-set usage statistics for an ELF32 ARM .so with FUNC symbols.

Disassembles every FUNC symbol (mode from bit 0, body = st_size) and reports:
  * ARM vs Thumb function counts
  * counts of VFP / NEON / hardware-divide / CP15 (TLS: mrc p15,0,rX,c13,c0,3) / svc / swp /
    ldrex-strex / IT-block instructions and the first functions that use each class
  * the set of distinct VFP/NEON mnemonics seen (what the CPU core must implement)

Usage: python insn_stats.py <lib.so> [out.json] [--jobs N]
Requires: pyelftools, capstone
"""
import sys, json, collections, argparse, multiprocessing as mp
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone.arm import ARM_GRP_NEON, ARM_GRP_VFP2, ARM_GRP_VFP3, ARM_GRP_VFP4, ARM_GRP_FPARMV8

_code = None
_base = 0


def _init(path):
    global _code, _base
    e = ELFFile(open(path, 'rb'))
    t = e.get_section_by_name('.text')
    _code = t.data()
    _base = t['sh_addr']


def _work(chunk):
    cls = collections.Counter()
    mn = collections.Counter()
    first = {}
    for name, addr, size, thumb in chunk:
        md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
        md.detail = True
        md.skipdata = True
        for ins in md.disasm(_code[addr - _base:addr - _base + size], addr):
            if ins.id == 0:
                continue
            m = ins.mnemonic
            g = ins.groups
            k = None
            if ARM_GRP_NEON in g:
                k = 'neon'
            elif ARM_GRP_VFP2 in g or ARM_GRP_VFP3 in g or ARM_GRP_VFP4 in g or ARM_GRP_FPARMV8 in g:
                k = 'vfp'
            elif m in ('sdiv', 'udiv'):
                k = 'hwdiv'
            elif m in ('mrc', 'mcr'):
                k = 'cp15:' + ins.op_str.replace(' ', '')
            elif m in ('svc', 'swp', 'swpb', 'bkpt', 'udf'):
                k = m
            elif m.startswith(('ldrex', 'strex')):
                k = 'excl'
            elif m.startswith('it') and len(m) <= 5 and thumb and not ins.op_str.startswith('r'):
                k = 'it-block'
            elif m in ('tbb', 'tbh'):
                k = 'tbb/tbh'
            elif m in ('movw', 'movt'):
                k = 'movw/movt'
            elif m in ('clz', 'rbit', 'rev', 'rev16', 'bfi', 'bfc', 'ubfx', 'sbfx', 'uxtb', 'sxtb', 'uxth', 'sxth', 'mls', 'smull', 'umull', 'umlal', 'smlal'):
                k = 'v6t2+:' + m
            if k:
                cls[k] += 1
                first.setdefault(k, name)
                if k in ('neon', 'vfp'):
                    mn[m.split('.')[0]] += 1
    return cls, mn, first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lib')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--jobs', type=int, default=max(1, mp.cpu_count() - 2))
    a = ap.parse_args()
    e = ELFFile(open(a.lib, 'rb'))
    t = e.get_section_by_name('.text')
    lo, hi = t['sh_addr'], t['sh_addr'] + t['sh_size']
    funcs = {}
    for s in e.get_section_by_name('.dynsym').iter_symbols():
        if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF' and lo <= (s['st_value'] & ~1) < hi and s['st_size']:
            funcs[s['st_value'] & ~1] = (s.name, s['st_value'] & ~1, s['st_size'], s['st_value'] & 1)
    fl = sorted(funcs.values(), key=lambda x: x[1])
    rep = {'functions': len(fl), 'thumb': sum(1 for f in fl if f[3]), 'arm': sum(1 for f in fl if not f[3]),
           'bytes_thumb': sum(f[2] for f in fl if f[3]), 'bytes_arm': sum(f[2] for f in fl if not f[3]),
           'arm_examples': [f[0] for f in fl if not f[3]][:40]}
    chunks = [fl[i:i + 500] for i in range(0, len(fl), 500)]
    cls = collections.Counter()
    mn = collections.Counter()
    first = {}
    with mp.Pool(a.jobs, initializer=_init, initargs=(a.lib,)) as pool:
        for c, m, f in pool.imap(_work, chunks):
            cls.update(c)
            mn.update(m)
            for k, v in f.items():
                first.setdefault(k, v)
    rep['classes'] = dict(cls.most_common())
    rep['first_user'] = first
    rep['fp_mnemonics'] = dict(mn.most_common())
    txt = json.dumps(rep, indent=1)
    if a.out:
        open(a.out, 'w').write(txt)
    print(txt)


if __name__ == '__main__':
    main()
