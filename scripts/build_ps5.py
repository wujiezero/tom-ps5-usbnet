#!/usr/bin/env python3
"""Cross-build the PS5 ELF from pinned inputs; never deploy or run it.

Zig's clang and lld stand in for the host LLVM the SDK's prospero-clang and
prospero-lld wrappers expect, with those wrappers' flags. The crt is built from
third_party/sdk-crt: the SDK's own crt1.o can stop a payload before main().
A build alone is not evidence that it runs.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import json
import re
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["src/main.c", "src/adapter.c", "src/log.c", "src/vendor.c", "src/vendor_regs.c", "src/net.c", "src/http_server.c",
           "src/app_assets.c", "src/files.c", "src/tile.c", "src/crash.c", "src/local_http.c", "src/ncm.c", "src/rtl.c", "src/smb.c", "src/smb_client.c", "src/smb_net.c",
           "src/smb_lwip.c", "src/secret.c", "src/forward.c", "src/forward_lwip.c", "port/sys_arch.c"]
LWIP_DIRECTORIES = ["third_party/lwip/src/core", "third_party/lwip/src/core/ipv4", "third_party/lwip/src/api"]
FLAGS = ["-DUN_COND_REALTIME", "-Iport", "-Ithird_party/lwip/src/include"]
# libsmb2 is third-party: no -Werror. Its socket calls go to src/smb_net.c (include/smb_sock.h).
SMB2_FLAGS = ["-DHAVE_CONFIG_H", "-Ithird_party/libsmb2", "-Ithird_party/libsmb2/include",
              "-Ithird_party/libsmb2/include/smb2", "-include", "include/smb_sock.h", "-w"]
LIBS = ["-lSceIpmi", "-lSceAppInstUtil"]        # in this order: see src/tile.c
OUTPUT = "tom-ps5-usbnet.elf"
SDK_LINK_SCRIPT = "ldscripts/elf_x86_64.x"
SDK_LIBS = ["-lc", "-lkernel_web", "-lSceLibcInternal", "-lSceNet"]
EXPECTED_NEEDED = {"libSceLibcInternal.sprx", "libkernel_web.sprx"}
ALLOWED_NEEDED = EXPECTED_NEEDED | {"libSceNet.sprx", "libSceIpmi.sprx", "libSceAppInstUtil.sprx"}
ELFOSABI_FREEBSD = 9
SUPPORTED_RELOCATIONS = {1, 6, 7, 8}    # R_X86_64_64, GLOB_DAT, JMP_SLOT, RELATIVE
CRT_DIR = ROOT / "third_party/sdk-crt"
CRT_NAMES = ["crt", "syscall", "klog", "nid", "kernel", "rtld", "rtld_so", "rtld_sprx",
             "rtld_payload", "rtld_dlfcn", "mdbg", "patch", "kernel_procio",
             "kernel_iommu", "_start"]
CRT_FLAGS = ["-ffreestanding", "-fno-builtin", "-nostdlib", "-fPIC", "-target", "x86_64-sie-ps5",
             "-fno-plt", "-fno-stack-protector", "-fvisibility-nodllstorageclass=default",
             "-fms-extensions", "-Wno-microsoft-anon-tag", "-O1"]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def pinned_input(name, directory, expected):
    target = (ROOT / directory).resolve()
    receipt = target / "OFFLINE_SOURCE.json"
    if not receipt.is_file():
        raise SystemExit(f"{name} is missing; run scripts/prepare.py --{name}")
    meta = json.loads(receipt.read_text())
    if meta.get("archive_sha256") != expected:
        raise SystemExit(f"{name} receipt identity mismatch")
    return target


def run(command, env):
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    if result.returncode:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"command failed: {command[1]}")
    return result


def build_crt(zig_bin, zig_include, sdk, directory, environment):
    """Every crt source, joined with a relocatable link."""
    directory.mkdir()
    objects = []
    for name in CRT_NAMES:
        source = CRT_DIR / (name + ".c")
        extra = []
        if not source.is_file():
            source = CRT_DIR / (name + ".S")
            extra = ["-masm=intel"]
        target = directory / (name + ".o")
        run([zig_bin, "clang", "-c"] + CRT_FLAGS + extra +
            ["-isystem", zig_include, "-isystem", str(sdk / "target/include"),
             "-o", str(target), str(source)], environment)
        objects.append(str(target))
    crt = directory / "crt1.o"
    run([zig_bin, "ld.lld", "-m", "elf_x86_64", "-r", "-o", str(crt)] + objects, environment)
    return crt


def elf_summary(path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("output is not a 64-bit ELF")
    if data[7] != ELFOSABI_FREEBSD:
        raise SystemExit(f"unexpected OSABI {data[7]}")
    entry_point = struct.unpack("<Q", data[24:32])[0]
    e_type, machine = struct.unpack("<HH", data[16:20])
    phoff = struct.unpack("<Q", data[32:40])[0]
    shoff = struct.unpack("<Q", data[40:48])[0]
    phentsize, phnum = struct.unpack("<HH", data[54:58])
    shentsize, shnum, shstrndx = struct.unpack("<HHH", data[58:64])
    loads, dynamic = [], {}
    for index in range(phnum):
        base = phoff + index * phentsize
        kind, flags, offset, vaddr, _paddr, filesz, memsz, align = struct.unpack(
            "<IIQQQQQQ", data[base:base + 56])
        if kind == 1:
            loads.append({"flags": flags, "align": align, "filesz": filesz})
        elif kind == 3:
            raise SystemExit("unexpected PT_INTERP: host dynamic loader leaked into the artifact")
        elif kind == 2:
            dynamic = {"offset": offset, "filesz": filesz}
    if e_type != 3:
        raise SystemExit("expected ET_DYN (position-independent payload)")
    sections = []
    for index in range(shnum):
        base = shoff + index * shentsize
        sections.append(struct.unpack("<IIQQQQIIQQ", data[base:base + 64]))
    shstr = sections[shstrndx]
    names = data[shstr[4]:shstr[4] + shstr[5]]
    section_names = []
    for entry in sections:
        tail = names[entry[0]:]
        section_names.append(tail[:tail.index(b"\0")].decode())
    symtab = next((s for s in sections if s[1] == 2), None)
    if symtab is None:
        raise SystemExit("no symbol table")
    strtab = sections[symtab[6]]
    symbols = data[strtab[4]:strtab[4] + strtab[5]]
    for step in range(0, symtab[5], 24):
        name, _info, _other, _shndx, value = struct.unpack(
            "<IBBHQ", data[symtab[4] + step:symtab[4] + step + 16])
        if symbols[name:name + 7] == b"_start\0":
            if value != entry_point or entry_point == 0:
                raise SystemExit(f"entry {entry_point:#x} is not a non-zero _start ({value:#x})")
            break
    else:
        raise SystemExit("_start not found")
    # third_party/sdk-crt/rtld_payload.c fails the payload on any other type.
    for section in sections:
        if section[1] == 4:
            for step in range(0, section[5], 24):
                info = struct.unpack("<Q", data[section[4] + step + 8:section[4] + step + 16])[0]
                if info & 0xffffffff not in SUPPORTED_RELOCATIONS:
                    raise SystemExit(f"relocation type {info & 0xffffffff} is not supported by the crt")
    needed = []
    dyn_section = next(s for s in sections if s[1] == 6)
    dynstr = sections[dyn_section[6]]
    strings = data[dynstr[4]:dynstr[4] + dynstr[5]]
    for step in range(0, dyn_section[5], 16):
        tag, value = struct.unpack("<QQ", data[dyn_section[4] + step:dyn_section[4] + step + 16])
        if tag == 1:
            needed.append(strings[value:strings.index(b"\0", value)].decode())
    if not EXPECTED_NEEDED <= set(needed) <= ALLOWED_NEEDED:
        raise SystemExit(f"unexpected DT_NEEDED: {needed}")
    if not loads or any(item["align"] != 0x4000 for item in loads):
        raise SystemExit("unexpected segment alignment")
    for required in (".text", ".rodata", ".dynamic", ".data", ".bss"):
        if required not in section_names:
            raise SystemExit(f"missing section {required}")
    return {
        "path": str(path), "bytes": len(data), "elf_type": "ET_DYN",
        "machine": "x86-64", "osabi": data[7], "entry": hex(entry_point), "dyn_needed": needed,
        "segments": len(loads), "page_size": "0x4000",
        "has_pt_interp": False, "sha256": sha256(path),
        "sections": [name for name in section_names if name],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()

    zig = pinned_input("zig", ".cache/zig-0.14.1", "39f3dc5e79c22088ce878edc821dedb4ca5a1cd9f5ef915e9b3cc3053e8faefa")
    sdk = pinned_input("sdk", ".cache/sdk-v0.45", "5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c")
    if not (sdk / SDK_LINK_SCRIPT).is_file() or not (CRT_DIR / "crt.c").is_file():
        raise SystemExit("SDK link script or vendored crt missing")

    output = Path(args.output or ROOT / "build-ps5" / OUTPUT).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    objects = output.parent / "obj"
    if objects.exists():
        shutil.rmtree(objects)
    objects.mkdir()

    environment = dict(os.environ)
    environment["ZIG_GLOBAL_CACHE_DIR"] = str((ROOT / ".cache/zig-global-cache").resolve())
    environment["ZIG_LOCAL_CACHE_DIR"] = str(output.parent / "zig-cache")
    zig_bin = str(zig / "zig")
    zig_include = str(zig / "lib/include")

    compiled = []
    sources = [(source, ["-Wall", "-Wextra", "-Werror", "-Ithird_party/libsmb2/include", "-Ithird_party/libsmb2/lib"]) for source in SOURCES]
    pinned_input("lwip", "third_party/lwip", "85a09ebe68c260d4876e800220588e5a0b624266c994cc25da978210c2813f14")
    for directory in LWIP_DIRECTORIES:
        sources += [(str(path.relative_to(ROOT)), []) for path in sorted((ROOT / directory).glob("*.c"))]
    sources.append(("third_party/lwip/src/netif/ethernet.c", []))
    # Both libsmb2 and lwIP have an init.c: libsmb2's objects get a prefix.
    sources += [(str(path.relative_to(ROOT)), SMB2_FLAGS) for path in sorted((ROOT / "third_party/libsmb2/lib").glob("*.c"))]
    for source, warnings in sources:
        prefix = "smb2-" if source.startswith("third_party/libsmb2/") else ""
        result = objects / (prefix + Path(source).stem + ".o")
        run([zig_bin, "clang", "-target", "x86_64-sie-ps5", "-O2", "-fPIC",
             "-fvisibility-nodllstorageclass=default", "-fno-stack-protector", "-fno-plt",
             "-femulated-tls", f"-ffile-prefix-map={ROOT}=."] + warnings + ["-Iinclude"] + FLAGS +
            ["-isystem", zig_include, "-isystem", str(sdk / "target/include"),
             "-c", str(ROOT / source), "-o", str(result)], environment)
        compiled.append(str(result))

    crt = build_crt(zig_bin, zig_include, sdk, objects / "crt", environment)
    # prospero-lld's flags, the -z options clang's PS5 driver adds, and crt1.o after the
    # objects, so that _start does not land at address 0.
    # The _fbsd emulation sets ELFOSABI_FREEBSD, which this clang leaves out for sie-ps5.
    run([zig_bin, "ld.lld", "-m", "elf_x86_64_fbsd", "-pie", "-T", str(sdk / SDK_LINK_SCRIPT),
         "--eh-frame-hdr", "-z", "max-page-size=0x4000", "-mllvm", "-emulated-tls",
         "-z", "now", "-z", "rodynamic", "--unresolved-symbols=report-all",
         "-L", str(sdk / "target/lib"), "-o", str(output)] + compiled + [str(crt)] + LIBS + SDK_LIBS +
        ["--hash-style=gnu"], environment)

    summary = elf_summary(output)
    # The page shows APP_VERSION (/api/info), so the package and the page agree by
    # construction; this only checks that the build picked it up.
    version = re.search(r'#define APP_VERSION "([^"]+)"', (ROOT / "include/log.h").read_text()).group(1)
    if f'"version":"{version}"'.encode() not in output.read_bytes():
        raise SystemExit(f"APP_VERSION {version} is not in the build")
    summary["version"] = version
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
