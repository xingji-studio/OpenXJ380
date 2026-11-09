"""Shared native ELF milestone targets for the product and public build graphs."""
from pathlib import Path

PRODUCT_XAPI_SOURCES = (
    "libsys", "krlibc", "string", "ctype", "errno", "stdlib", "stdio", "unistd",
    "posix_ret", "posix_fs", "posix_mman", "posix_time", "posix_ioctl", "posix_select",
    "posix_uio", "poll", "permissions", "xtuiapi", "xguiapi", "xclipboard",
)
PUBLIC_XAPI_SOURCES = ("libsys", "xgui_stubs", "installer_native")


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


def emit_xapi(n, base=".", product=False):
    """Build a separate PIC runtime; never reuse the static application's objects."""
    base = Path(base)
    source = base / "user/rtld"
    includes = Path("user/xapi/include")
    flags = ("-O2 -g -ffreestanding -fno-builtin -fno-stack-protector -fno-strict-aliasing "
             "-fno-asynchronous-unwind-tables -mno-red-zone -fPIC -fshort-wchar -nostdinc "
             "-Wall -Wextra -Wno-write-strings -DXAPI_SHARED_RUNTIME=1 -I" + includes.as_posix())
    n.rule("xapi_shared_cxx", "mkdir -p $$(dirname $out) && $user_cxx " + flags +
           " -std=gnu++17 -fno-exceptions -fno-rtti -MMD -MF $out.d -c $in -o $out",
           "[XAPI PIC] $in", depfile="$out.d")
    n.rule("xapi_shared_cc", "mkdir -p $$(dirname $out) && $user_cc " + flags +
           " -std=c11 $profile -I" + source.as_posix() + " -MMD -MF $out.d -c $in -o $out",
           "[XAPI PIC] $in", depfile="$out.d")
    if product:
        # Core libc/POSIX and native GUI syscall wrappers. The network/TLS and
        # media archives keep their separate dependency and licensing scopes.
        names = PRODUCT_XAPI_SOURCES
    else:
        names = PUBLIC_XAPI_SOURCES
    objects = []
    for name in names:
        obj = Path("out/xapi-shared") / (name + ".cpp.o")
        n.build(obj, "xapi_shared_cxx", Path("user/xapi") / (name + ".cpp"))
        objects.append(obj)
    runtime = Path("out/xapi-shared/runtime.c.o")
    n.build(runtime, "xapi_shared_cc", source / "xapi_runtime.c",
            variables={"profile": "" if product else "-DXAPI_MINIMAL_RUNTIME=1"})
    crt = Path("out/rtld/xapi_crt0.S.o")
    n.build(crt, "native_rtld_as", source / "xapi_crt0.S")
    note = Path("out/rtld/abi_note.S.o")
    library = Path("out/libxapi.so.1")
    n.build(library, "native_rtld_link", objects + [runtime, note],
            variables={"mode": "-shared -soname libxapi.so.1"})
    outputs = [library, crt]
    if product:
        demo_source = Path("user/xapi_demo")
        demo_objects = []
        for name in ("main.c", "library.c"):
            obj = Path("out/xapi_demo") / (name + ".o")
            n.build(obj, "xapi_shared_cc", demo_source / name)
            demo_objects.append(obj)
        companion = Path("out/libxapi-demo.so.1")
        n.build(companion, "native_rtld_link", [demo_objects[1], note, library],
                variables={"mode": "-shared -soname libxapi-demo.so.1 -rpath '$$ORIGIN'"})
        program = Path("out/xapi-demo.elf")
        n.build(program, "native_rtld_link", [demo_objects[0], crt, note, companion, library],
                variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-xj380-x86_64.so.1 "
                                   "-rpath '$$ORIGIN'"})
        outputs += [program, companion]
    else:
        obj = Path("out/xapi-shared/demo.c.o")
        n.build(obj, "xapi_shared_cc", source / "xapi_demo.c")
        program = Path("out/xapi-shared-demo.elf")
        n.build(program, "native_rtld_link", [obj, crt, note, library],
                variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-xj380-x86_64.so.1"})
        outputs.append(program)
    n.build("shared_xapi", "phony", outputs + [Path("out/ld-xj380-x86_64.so.1")])
    return outputs
