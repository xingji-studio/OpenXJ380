# SPDX-License-Identifier: Apache-2.0
"""Shared EPF runtime and dynamic example edges for both build graphs."""
from pathlib import Path


def emit(n, base="."):
    base = Path(base)
    source = base / "user/epf_rtld"
    converter = base / "tools/elf_to_epf.py"
    includes = base / "include"
    n.rule("epf_rtld_cc", "mkdir -p $$(dirname $out) && $user_cc "
           "-O2 -g -std=c11 -ffreestanding -fno-builtin -fno-stack-protector "
           "-fno-asynchronous-unwind-tables -mno-red-zone -fPIC -Wall -Wextra -Werror "
           " $visibility $tls_model -I" + includes.as_posix() + " -MMD -MF $out.d -c $in -o $out",
           "[EPF CC] $in", depfile="$out.d")
    n.rule("epf_convert", "python3 " + converter.as_posix() + " $in --output $out $mode", "[EPF] $out")
    n.rule("epf_rtld_cxx", "mkdir -p $$(dirname $out) && $user_cxx "
           "-O2 -g -std=c++17 -ffreestanding -fno-builtin -fno-stack-protector -fno-exceptions -fno-rtti "
           "-fno-asynchronous-unwind-tables -mno-red-zone -fPIC -Wall -Wextra -Werror "
           "-I" + includes.as_posix() + " -MMD -MF $out.d -c $in -o $out",
           "[EPF CXX] $in", depfile="$out.d")
    objects = {}
    for name in ("rtld.c", "demo.c", "demo_library.c", "tls.c", "tls_library.c", "tls_ie.c", "tls_demo.c",
                 "cxx_abi.c", "cxx_tls_library.cpp", "cxx_tls_demo.cpp"):
        obj = Path("out/epf_rtld") / (name + ".o")
        n.build(obj, "epf_rtld_cxx" if name.endswith(".cpp") else "epf_rtld_cc", source / name,
                implicit=[includes / "epf_format.h", includes / "epf_runtime.h", source / "tls.h",
                          base / "user/rtld/platform.h"],
                variables={"visibility": "-fvisibility=hidden" if name in ("rtld.c", "tls.c") else "",
                           "tls_model": "-ftls-model=initial-exec" if name == "tls_ie.c" else ""})
        objects[name] = obj
    thread_object = Path("out/epf_rtld/thread.S.o")
    n.build(thread_object, "native_rtld_as", source / "thread.S", implicit=[base / "user/rtld/thread.S"])
    bootstrap = Path("out/epf_rtld/bootstrap.elf")
    library_elf = Path("out/epf_rtld/library.elf")
    program_elf = Path("out/epf_rtld/demo.elf")
    interpreter = Path("out/ld-epf-x86_64.epf")
    library = Path("out/libepf-demo.eps")
    example = Path("out/epf-dynamic-demo.epf")
    n.build(bootstrap, "native_rtld_link",
            [objects["rtld.c"], objects["tls.c"], thread_object, Path("out/rtld/start.S.o")],
            variables={"mode": "-pie --no-dynamic-linker -Bsymbolic -e _start"})
    n.build(library_elf, "native_rtld_link", [objects["demo_library.c"]],
            variables={"mode": "-shared -soname libepf-demo.eps"})
    n.build(program_elf, "native_rtld_link", [objects["demo.c"], Path("out/rtld/crt0.S.o"), library_elf],
            variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-epf-x86_64.epf"})
    n.build(interpreter, "epf_convert", bootstrap, implicit=[converter], variables={"mode": "--abi-version 3"})
    n.build(library, "epf_convert", library_elf, implicit=[converter], variables={"mode": "--shared"})
    n.build(example, "epf_convert", program_elf, implicit=[converter],
            variables={"mode": "--interpreter /system/lib/ld-epf-x86_64.epf"})
    runtime_elf = Path("out/epf_rtld/runtime.elf")
    gd_elf = Path("out/epf_rtld/tls-library.elf")
    ie_elf = Path("out/epf_rtld/tls-ie.elf")
    tls_elf = Path("out/epf_rtld/tls-demo.elf")
    runtime = Path("out/libepf-runtime.eps")
    gd = Path("out/libepf-tls.eps")
    ie = Path("out/libepf-tls-ie.eps")
    tls_example = Path("out/epf-tls-demo.epf")
    n.build(runtime_elf, "native_rtld_link", [objects["tls.c"], thread_object],
            variables={"mode": "-shared -soname libepf-runtime.eps"})
    n.build(gd_elf, "native_rtld_link", [objects["tls_library.c"], runtime_elf],
            variables={"mode": "-shared -soname libepf-tls.eps"})
    n.build(ie_elf, "native_rtld_link", [objects["tls_ie.c"]],
            variables={"mode": "-shared -soname libepf-tls-ie.eps"})
    n.build(tls_elf, "native_rtld_link",
            [objects["tls_demo.c"], Path("out/rtld/crt0.S.o"), gd_elf, ie_elf, runtime_elf],
            variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-epf-x86_64.epf"})
    for output, source_elf, mode in ((runtime, runtime_elf, "--shared --require-tls"),
                                   (gd, gd_elf, "--shared"), (ie, ie_elf, "--shared"),
                                   (tls_example, tls_elf, "--interpreter /system/lib/ld-epf-x86_64.epf")):
        n.build(output, "epf_convert", source_elf, implicit=[converter], variables={"mode": mode})
    cxx_library_elf = Path("out/epf_rtld/cxx-tls-library.elf")
    cxx_program_elf = Path("out/epf_rtld/cxx-tls-demo.elf")
    cxx_library = Path("out/libepf-cxx-tls.eps")
    cxx_example = Path("out/epf-cxx-tls-demo.epf")
    n.build(cxx_library_elf, "native_rtld_link", [objects["cxx_tls_library.cpp"], objects["cxx_abi.c"], runtime_elf],
            variables={"mode": "-shared -soname libepf-cxx-tls.eps"})
    n.build(cxx_program_elf, "native_rtld_link",
            [objects["cxx_tls_demo.cpp"], objects["cxx_abi.c"], Path("out/rtld/crt0.S.o"),
             cxx_library_elf, runtime_elf],
            variables={"mode": "-pie -e _start --dynamic-linker /system/lib/ld-epf-x86_64.epf"})
    n.build(cxx_library, "epf_convert", cxx_library_elf, implicit=[converter], variables={"mode": "--shared"})
    n.build(cxx_example, "epf_convert", cxx_program_elf, implicit=[converter],
            variables={"mode": "--interpreter /system/lib/ld-epf-x86_64.epf"})
    outputs = [interpreter, library, example, runtime, gd, ie, tls_example, cxx_library, cxx_example]
    n.build("epf_tls", "phony", [interpreter, runtime, gd, ie, tls_example])
    n.build("epf_cxx_tls", "phony", [interpreter, runtime, cxx_library, cxx_example])
    n.build("epf_rtld", "phony", outputs)
    return outputs
