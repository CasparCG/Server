#!/bin/bash

# Verify CMake's CEF selection with synthetic packages and no CEF downloads or compilation.
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT

check_selection() {
  local name="$1" expected="$2"
  shift 2
  # Some cases deliberately pass package hints that the selected policy must ignore.
  cmake --no-warn-unused-cli -S "$script_dir/cef-selection" -B "$test_dir/$name" \
    -DEXPECTED_CEF_SOURCE="$expected" "$@"
  echo "PASS: $name"
}

check_selection ppa-priority ppa \
  "-Dcasparcg-cef-api15000_DIR=$script_dir/cef-selection" -DTEST_CEF_VERSION=154
check_selection system-fallback system \
  -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON -DTEST_CEF_VERSION=150
check_selection newer-system system \
  -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON -DTEST_CEF_VERSION=154
check_selection old-system-download download \
  -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON -DTEST_CEF_VERSION=146
check_selection missing-system-download download \
  -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON
# Reconfigure the same cache to confirm AUTO can discover a newly available package.
check_selection missing-system-download system \
  -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON -DTEST_CEF_VERSION=150
check_selection explicit-download download -DUSE_SYSTEM_CEF=OFF \
  "-Dcasparcg-cef-api15000_DIR=$script_dir/cef-selection" -DTEST_CEF_VERSION=154
check_selection legacy-boolean download -DUSE_SYSTEM_CEF=0
check_selection explicit-system system -DUSE_SYSTEM_CEF=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON -DTEST_CEF_VERSION=150
if cmake --no-warn-unused-cli -S "$script_dir/cef-selection" -B "$test_dir/required-system" \
    -DUSE_SYSTEM_CEF=ON -DCMAKE_DISABLE_FIND_PACKAGE_casparcg-cef-api15000=ON \
    -DTEST_CEF_VERSION=146; then
  echo "FAIL: explicit system CEF must reject incompatible versions" >&2
  exit 1
fi
echo "PASS: explicit system CEF rejects incompatible versions"
