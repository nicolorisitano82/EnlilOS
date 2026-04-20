#!/usr/bin/env python3
"""
Stage a fixed Linux AArch64 userland bundle for EnlilOS M11-05.

Usage:
    tools/stage_linux_compat.py LINUX_ROOT_DIR OUT_DIR
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


BINARIES = [
    ("bash", "usr/bin/bash"),
    ("env", "usr/bin/env"),
    ("uname", "usr/bin/uname"),
    ("pwd", "usr/bin/pwd"),
    ("ls", "usr/bin/ls"),
    ("cat", "usr/bin/cat"),
    ("echo", "usr/bin/echo"),
    ("mkdir", "usr/bin/mkdir"),
    ("rm", "usr/bin/rm"),
    ("ln", "usr/bin/ln"),
    ("sleep", "usr/bin/sleep"),
    ("test", "usr/bin/test"),
    ("curl", "usr/bin/curl"),
]

ETC_FILES = [
    "etc/hostname",
    "etc/hosts",
    "etc/passwd",
    "etc/group",
    "etc/os-release",
    "etc/nsswitch.conf",
    "etc/ld.so.conf",
    "etc/resolv.conf",
]

INTERPRETERS = [
    "lib/ld-linux-aarch64.so.1",
    "lib/ld-musl-aarch64.so.1",
    "lib64/ld-linux-aarch64.so.1",
]

LIB_DIRS = [
    "lib",
    "usr/lib",
    "lib/aarch64-linux-gnu",
    "usr/lib/aarch64-linux-gnu",
]


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def copy_file(src: Path, dst: Path) -> None:
    ensure_parent(dst)
    shutil.copy2(src.resolve(), dst)


def first_existing(root: Path, candidates: list[str]) -> Path | None:
    for rel in candidates:
        path = root / rel
        if path.exists():
            return path
    return None


def find_binary(root: Path, name: str) -> Path | None:
    candidates = [
        root / "usr/bin" / name,
        root / "bin" / name,
        root / "usr/sbin" / name,
        root / "sbin" / name,
    ]
    for path in candidates:
        if path.exists():
            return path
    return None


def stage_libraries(root: Path, out_dir: Path) -> list[str]:
    copied: list[str] = []
    seen: set[str] = set()

    for rel_dir in LIB_DIRS:
        src_dir = root / rel_dir
        if not src_dir.is_dir():
            continue
        for path in sorted(src_dir.rglob("*")):
            if not path.is_file():
                continue
            name = path.name
            if ".so" not in name:
                continue
            rel = path.relative_to(root)
            rel_key = rel.as_posix()
            if rel_key in seen:
                continue
            seen.add(rel_key)
            copy_file(path, out_dir / rel)
            copied.append(rel_key)
    return copied


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: stage_linux_compat.py LINUX_ROOT_DIR OUT_DIR", file=sys.stderr)
        return 1

    root = Path(argv[1]).resolve()
    out_dir = Path(argv[2]).resolve()

    if not root.is_dir():
        print(f"linux root missing: {root}", file=sys.stderr)
        return 1

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    copied: list[str] = []

    for name, dest_rel in BINARIES:
        src = find_binary(root, name)
        if src is None:
            print(f"[stage-linux-compat] warning: missing binary {name}", file=sys.stderr)
            continue
        copy_file(src, out_dir / dest_rel)
        copied.append(dest_rel)

    for rel in ETC_FILES:
        src = root / rel
        if src.exists():
            copy_file(src, out_dir / rel)
            copied.append(rel)

    interp = first_existing(root, INTERPRETERS)
    if interp is not None:
        rel = interp.relative_to(root)
        copy_file(interp, out_dir / rel)
        copied.append(rel.as_posix())
    else:
        print("[stage-linux-compat] warning: no AArch64 dynamic linker found", file=sys.stderr)

    copied.extend(stage_libraries(root, out_dir))

    manifest = out_dir / ".manifest.txt"
    manifest.write_text("".join(f"{item}\n" for item in sorted(set(copied))), encoding="utf-8")
    (out_dir / ".staged").write_text("linux-compat-stage-v1\n", encoding="utf-8")

    print(f"[stage-linux-compat] staged {len(set(copied))} files into {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
