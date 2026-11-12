"""Regenera los blobs ARM de los tests de T3 con el clang/lld de Visual Studio.

  python tests/cpu/gen_blobs.py            (desde Zombiewood/)

Salidas (versionadas, para que el build no dependa de un toolchain ARM):
  tests/cpu/guest_blob.inc      .text de guest_code.c + offsets de simbolos
  tests/loader/testlib_blob.inc libzwtest.so (ELF32 ARM) como array de bytes
Requiere pyelftools.
"""
import os, subprocess, sys, tempfile
from elftools.elf.elffile import ELFFile

LLVM = os.environ.get("ZW_LLVM_BIN", r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin")
CLANG = os.path.join(LLVM, "clang.exe")
LLD = os.path.join(LLVM, "ld.lld.exe")
HERE = os.path.dirname(os.path.abspath(__file__))
CFLAGS = ["--target=armv7a-linux-androideabi", "-mthumb", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16", "-O1",
          "-ffreestanding", "-fno-builtin", "-nostdlib", "-fPIC", "-fno-jump-tables", "-fno-stack-protector",
          "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fvisibility=default"]

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("fallo: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))

def emit_bytes(f, name, data):
    f.write("static const unsigned char %s[] = {\n" % name)
    for i in range(0, len(data), 24):
        f.write("  " + ",".join(str(b) for b in data[i:i + 24]) + ",\n")
    f.write("};\n")

def gen_cpu_blob(tmp):
    obj = os.path.join(tmp, "guest_code.o")
    run([CLANG, *CFLAGS, "-c", os.path.join(HERE, "guest_code.c"), "-o", obj])
    with open(obj, "rb") as fh:
        elf = ELFFile(fh)
        text = elf.get_section_by_name(".text")
        for sec in elf.iter_sections():
            if sec.header.sh_type in ("SHT_REL", "SHT_RELA") and sec.name in (".rel.text", ".rela.text"):
                n = sec.num_relocations()
                if n:
                    sys.exit("guest_code.o tiene %d relocaciones en .text; el blob debe ser autocontenido" % n)
        idx = [i for i, s in enumerate(elf.iter_sections()) if s.name == ".text"][0]
        syms = sorted((s.name, s.entry.st_value) for s in elf.get_section_by_name(".symtab").iter_symbols()
                      if s.entry.st_shndx == idx and s.name.startswith("t_") and s.entry.st_info.type == "STT_FUNC")
        data = text.data()
    with open(os.path.join(HERE, "guest_blob.inc"), "w", newline="\n") as f:
        f.write("// GENERADO por tests/cpu/gen_blobs.py a partir de guest_code.c. No editar.\n")
        emit_bytes(f, "kGuestBlob", data)
        f.write("// Offsets dentro del blob (bit 0 = Thumb).\n")
        for name, val in syms:
            f.write("static const unsigned kOff_%s = 0x%x;\n" % (name, val))

def gen_testlib(tmp):
    src = os.path.join(HERE, "..", "loader", "testlib.c")
    obj = os.path.join(tmp, "testlib.o")
    so = os.path.join(tmp, "libzwtest.so")
    flags = [f for f in CFLAGS if f not in ("-fno-unwind-tables", "-fno-asynchronous-unwind-tables")] + ["-funwind-tables"]
    run([CLANG, *flags, "-c", src, "-o", obj])
    run([LLD, "-shared", "--hash-style=sysv", "-soname", "libzwtest.so", "-z", "now", "-Bsymbolic",
         "--no-rosegment", "-o", so, obj])
    with open(so, "rb") as fh:
        data = fh.read()
    with open(os.path.join(HERE, "..", "loader", "testlib_blob.inc"), "w", newline="\n") as f:
        f.write("// GENERADO por tests/cpu/gen_blobs.py a partir de tests/loader/testlib.c. No editar.\n")
        emit_bytes(f, "kTestLib", data)

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        gen_cpu_blob(tmp)
        if os.path.exists(os.path.join(HERE, "..", "loader", "testlib.c")):
            gen_testlib(tmp)
    print("ok")
