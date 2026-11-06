#!/usr/bin/env python3
"""Acquire pinned build inputs under this project only; never install system tools."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import platform
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPECS = {
    "lwip": (
        "https://codeload.github.com/lwip-tcpip/lwip/tar.gz/77dcd25a72509eb83f72b033d219b1d40cd8eb95",
        "85a09ebe68c260d4876e800220588e5a0b624266c994cc25da978210c2813f14",
        "third_party/lwip", "tar", 8 * 1024 * 1024, 16 * 1024 * 1024,
    ),
    "sdk": (
        "https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip",
        "5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c",
        ".cache/sdk-v0.45", "zip", 16 * 1024 * 1024, 128 * 1024 * 1024,
    ),
    "zig": (
        "https://ziglang.org/download/0.14.1/zig-aarch64-macos-0.14.1.tar.xz",
        "39f3dc5e79c22088ce878edc821dedb4ca5a1cd9f5ef915e9b3cc3053e8faefa",
        ".cache/zig-0.14.1", "tar", 64 * 1024 * 1024, 600 * 1024 * 1024,
    ),
}


def safe_parts(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise ValueError("unsafe archive path")
    return path.parts[1:]


def acquire(name):
    url, expected, directory, kind, maximum, unpacked_max = SPECS[name]
    target = (ROOT / directory).resolve()
    if not target.is_relative_to(ROOT):
        raise ValueError("target outside project")
    receipt = target / "OFFLINE_SOURCE.json"
    if target.exists():
        meta = json.loads(receipt.read_text())
        if meta.get("archive_sha256") != expected or meta.get("url") != url:
            raise ValueError("existing dependency identity mismatch: " + str(target))
        print(name + ": existing pinned input " + str(target))
        return
    if name == "zig" and (platform.system(), platform.machine()) != ("Darwin", "arm64"):
        raise RuntimeError("pinned Zig binary is macOS arm64 only; use a reviewed native toolchain")
    request = urllib.request.Request(url, headers={"User-Agent": "tom-ps5-usbnet-build"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(maximum + 1)
    if len(data) > maximum or hashlib.sha256(data).hexdigest() != expected:
        raise ValueError("archive size/digest mismatch: " + name)
    rows = []
    if kind == "zip":
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for entry in archive.infolist():
                parts = safe_parts(entry.filename)
                if ((entry.external_attr >> 16) & 0o170000) == 0o120000:
                    raise ValueError("symlink not admitted")
                if entry.is_dir() or not parts:
                    continue
                if entry.file_size > unpacked_max:
                    raise ValueError("oversized member")
                rows.append(("/".join(parts), archive.read(entry)))
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
            for entry in archive.getmembers():
                parts = safe_parts(entry.name)
                if entry.issym() or entry.islnk():
                    raise ValueError("archive links not admitted")
                if not entry.isfile() or not parts:
                    continue
                if name == "lwip" and parts[0] != "src" and "/".join(parts) not in (
                    "COPYING", "README", "README.md", "UPGRADING"
                ):
                    continue
                if entry.size > unpacked_max:
                    raise ValueError("oversized member")
                rows.append(("/".join(parts), archive.extractfile(entry).read()))
    if len(rows) > 30000 or sum(len(content) for _, content in rows) > unpacked_max:
        raise ValueError("extraction budget exceeded")
    if len({path for path, _ in rows}) != len(rows):
        raise ValueError("duplicate members")
    for path, content in rows:
        output = (target / path).resolve()
        if not output.is_relative_to(target):
            raise ValueError("output outside dependency")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as handle:
            handle.write(content)
        if (name == "zig" and path == "zig") or (name == "sdk" and path.startswith("bin/")):
            output.chmod(0o755)
    receipt.write_text(json.dumps({
        "name": name, "url": url, "archive_sha256": expected,
        "archive_bytes": len(data), "files": len(rows), "directory": str(target),
    }, indent=2) + "\n")
    print(name + ": acquired verified input " + str(target))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in SPECS:
        parser.add_argument("--" + name, action="store_true")
    args = parser.parse_args()
    for name in SPECS:
        if getattr(args, name):
            acquire(name)


if __name__ == "__main__":
    main()
