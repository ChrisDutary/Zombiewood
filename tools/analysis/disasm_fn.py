#!/usr/bin/env python3
"""Annotated disassembly of functions of an ELF32 ARM .so (ARM or Thumb-2 picked from the symbol's bit 0).

Annotations:
  * branch targets -> exported symbol / PLT import names
  * PIC literal idioms (`ldr rX,[pc,#n]` + `add rX,pc`) -> string contents, symbol name or GOT import
  * with --jni: `ldr rX,[rY,#off]` where off is a JNIEnv / JavaVM vtable slot -> member name (heuristic)

Usage: python disasm_fn.py <lib.so> <symbol-or-0xaddr>[,more] [--jni] [--calls-only] [--max N]
       (0xaddr with bit 0 set = Thumb)
Requires: pyelftools, capstone
"""
import sys, struct, bisect, argparse
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG, ARM_REG_PC

JNI = ("reserved0 reserved1 reserved2 reserved3 GetVersion DefineClass FindClass FromReflectedMethod FromReflectedField "
"ToReflectedMethod GetSuperclass IsAssignableFrom ToReflectedField Throw ThrowNew ExceptionOccurred ExceptionDescribe "
"ExceptionClear FatalError PushLocalFrame PopLocalFrame NewGlobalRef DeleteGlobalRef DeleteLocalRef IsSameObject "
"NewLocalRef EnsureLocalCapacity AllocObject NewObject NewObjectV NewObjectA GetObjectClass IsInstanceOf GetMethodID "
"CallObjectMethod CallObjectMethodV CallObjectMethodA CallBooleanMethod CallBooleanMethodV CallBooleanMethodA "
"CallByteMethod CallByteMethodV CallByteMethodA CallCharMethod CallCharMethodV CallCharMethodA CallShortMethod "
"CallShortMethodV CallShortMethodA CallIntMethod CallIntMethodV CallIntMethodA CallLongMethod CallLongMethodV "
"CallLongMethodA CallFloatMethod CallFloatMethodV CallFloatMethodA CallDoubleMethod CallDoubleMethodV CallDoubleMethodA "
"CallVoidMethod CallVoidMethodV CallVoidMethodA CallNonvirtualObjectMethod CallNonvirtualObjectMethodV "
"CallNonvirtualObjectMethodA CallNonvirtualBooleanMethod CallNonvirtualBooleanMethodV CallNonvirtualBooleanMethodA "
"CallNonvirtualByteMethod CallNonvirtualByteMethodV CallNonvirtualByteMethodA CallNonvirtualCharMethod "
"CallNonvirtualCharMethodV CallNonvirtualCharMethodA CallNonvirtualShortMethod CallNonvirtualShortMethodV "
"CallNonvirtualShortMethodA CallNonvirtualIntMethod CallNonvirtualIntMethodV CallNonvirtualIntMethodA "
"CallNonvirtualLongMethod CallNonvirtualLongMethodV CallNonvirtualLongMethodA CallNonvirtualFloatMethod "
"CallNonvirtualFloatMethodV CallNonvirtualFloatMethodA CallNonvirtualDoubleMethod CallNonvirtualDoubleMethodV "
"CallNonvirtualDoubleMethodA CallNonvirtualVoidMethod CallNonvirtualVoidMethodV CallNonvirtualVoidMethodA GetFieldID "
"GetObjectField GetBooleanField GetByteField GetCharField GetShortField GetIntField GetLongField GetFloatField "
"GetDoubleField SetObjectField SetBooleanField SetByteField SetCharField SetShortField SetIntField SetLongField "
"SetFloatField SetDoubleField GetStaticMethodID CallStaticObjectMethod CallStaticObjectMethodV CallStaticObjectMethodA "
"CallStaticBooleanMethod CallStaticBooleanMethodV CallStaticBooleanMethodA CallStaticByteMethod CallStaticByteMethodV "
"CallStaticByteMethodA CallStaticCharMethod CallStaticCharMethodV CallStaticCharMethodA CallStaticShortMethod "
"CallStaticShortMethodV CallStaticShortMethodA CallStaticIntMethod CallStaticIntMethodV CallStaticIntMethodA "
"CallStaticLongMethod CallStaticLongMethodV CallStaticLongMethodA CallStaticFloatMethod CallStaticFloatMethodV "
"CallStaticFloatMethodA CallStaticDoubleMethod CallStaticDoubleMethodV CallStaticDoubleMethodA CallStaticVoidMethod "
"CallStaticVoidMethodV CallStaticVoidMethodA GetStaticFieldID GetStaticObjectField GetStaticBooleanField "
"GetStaticByteField GetStaticCharField GetStaticShortField GetStaticIntField GetStaticLongField GetStaticFloatField "
"GetStaticDoubleField SetStaticObjectField SetStaticBooleanField SetStaticByteField SetStaticCharField "
"SetStaticShortField SetStaticIntField SetStaticLongField SetStaticFloatField SetStaticDoubleField NewString "
"GetStringLength GetStringChars ReleaseStringChars NewStringUTF GetStringUTFLength GetStringUTFChars "
"ReleaseStringUTFChars GetArrayLength NewObjectArray GetObjectArrayElement SetObjectArrayElement NewBooleanArray "
"NewByteArray NewCharArray NewShortArray NewIntArray NewLongArray NewFloatArray NewDoubleArray GetBooleanArrayElements "
"GetByteArrayElements GetCharArrayElements GetShortArrayElements GetIntArrayElements GetLongArrayElements "
"GetFloatArrayElements GetDoubleArrayElements ReleaseBooleanArrayElements ReleaseByteArrayElements "
"ReleaseCharArrayElements ReleaseShortArrayElements ReleaseIntArrayElements ReleaseLongArrayElements "
"ReleaseFloatArrayElements ReleaseDoubleArrayElements GetBooleanArrayRegion GetByteArrayRegion GetCharArrayRegion "
"GetShortArrayRegion GetIntArrayRegion GetLongArrayRegion GetFloatArrayRegion GetDoubleArrayRegion "
"SetBooleanArrayRegion SetByteArrayRegion SetCharArrayRegion SetShortArrayRegion SetIntArrayRegion SetLongArrayRegion "
"SetFloatArrayRegion SetDoubleArrayRegion RegisterNatives UnregisterNatives MonitorEnter MonitorExit GetJavaVM "
"GetStringRegion GetStringUTFRegion GetPrimitiveArrayCritical ReleasePrimitiveArrayCritical GetStringCritical "
"ReleaseStringCritical NewWeakGlobalRef DeleteWeakGlobalRef ExceptionCheck NewDirectByteBuffer GetDirectBufferAddress "
"GetDirectBufferCapacity GetObjectRefType").split()
JVM = "reserved0 reserved1 reserved2 DestroyJavaVM AttachCurrentThread DetachCurrentThread GetEnv AttachCurrentThreadAsDaemon".split()


class Lib:
    def __init__(self, path):
        self.e = ELFFile(open(path, 'rb'))
        self.secs = [(s['sh_addr'], s['sh_addr'] + s['sh_size'], s.name, s.data() if s['sh_type'] != 'SHT_NOBITS' else b'')
                     for s in self.e.iter_sections() if s['sh_addr']]
        ds = self.e.get_section_by_name('.dynsym')
        syms = list(ds.iter_symbols())
        self.byname = {s.name: s for s in syms if s.name}
        fs = sorted((s['st_value'] & ~1, s.name, s['st_size'], s['st_value'] & 1) for s in syms
                    if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT'))
        self.fs = fs
        self.fa = [x[0] for x in fs]
        self.plt = {}
        self.got = {}
        plt = self.e.get_section_by_name('.plt')
        for i, r in enumerate(self.e.get_section_by_name('.rel.plt').iter_relocations()):
            self.plt[plt['sh_addr'] + 20 + 12 * i] = syms[r['r_info_sym']].name
            self.got[r['r_offset']] = syms[r['r_info_sym']].name
        for r in self.e.get_section_by_name('.rel.dyn').iter_relocations():
            if r['r_info_sym']:
                self.got[r['r_offset']] = syms[r['r_info_sym']].name

    def read(self, a, n):
        for lo, hi, nm, d in self.secs:
            if lo <= a < hi and d:
                return d[a - lo:a - lo + n]
        return b''

    def u32(self, a):
        b = self.read(a, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None

    def sec(self, a):
        for lo, hi, nm, d in self.secs:
            if lo <= a < hi:
                return nm

    def name(self, a):
        a &= ~1
        if a in self.plt:
            return self.plt[a] + '@plt'
        i = bisect.bisect_right(self.fa, a) - 1
        if i >= 0:
            b, n, sz, _ = self.fs[i]
            if a == b:
                return n
            if a < b + max(sz, 1):
                return '%s+0x%x' % (n, a - b)
        return None

    def cstr(self, a, maxlen=120):
        b = self.read(a, maxlen)
        z = b.split(b'\0')[0]
        if len(z) >= 1 and all(32 <= c < 127 or c in (9, 10, 13) for c in z):
            return z.decode('latin1')
        return None

    def describe(self, a):
        s = self.sec(a)
        if s is None:
            return None
        if a in self.got:
            return 'GOT[%s]' % self.got[a]
        if s == '.rodata':
            z = self.cstr(a)
            if z is not None and len(z) >= 2:
                return '"%s"' % z.replace('\n', '\\n')
        n = self.name(a)
        if n:
            return n
        if s in ('.got', '.data.rel.ro', '.data.rel.ro.local', '.data'):
            v = self.u32(a)
            if v:
                n2 = self.name(v)
                if n2:
                    return '%s:*%08x -> %s' % (s, a, n2)
        return '%s:%08x' % (s, a)


def disasm(lib, target, jni=False, calls_only=False, maxn=4000, out=sys.stdout):
    if target.startswith('0x'):
        addr = int(target, 16)
        thumb = addr & 1
        addr &= ~1
        size = 0
        nm = lib.name(addr) or target
    else:
        s = lib.byname[target]
        addr = s['st_value'] & ~1
        thumb = s['st_value'] & 1
        size = s['st_size']
        nm = target
    if not size:
        i = bisect.bisect_right(lib.fa, addr)
        size = (lib.fa[i] - addr) if i < len(lib.fa) else 0x400
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    md.detail = True
    code = lib.read(addr, size)
    out.write('== %s @%08x %s size=%d\n' % (nm, addr, 'thumb' if thumb else 'arm', size))
    regs = {}
    n = 0
    off = 0
    while off < len(code) and n < maxn:
        ins = next(md.disasm(code[off:off + 4], addr + off, 1), None)
        if ins is None:
            off += 2 if thumb else 4
            continue
        n += 1
        off += ins.size
        note = ''
        pc = ins.address + (4 if thumb else 8)
        ops = ins.operands
        m = ins.mnemonic.split('.')[0]
        try:
            if m in ('bl', 'blx') or (m.startswith('b') and len(m) <= 3 and m not in ('bic', 'bfc', 'bfi', 'bkpt')) or m in ('cbz', 'cbnz'):
                if ops and ops[-1].type == ARM_OP_IMM:
                    t = ops[-1].imm & 0xffffffff
                    if not (addr <= t < addr + size):
                        note = lib.name(t) or ''
                elif ops and ops[0].type == ARM_OP_REG and m == 'blx':
                    r = regs.get(ops[0].reg)
                    note = 'indirect' + ((' ' + str(r[1])) if r and r[0] in ('sym', 'jni') else '')
                if m in ('bl', 'blx'):
                    for r in (66, 67, 68, 69, 78):  # r0-r3, r12 clobbered (capstone ARM_REG_R0..R3, R12)
                        regs.pop(r, None)
            elif m == 'ldr' and len(ops) == 2 and ops[1].type == ARM_OP_MEM:
                mem = ops[1].mem
                if mem.base == ARM_REG_PC and mem.index == 0:
                    v = lib.u32((pc & ~3) + mem.disp)
                    regs[ops[0].reg] = ('lit', v)
                    note = 'lit=0x%x' % v if v is not None else ''
                else:
                    b = regs.get(mem.base)
                    newv = None
                    if jni and mem.index == 0 and mem.disp % 4 == 0 and 16 <= mem.disp < 4 * len(JNI):
                        k = mem.disp // 4
                        note = 'JNIEnv->%s?' % JNI[k]
                        if k < len(JVM):
                            note += ' /JavaVM->%s?' % JVM[k]
                        newv = ('jni', JNI[k])
                    if b and b[0] == 'addr' and mem.index == 0:
                        a = b[1] + mem.disp
                        note = '[' + str(lib.describe(a)) + ']'
                        if a in lib.got:
                            newv = ('sym', lib.got[a])
                        else:
                            v = lib.u32(a)
                            newv = ('addr', v) if v and lib.sec(v) else None
                            if newv:
                                note += ' = ' + str(lib.describe(v))
                    if newv:
                        regs[ops[0].reg] = newv
                    else:
                        regs.pop(ops[0].reg, None)
            elif m in ('add', 'adds') and len(ops) == 2 and ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_PC:
                b = regs.get(ops[0].reg)
                if b and b[0] == 'lit' and b[1] is not None:
                    a = (b[1] + pc) & 0xffffffff
                    regs[ops[0].reg] = ('addr', a)
                    note = '-> ' + str(lib.describe(a))
                else:
                    regs.pop(ops[0].reg, None)
            elif m == 'add' and len(ops) == 3 and ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_PC and ops[2].type == ARM_OP_REG:
                b = regs.get(ops[2].reg)
                if b and b[0] == 'lit' and b[1] is not None:
                    a = (b[1] + pc) & 0xffffffff
                    regs[ops[0].reg] = ('addr', a)
                    note = '-> ' + str(lib.describe(a))
                else:
                    regs.pop(ops[0].reg, None)
            elif m == 'adr' or (m in ('add', 'addw') and len(ops) == 3 and ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_PC and ops[2].type == ARM_OP_IMM):
                a = ((pc & ~3) + ops[-1].imm) & 0xffffffff
                regs[ops[0].reg] = ('addr', a)
                note = '-> ' + str(lib.describe(a))
            elif m in ('mov', 'movs') and len(ops) == 2 and ops[1].type == ARM_OP_REG:
                if ops[1].reg in regs:
                    regs[ops[0].reg] = regs[ops[1].reg]
                    if regs[ops[0].reg][0] == 'addr':
                        note = '= ' + str(lib.describe(regs[ops[0].reg][1]))
                else:
                    regs.pop(ops[0].reg, None)
            elif ops and ops[0].type == ARM_OP_REG and m not in ('cmp', 'tst', 'str', 'strb', 'strh', 'push', 'cmn', 'strd'):
                regs.pop(ops[0].reg, None)
        except Exception as ex:  # keep going on odd encodings
            note = '!' + str(ex)
        if calls_only and not note:
            continue
        out.write('%08x  %-8s %-34s %s\n' % (ins.address, ins.mnemonic, ins.op_str, ('; ' + note) if note else ''))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('lib')
    ap.add_argument('targets')
    ap.add_argument('--jni', action='store_true')
    ap.add_argument('--calls-only', action='store_true')
    ap.add_argument('--max', type=int, default=4000)
    a = ap.parse_args()
    L = Lib(a.lib)
    for t in a.targets.split(','):
        disasm(L, t, a.jni, a.calls_only, a.max)
