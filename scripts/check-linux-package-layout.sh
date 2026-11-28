#!/usr/bin/env bash
# Audit the actual staged Linux payload, not merely CMake's install rules.
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <deb|rpm|appdir> <package-or-AppDir> <cmake-build-dir>" >&2
  exit 2
fi

format=$1
artifact=$2
build_dir=$3
libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:PATH=//p' "$build_dir/CMakeCache.txt")
prefix=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$build_dir/CMakeCache.txt")
if [[ "$prefix" != "/usr" || -z "$libdir" || "$libdir" = /* ]]; then
  echo "Expected /usr prefix and a relative GNUInstallDirs libdir, got prefix='$prefix' libdir='$libdir'" >&2
  exit 1
fi

plugin="usr/$libdir/matterleast/plugins/seasonal-snow.so"
sdk="usr/$libdir/cmake/MatterLeastPluginApi/MatterLeastPluginApiTargets.cmake"

case "$format" in
  deb|appdir)
    triplet=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
    if [[ "$libdir" != "lib/$triplet" ]]; then
      echo "Expected Debian multiarch lib/$triplet, got $libdir" >&2
      exit 1
    fi
    if [[ "$format" = deb ]]; then
      stage=$(mktemp -d)
      trap 'rm -rf "$stage"' EXIT
      dpkg-deb -x "$artifact" "$stage"
    else
      stage=$artifact
    fi
    test -f "$stage/$plugin"
    if [[ "$format" = deb ]]; then
      test ! -e "$stage/$sdk"
      test ! -e "$stage/usr/include/matterleast/plugin"
    else
      test -f "$stage/$sdk"
    fi
    if [[ -d "$stage/usr/bin/plugins" || -d "$stage/usr/lib/cmake/MatterLeastPluginApi" ]]; then
      echo "Legacy bin/plugins or non-multiarch CMake SDK directory is still packaged" >&2
      exit 1
    fi
    ;;
  rpm)
    expected_libdir=$(rpm --eval '%{_libdir}')
    if [[ "/usr/$libdir" != "$expected_libdir" ]]; then
      echo "Expected Fedora library directory $expected_libdir, got /usr/$libdir" >&2
      exit 1
    fi
    files=$(rpm -qpl "$artifact")
    grep -Fxq "/$plugin" <<< "$files"
    if grep -Eq '/cmake/MatterLeastPluginApi/|/include/matterleast/plugin/' <<< "$files"; then
      echo "Plugin SDK leaked into runtime RPM" >&2
      exit 1
    fi
    if grep -Eq '^/usr/bin/plugins/|^/usr/lib/cmake/MatterLeastPluginApi/' <<< "$files"; then
      echo "Legacy RPM plugin/SDK location is still packaged" >&2
      exit 1
    fi
    ;;
  *)
    echo "Unknown Linux package format: $format" >&2
    exit 2
    ;;
esac

echo "Verified $format runtime layout: /$plugin"
