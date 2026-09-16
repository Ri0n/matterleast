#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 \"16 32 48 256\" input.svg" >&2
    exit 1
fi

exec python3 "$(dirname "$0")/svgToIco.py" "$@"
