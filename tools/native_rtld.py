"""Shared native ELF milestone targets for the product and public build graphs."""
from pathlib import Path


def emit(n, base="."):
    base = Path(base)
    source = base / "user/rtld"
    includes = base / "include"
    flags = ("-O2 -g -std=c11 -ffreestanding -fno-builtin -fno-stack-protector "
             "-fno-asynchronous-unwind-tables -mno-red-zone -fPIC -Wall -Wextra -Werror")
    n.rule("native_rtld_cc", "mkdir -p $$(dirname $out) && $user_cc " + flags +
           " $visibility -I" + includes.as_posix() + " -I" + source.as_posix() +
           " -MMD -MF $out.d -c $in -o $out",
           "[RTLD CC] $in", depfile="$out.d")
    n.rule("native_rtld_as", "mkdir -p $$(dirname $out) && $user_cc -fPIC -c $in -o $out", "[RTLD AS] $in")
    link_flags = ("--hash-style=both --build-id=none --no-undefined -z max-page-size=0x1000 "
                  "-z separate-code -z now -z relro -z noexecstack")
    n.rule("native_rtld_link", "$user_ld " + link_flags + " $mode $in -o $out", "[RTLD LD] $out")
    objects = {}
    for filename in ("rtld.c", "start.S", "abi_note.S", "crt0.S", "demo.c", "demo_library.c"):
        obj = Path("out/rtld") / (filename + ".o")
        n.build(obj, "native_rtld_cc" if filename.endswith(".c") else "native_rtld_as", source / filename,
                implicit=[source / "platform.h", includes / "elf.h"],
                variables={"visibility": "-fvisibility=hidden" if filename == "rtld.c" else ""})
        objects[filename] = obj
    interpreter = Path("out/ld-xj380-x86_64.so.1")
    library = Path("out/libnative-demo.so.1")
    example = Path("out/native-dynamic-demo.elf")
    n.build(interpreter, "native_rtld_link", [objects[name] for name in ("rtld.c", "start.S", "abi_note.S")],
            variables={"mode": "-shared -Bsymbolic -e _start -soname ld-xj380-x86_64.so.1"})
    n.build(library, "native_rtld_link", [objects["demo_library.c"], objects["abi_note.S"]],
            variables={"mode": "-shared -soname libnative-demo.so.1"})
    n.build(example, "native_rtld_link", [objects[name] for name in ("demo.c", "crt0.S", "abi_note.S")] + [library],
            variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-xj380-x86_64.so.1"})
    outputs = [interpreter, library, example]
    n.build("native_rtld", "phony", outputs)
    return outputs


def emit_debug_demo(n, source="user/rtld_demo"):
    """Product-facing demo; share the already emitted interpreter/CRT rules."""
    source = Path(source)
    objects = []
    for name in ("main.c", "library.c"):
        obj = Path("out/rtld_demo") / (name + ".o")
        n.build(obj, "native_rtld_cc", source / name, implicit=[source / "demo.h"])
        objects.append(obj)
    library = Path("out/librtld-demo.so.1")
    program = Path("out/rtld-demo.elf")
    note = Path("out/rtld/abi_note.S.o")
    n.build(library, "native_rtld_link", [objects[1], note],
            variables={"mode": "-shared -soname librtld-demo.so.1"})
    n.build(program, "native_rtld_link", [objects[0], Path("out/rtld/crt0.S.o"), note, library],
            variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-xj380-x86_64.so.1 "
                               "-rpath '$$ORIGIN'"})
    outputs = [program, library]
    n.build("rtld_demo", "phony", outputs + [Path("out/ld-xj380-x86_64.so.1")])
    return outputs
