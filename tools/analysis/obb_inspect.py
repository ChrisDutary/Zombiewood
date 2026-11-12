#!/usr/bin/env python3
"""Inspect a Gameloft OBB (a ZIP) and its nested .bar archives (ZIPs too).

Prints, for the outer archive and every nested archive: entry count, compression methods used,
general-purpose flags seen (bit0 = encrypted, bit3 = data descriptor), extension histogram, and
whether nested archives are STORED (method 0, readable in place by offset) or DEFLATED.

Usage: python obb_inspect.py <file.obb> [--list] [--nested-list N]
"""
import sys, io, zipfile, collections, argparse


def summarize(zf, label, show=0):
    infos = zf.infolist()
    meth = collections.Counter(i.compress_type for i in infos)
    flags = collections.Counter(i.flag_bits for i in infos)
    ext = collections.Counter((i.filename.rsplit('.', 1)[-1].lower() if '.' in i.filename else '(none)') for i in infos if not i.is_dir())
    tot_c = sum(i.compress_size for i in infos)
    tot_u = sum(i.file_size for i in infos)
    print('%s: %d entries, comp=%d uncomp=%d methods=%s flags=%s' % (
        label, len(infos), tot_c, tot_u, dict(meth), {hex(k): v for k, v in flags.items()}))
    print('   ext:', dict(ext.most_common(25)))
    for i in infos[:show]:
        print('     %-60s m=%d %10d -> %10d off=%d' % (i.filename, i.compress_type, i.compress_size, i.file_size, i.header_offset))
    return infos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('obb')
    ap.add_argument('--list', action='store_true', help='list all outer entries')
    ap.add_argument('--nested-list', type=int, default=0, help='list first N entries of each nested archive')
    a = ap.parse_args()
    zf = zipfile.ZipFile(a.obb)
    infos = summarize(zf, 'OUTER ' + a.obb, 10 ** 9 if a.list else 0)
    for i in infos:
        head = b''
        with zf.open(i) as f:
            head = f.read(4)
        if head[:2] == b'PK':
            data = zf.read(i)
            try:
                inner = zipfile.ZipFile(io.BytesIO(data))
            except zipfile.BadZipFile as ex:
                print('  nested %s: PK header but not a zip (%s)' % (i.filename, ex))
                continue
            summarize(inner, '  NESTED %s (outer method=%d, outer off=%d)' % (i.filename, i.compress_type, i.header_offset), a.nested_list)
        else:
            print('  file %s m=%d size=%d head=%s' % (i.filename, i.compress_type, i.file_size, head.hex()))


if __name__ == '__main__':
    main()
