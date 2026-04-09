#!/bin/sh
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ENLILOS_ROOT=$(CDPATH= cd -- "$SELF_DIR/../.." && pwd)
ENLILOS_SYSROOT="$ENLILOS_ROOT/toolchain/sysroot"

find_prefixed_tool() {
    suffix=$1

    if [ "${ENLILOS_CROSS:-}" != "" ] && command -v "${ENLILOS_CROSS}${suffix}" >/dev/null 2>&1; then
        command -v "${ENLILOS_CROSS}${suffix}"
        return 0
    fi
    if [ "${CROSS:-}" != "" ] && command -v "${CROSS}${suffix}" >/dev/null 2>&1; then
        command -v "${CROSS}${suffix}"
        return 0
    fi

    for prefix in aarch64-elf- aarch64-none-elf- aarch64-linux-gnu-; do
        if command -v "${prefix}${suffix}" >/dev/null 2>&1; then
            command -v "${prefix}${suffix}"
            return 0
        fi
    done

    echo "enlilos-toolchain: missing ${suffix} cross tool" >&2
    exit 1
}

ENLILOS_CC=$(find_prefixed_tool gcc)
ENLILOS_AR=$(find_prefixed_tool ar)
ENLILOS_RANLIB=$(find_prefixed_tool ranlib)
