#!/usr/bin/env python3
"""ELF32 ARM ABI report: headers, segments, sections, .ARM.attributes, dynamic tags,
relocation type counts, .init_array/.fini_array contents, TLS/stack-guard indicators.

Usage: python elf_abi.py <lib.so> [out.json]
Requires: pyelftools
"""
import sys, json, struct, collections
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from elftools.elf.enums import ENUM_RELOC_TYPE_ARM

RNAME = {v: k for k, v in ENUM_RELOC_TYPE_ARM.items()}

def main(path, out=None):
    f = open(path, 'rb'); e = ELFFile(f)
    rep = {'header': {k: (v if not hasattr(v, 'items') else dict(v)) for k, v in e.header.items() if k != 'e_ident'},
           'flags_hex': hex(e.header['e_flags'])}
    rep['segments'] = [{'type': s['p_type'], 'off': s['p_offset'], 'vaddr': s['p_vaddr'], 'filesz': s['p_filesz'],
                        'memsz': s['p_memsz'], 'flags': s['p_flags'], 'align': s['p_align']} for s in e.iter_segments()]
    rep['sections'] = [{'name': s.name, 'type': s['sh_type'], 'addr': s['sh_addr'], 'off': s['sh_offset'],
                        'size': s['sh_size'], 'flags': s['sh_flags']} for s in e.iter_sections()]
    att = e.get_section_by_name('.ARM.attributes')
    attrs = {}
    if att:
        for sub in att.iter_subsections():
            for ss in sub.iter_subsubsections():
                for a in ss.iter_attributes():
                    attrs[str(a.tag)] = a.value
    rep['arm_attributes'] = attrs
    dyn = e.get_section_by_name('.dynamic')
    rep['dynamic'] = [(t.entry.d_tag, getattr(t, 'needed', None) or getattr(t, 'soname', None) or t.entry.d_val)
                      for t in dyn.iter_tags()]
    relocs = {}
    symrel = collections.defaultdict(collections.Counter)
    for s in e.iter_sections():
        if isinstance(s, RelocationSection):
            c = collections.Counter()
            symtab = e.get_section(s['sh_link'])
            for r in s.iter_relocations():
                t = RNAME.get(r['r_info_type'], str(r['r_info_type']))
                c[t] += 1
                if r['r_info_sym']:
                    sym = symtab.get_symbol(r['r_info_sym'])
                    kind = 'undef' if sym['st_shndx'] == 'SHN_UNDEF' else 'defined'
                    symrel[s.name][t + ':' + kind] += 1
            relocs[s.name] = dict(c)
    rep['relocations'] = relocs
    rep['relocations_by_symkind'] = {k: dict(v) for k, v in symrel.items()}
    for nm in ('.init_array', '.fini_array'):
        s = e.get_section_by_name(nm)
        if s:
            d = s.data()
            rep[nm] = [hex(x) for x in struct.unpack('<%dI' % (len(d) // 4), d)]
    ds = e.get_section_by_name('.dynsym')
    names = {s.name for s in ds.iter_symbols()}
    rep['indicators'] = {k: (k in names) for k in
                         ('__stack_chk_guard', '__stack_chk_fail', '__tls_get_addr', '__aeabi_read_tp',
                          'pthread_key_create', '__cxa_thread_atexit', '__gnu_Unwind_Find_exidx', 'dl_unwind_find_exidx',
                          'JNI_OnLoad', 'JNI_OnUnload', '__cxa_begin_catch', '__gxx_personality_v0')}
    rep['has_PT_TLS'] = any(s['p_type'] == 'PT_TLS' for s in e.iter_segments())
    txt = json.dumps(rep, indent=1, default=str)
    if out: open(out, 'w').write(txt)
    print(txt if not out else 'written ' + out)

if __name__ == '__main__':
    main(*sys.argv[1:3])
