#!/usr/bin/env python3
"""Derive a native->Java call table from the scan_all.py database.

For each function that looks up Java members (loads a JNIEnv slot such as FindClass /
GetStaticMethodID / GetMethodID / GetFieldID / GetStaticFieldID / RegisterNatives and references
string literals), prints the ordered token stream: quoted strings, JNIEnv slot names and direct calls.
The stream is a heuristic (straight-line data-flow only): read it as "these names are resolved here".

Usage: python jni_table.py <scan_all.txt> [--all-slots] [--func REGEX]
"""
import sys, re, argparse

KEY = {'FindClass', 'GetMethodID', 'GetStaticMethodID', 'GetFieldID', 'GetStaticFieldID', 'RegisterNatives',
       'NewGlobalRef', 'GetObjectClass'}
LOOKUP = {'FindClass', 'GetMethodID', 'GetStaticMethodID', 'GetFieldID', 'GetStaticFieldID', 'RegisterNatives'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('db')
    ap.add_argument('--all-slots', action='store_true')
    ap.add_argument('--func')
    a = ap.parse_args()
    rx = re.compile(a.func) if a.func else None
    cur = None
    toks = []

    def flush():
        if cur is None:
            return
        slots = {t[1:] for t in toks if t.startswith('@')}
        strs = [t for t in toks if t.startswith('"')]
        if rx:
            if not rx.search(cur):
                return
        elif not (slots & LOOKUP and strs):
            return
        out = []
        for t in toks:
            if t.startswith('@') and not a.all_slots and t[1:] not in KEY and not t[1:].startswith('Call') \
                    and not t[1:].startswith('GetStatic') and not t[1:].startswith('NewString'):
                continue
            if out and out[-1] == t:
                continue
            out.append(t)
        print('== ' + cur)
        print('   ' + ' '.join(out))

    for line in open(a.db, encoding='utf-8'):
        if line.startswith('== '):
            flush()
            cur = line[3:].split(' @')[0]
            toks = []
            continue
        note = line.split(';', 1)[1].strip() if ';' in line else ''
        m = re.search(r'-> ("(?:.*)")$', note) or re.search(r'= ("(?:.*)")$', note)
        if m and ' mov' not in line[:20]:
            toks.append(m.group(1))
            continue
        m = re.search(r'JNIEnv->(\w+)\?', note)
        if m and ' ldr' in line[:22]:
            toks.append('@' + m.group(1))
            continue
        if re.search(r'^\w+\s+(bl|blx|b\.w|b)\s', line) and note and not note.startswith('indirect') and not note.startswith('lit'):
            toks.append('->' + note.split('+0x')[0])
    flush()


if __name__ == '__main__':
    main()
