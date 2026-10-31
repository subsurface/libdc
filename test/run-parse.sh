#!/bin/sh
# AI-generated (Claude)
#
# run-parse.sh - Parse fixture harness for libdivecomputer
#
# Reads test/fixtures/manifest.txt, runs 'dctool parse' over each raw blob
# using the exact model descriptor recorded in the manifest, and exits
# non-zero if any invocation fails.
#
# When run under an ASAN/UBSAN build (as wired into the 'sanitizers' CI job)
# the sanitizer environment variables cause any detected error to abort the
# process, which surfaces here as a non-zero exit from dctool.
#
# Usage: test/run-parse.sh [path-to-dctool]
#   If no path is given the script searches for 'dctool' under examples/.
#
# Copyright (C) 2024 The libdivecomputer contributors
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.

set -e

# Locate the source root (the directory containing this script's parent).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURE_DIR="$SCRIPT_DIR/fixtures"
MANIFEST="$FIXTURE_DIR/manifest.txt"

# Locate dctool.
if [ -n "$1" ]; then
    DCTOOL="$1"
elif [ -x "$ROOT_DIR/examples/dctool" ]; then
    DCTOOL="$ROOT_DIR/examples/dctool"
elif [ -x "$ROOT_DIR/examples/.libs/dctool" ]; then
    DCTOOL="$ROOT_DIR/examples/.libs/dctool"
else
    echo "ERROR: cannot find dctool under examples/. Build the project first or pass its path as an argument." >&2
    exit 1
fi

echo "Using dctool: $DCTOOL"

if [ ! -f "$MANIFEST" ]; then
    echo "ERROR: manifest not found: $MANIFEST" >&2
    exit 1
fi

FAILURES=0

while IFS='|' read -r filename vendor product; do
    # Skip blank lines and comments.
    case "$filename" in
        ''|\#*) continue ;;
    esac

    blob="$FIXTURE_DIR/$filename"
    descriptor="$vendor $product"

    if [ ! -f "$blob" ]; then
        echo "ERROR: fixture not found: $blob" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi

    echo "Parsing $filename with descriptor '$descriptor' ..."
    if "$DCTOOL" -d "$descriptor" parse -o /dev/null "$blob"; then
        echo "  OK"
    else
        echo "  FAILED (exit $?)" >&2
        FAILURES=$((FAILURES + 1))
    fi
done < "$MANIFEST"

if [ "$FAILURES" -ne 0 ]; then
    echo "ERROR: $FAILURES fixture(s) failed to parse." >&2
    exit 1
fi

echo "All fixtures parsed successfully."
