#!/usr/bin/env bash
# Sign the exact JSON bytes, not a parsed/reformatted representation.
# The PEM private key is provided by trusted release infrastructure.
set -euo pipefail
if [[ $# -ne 2 || ! -s "$1" || ! -r "$2" ]]; then
  echo "Usage: $0 <windows-x64.json> <rsa-private-key.pem>" >&2
  exit 2
fi
manifest=$1
private_key=$2
signature="${manifest}.sig"
public_key=$(mktemp)
trap 'rm -f "$public_key"' EXIT
openssl dgst -sha256 -sign "$private_key" -out "$signature" "$manifest"
openssl pkey -in "$private_key" -pubout -out "$public_key" 2>/dev/null
openssl dgst -sha256 -verify "$public_key" -signature "$signature" "$manifest" >/dev/null
test -s "$signature"
