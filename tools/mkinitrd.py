#!/usr/bin/env python3
"""
Build a tiny CPIO "newc" archive for the EnlilOS embedded initrd.

Usage:
    tools/mkinitrd.py OUTPUT \
        README.TXT=initrd/README.TXT \
        BOOT.TXT=initrd/BOOT.TXT \
        dir:dev dir:data dir:sysroot \
        INIT.ELF=user/nsh.elf
"""

from __future__ import annotations

import os
import stat
import sys
from pathlib import Path


def align4(data: bytes) -> bytes:
    pad = (-len(data)) & 3
    if pad:
        data += b"\0" * pad
    return data


def hex8(value: int) -> bytes:
    return f"{value:08x}".encode("ascii")


def newc_entry(name: str, mode: int, data: bytes) -> bytes:
    namesize = len(name.encode("utf-8")) + 1
    header = b"".join(
        [
            b"070701",
            hex8(0),
            hex8(mode),
            hex8(0),
            hex8(0),
            hex8(1),
            hex8(0),
            hex8(len(data)),
            hex8(0),
            hex8(0),
            hex8(0),
            hex8(0),
            hex8(namesize),
            hex8(0),
        ]
    )
    body = header + name.encode("utf-8") + b"\0"
    body = align4(body)
    body += data
    return align4(body)


def parse_spec(spec: str) -> tuple[str, int, bytes]:
    if spec.startswith("dir:"):
        name = spec[4:]
        if not name:
            raise ValueError("directory name missing")
        return name, stat.S_IFDIR | 0o755, b""

    if "=" not in spec:
        raise ValueError(f"invalid spec: {spec}")
    name, src = spec.split("=", 1)
    if not name or not src:
        raise ValueError(f"invalid spec: {spec}")
    data = Path(src).read_bytes()
    return name, stat.S_IFREG | 0o644, data


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: mkinitrd.py OUTPUT SPEC...", file=sys.stderr)
        return 1

    output = Path(argv[1])
    entries = [parse_spec(spec) for spec in argv[2:]]
    archive = bytearray()

    for name, mode, data in entries:
        archive += newc_entry(name, mode, data)
    archive += newc_entry("TRAILER!!!", stat.S_IFREG, b"")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(archive)
    print(f"[mkinitrd] wrote {output} ({len(archive)} bytes, {len(entries)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
