#!/usr/bin/env python3
"""Whole-library reference scan: for every FUNC symbol of an ELF32 ARM .so writes the annotated
"interesting" lines (calls, string / GOT / symbol references, JNIEnv vtable slot loads) produced by
disasm_fn.py. Each function is disassembled up to the next FUNC symbol so that static helpers that
follow an exported function are covered too (attributed to the preceding symbol).

The output is a grep-able text database:
    == <symbol> @<addr> <mode> size=<n>
    <addr>  <mnemonic> <operands>   ; <annotation>

Usage: python scan_all.py <lib.so> <out.txt> [--jobs N]
Requires: pyelftools, capstone
"""
import sys, io, argparse, multiprocessing as mp
import disasm_fn

_L = None


def _init(path):
    global _L
    _L = disasm_fn.Lib(path)


def _work(chunk):
    buf = io.StringIO()
    for addr in chunk:
        disasm_fn.disasm(_L, hex(addr), jni=True, calls_only=True, maxn=200000, out=buf)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lib')
    ap.add_argument('out')
    ap.add_argument('--jobs', type=int, default=max(1, mp.cpu_count() - 2))
    a = ap.parse_args()
    L = disasm_fn.Lib(a.lib)
    text = L.e.get_section_by_name('.text')
    lo, hi = text['sh_addr'], text['sh_addr'] + text['sh_size']
    funcs = sorted({(b | t) for b, n, sz, t in L.fs if lo <= b < hi and L.byname[n]['st_info']['type'] == 'STT_FUNC'})
    # dedupe addresses that differ only by thumb bit
    seen = set()
    addrs = []
    for f in funcs:
        if (f & ~1) not in seen:
            seen.add(f & ~1)
            addrs.append(f)
    # make Lib.fa contain only function starts so "size to next symbol" is used by disasm()
    chunks = [addrs[i:i + 400] for i in range(0, len(addrs), 400)]
    with mp.Pool(a.jobs, initializer=_init, initargs=(a.lib,)) as pool, open(a.out, 'w', encoding='utf-8') as out:
        for i, txt in enumerate(pool.imap(_work, chunks)):
            out.write(txt)
            if i % 10 == 0:
                print('chunk %d/%d' % (i, len(chunks)), file=sys.stderr)


if __name__ == '__main__':
    main()
