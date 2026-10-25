#!/bin/sh
# Show an ELF file's machine, byte order, interpreter, and dynamic dependencies.
set -eu

if [ "$#" -ne 1 ]; then
    printf 'Usage: %s ELF_FILE\n' "$0" >&2
    exit 2
fi

ELF_FILE=$1
if [ ! -r "$ELF_FILE" ]; then
    printf 'Cannot read: %s\n' "$ELF_FILE" >&2
    exit 1
fi

FILE_TOOL=${FILE_TOOL:-file}
"$FILE_TOOL" "$ELF_FILE"

READELF=${READELF:-}
if [ -z "$READELF" ]; then
    for candidate in nto-readelf readelf llvm-readelf; do
        if command -v "$candidate" >/dev/null 2>&1; then
            READELF=$(command -v "$candidate")
            break
        fi
    done
fi

if [ -z "$READELF" ] || ! command -v "$READELF" >/dev/null 2>&1; then
    printf 'No ELF reader found; set READELF to nto-readelf, readelf, or llvm-readelf.\n' >&2
    exit 127
fi

printf '\n%s\n' 'ELF header:'
"$READELF" -h "$ELF_FILE"
printf '\n%s\n' 'Program headers (including PT_INTERP):'
"$READELF" -l "$ELF_FILE"
printf '\n%s\n' 'Dynamic section (including NEEDED libraries):'
"$READELF" -d "$ELF_FILE"
