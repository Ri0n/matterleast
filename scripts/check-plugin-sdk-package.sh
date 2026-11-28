#!/usr/bin/env bash
# Audit the actual SDK payload and build a plugin against only that payload.
set -euo pipefail
test "$#" -eq 3
format=$1
artifact=$(realpath "$2")
build_dir=$3
libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:PATH=//p' "$build_dir/CMakeCache.txt")
test -n "$libdir"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage="$work/stage"
mkdir -p "$stage"
case "$format" in
  deb)
    test "$(dpkg-deb -f "$artifact" Package)" = matterleast-dev
    depends=$(dpkg-deb -f "$artifact" Depends)
    grep -Eq '^qt(6-base|base5)-dev([[:space:]]|$)' <<< "$depends"
    dpkg-deb -x "$artifact" "$stage"
    ;;
  rpm)
    test "$(rpm -qp --qf '%{NAME}' "$artifact")" = matterleast-devel
    requires=$(rpm -qpR "$artifact")
    grep -Eq '^qt[56]-qtbase-devel([[:space:]]|$)' <<< "$requires"
    (cd "$stage" && rpm2cpio "$artifact" | cpio -id --quiet)
    ;;
  *) exit 2 ;;
esac
sdk_dir="usr/$libdir/cmake/MatterLeastPluginApi"
# The only payload files should be the existing public API headers and
# the three CMake package files, with no symlinks or application binaries.
{
  (cd plugins/api && find . -type f -name '*.h' -printf 'usr/include/matterleast/plugin/%P\n')
  printf '%s\n' "$sdk_dir/MatterLeastPluginApiConfig.cmake" \
    "$sdk_dir/MatterLeastPluginApiConfigVersion.cmake" \
    "$sdk_dir/MatterLeastPluginApiTargets.cmake"
} | LC_ALL=C sort > "$work/expected"
(cd "$stage" && find . \( -type f -o -type l \) -printf '%P\n' | LC_ALL=C sort) > "$work/actual"
diff -u "$work/expected" "$work/actual"
cp -R tests/plugin-sdk-smoke "$work/source"
cmake -S "$work/source" -B "$work/build" \
    -DCMAKE_PREFIX_PATH="$stage/usr" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
grep -Fxq "MatterLeastPluginApi_DIR:PATH=$stage/$sdk_dir" "$work/build/CMakeCache.txt"
cmake --build "$work/build" --parallel 2
echo "Verified $format plugin SDK manifest and external build"
