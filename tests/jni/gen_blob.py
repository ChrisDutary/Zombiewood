"""Regenera tests/jni/jnitest_blob.inc (libjnitest.so, ELF32 ARM) con el clang/lld de Visual Studio.

  python tests/jni/gen_blob.py            (desde Zombiewood/)

El .inc se versiona para que el build no dependa de un toolchain ARM.
"""
import os, subprocess, sys, tempfile

LLVM = os.environ.get("ZW_LLVM_BIN", r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin")
CLANG = os.path.join(LLVM, "clang.exe")
LLD = os.path.join(LLVM, "ld.lld.exe")
HERE = os.path.dirname(os.path.abspath(__file__))
CFLAGS = ["--target=armv7a-linux-androideabi", "-mthumb", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16", "-O1",
          "-ffreestanding", "-fno-builtin", "-nostdlib", "-fPIC", "-fno-jump-tables", "-fno-stack-protector",
          "-funwind-tables", "-fvisibility=hidden"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("fallo: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        obj = os.path.join(tmp, "jnitest.o")
        so = os.path.join(tmp, "libjnitest.so")
        run([CLANG, *CFLAGS, "-c", os.path.join(HERE, "jnitest.c"), "-o", obj])
        run([LLD, "-shared", "--hash-style=sysv", "-soname", "libjnitest.so", "-z", "now", "-Bsymbolic",
             "--allow-shlib-undefined", "-o", so, obj])
        with open(so, "rb") as fh:
            data = fh.read()
    with open(os.path.join(HERE, "jnitest_blob.inc"), "w", newline="\n") as f:
        f.write("// GENERADO por tests/jni/gen_blob.py a partir de tests/jni/jnitest.c. No editar.\n")
        f.write("static const unsigned char kJniTestLib[] = {\n")
        for i in range(0, len(data), 24):
            f.write("  " + ",".join(str(b) for b in data[i:i + 24]) + ",\n")
        f.write("};\n")
    print("ok (%d bytes)" % len(data))


if __name__ == "__main__":
    main()
