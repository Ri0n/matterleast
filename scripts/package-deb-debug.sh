#!/usr/bin/env bash
# Split debug symbols from a finished Debian package into a matching
# matterleast-dbgsym package. Requires binutils, dpkg-deb and file.
set -euo pipefail

if [[ $# != 2 ]]; then
  echo "Usage: $0 <runtime.deb> <dbgsym.deb>" >&2
  exit 2
fi
runtime_deb=$1
debug_deb=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
runtime="$work/runtime"
debug="$work/debug"
mkdir -p "$runtime" "$debug/DEBIAN"
dpkg-deb --raw-extract "$runtime_deb" "$runtime"

version=$(dpkg-deb -f "$runtime_deb" Version)
arch=$(dpkg-deb -f "$runtime_deb" Architecture)
package=$(dpkg-deb -f "$runtime_deb" Package)
test "$package" = matterleast

count=0
while IFS= read -r -d '' binary; do
  if ! file -b "$binary" | grep -q '^ELF '; then
    continue
  fi
  # Limit symbol extraction to MatterLeast's own executable and its plugins;
  # dependencies from the distribution have separate distro debug packages.
  relative=${binary#"$runtime"}
  case "$relative" in
    /usr/bin/matterleast|/usr/bin/plugins/*.so|/usr/lib/matterleast/plugins/*.so) ;;
    *) continue ;;
  esac

  target="$debug/usr/lib/debug$relative.debug"
  mkdir -p "$(dirname "$target")"
  objcopy --only-keep-debug "$binary" "$target"
  strip --strip-debug "$binary"
  objcopy --add-gnu-debuglink="$target" "$binary"
  test -s "$target"
  count=$((count + 1))
done < <(find "$runtime" -type f -print0)

if (( count < 1 )); then
  echo "No MatterLeast ELF executables found for debug-symbol extraction" >&2
  exit 1
fi

cat >"$debug/DEBIAN/control" <<EOF
Package: matterleast-dbgsym
Version: $version
Architecture: $arch
Maintainer: Sergei Ilinykh <rion4ik@gmail.com>
Section: debug
Priority: optional
Depends: matterleast (= $version)
Description: Debug symbols for MatterLeast
 Separate debug information for MatterLeast's native executables and plugins.
EOF

# We modified the executable bytes after CPack generated its checksums.
# Regenerate md5sums so dpkg verification of the installed package succeeds.
(
  cd "$runtime"
  find . -type f ! -path './DEBIAN/*' -printf '%P\0' |
    sort -z |
    xargs -0 -r md5sum > DEBIAN/md5sums
)

dpkg-deb --build --root-owner-group "$runtime" "$runtime_deb"
dpkg-deb --build --root-owner-group "$debug" "$debug_deb"
test "$(dpkg-deb -f "$debug_deb" Version)" = "$version"
test "$(dpkg-deb -f "$debug_deb" Architecture)" = "$arch"
echo "Produced $debug_deb with debug symbols for $count ELF files"
