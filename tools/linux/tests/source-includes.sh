#!/bin/bash

# Verify source scanning and CI baseline invalidation using an isolated fixture tree.
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT
mkdir "$test_dir/src"

inventory() {
  bash "$script_dir/../source-includes" "$test_dir/src"
}

cat > "$test_dir/src/first.cpp" <<'EOF'
#include <vector>
#include "local.h"
// #include <commented.h>
/*
#include <block-commented.h>
*/
const char* example = "/*";
# include /* explanation */ <span> // explanation
#include \
  <thread>
#include PLATFORM_HEADER
int main() { return 0; }
EOF
expected="$(printf '%s\n' '"local.h"' '<span>' '<thread>' '<vector>' 'PLATFORM_HEADER')"
baseline="$(inventory)"
if [[ "$baseline" != "$expected" ]]; then
  printf 'FAIL: include inventory mismatch\nExpected:\n%s\nActual:\n%s\n' "$expected" "$baseline" >&2
  exit 1
fi
echo "PASS: comments, whitespace, continuations and macro includes"

echo 'int unrelated_function() { return 42; }' >> "$test_dir/src/first.cpp"
test "$(inventory)" = "$baseline"
echo "PASS: ordinary source changes keep the include inventory"

printf '#include <vector>\n#include "local.h"\n' > "$test_dir/src/second.hpp"
test "$(inventory)" = "$baseline"
echo "PASS: already-used headers do not change the include inventory"

echo '#include <new-library/api.h>' >> "$test_dir/src/second.hpp"
test "$(inventory)" != "$baseline"
echo "PASS: a new header dependency changes the include inventory"

printf '#include <vector>\n' > "$test_dir/src/second.hpp"
test "$(inventory)" = "$baseline"
echo "PASS: removing a newly introduced include restores the inventory"

echo '#include <docs-only.h>' > "$test_dir/src/README.md"
test "$(inventory)" = "$baseline"
echo "PASS: documentation is excluded"

# Exercise the real fingerprint with host tool/package queries but no package installation.
mkdir -p "$test_dir/tools/linux" "$test_dir/src/CMakeModules" "$test_dir/.github/workflows"
cp "$script_dir/../dependency-fingerprint" "$script_dir/../source-includes" "$test_dir/tools/linux/"
touch "$test_dir/.github/workflows/linux-system.yml"

fingerprint() {
  bash "$test_dir/tools/linux/dependency-fingerprint"
}

baseline_fingerprint="$(fingerprint)"
echo 'int another_function() { return 7; }' >> "$test_dir/src/first.cpp"
test "$(fingerprint)" = "$baseline_fingerprint"
echo '#include <vector>' >> "$test_dir/src/second.hpp"
test "$(fingerprint)" = "$baseline_fingerprint"
echo "PASS: full fingerprint stays stable for ordinary edits and existing includes"

echo '#include <another-library/api.h>' >> "$test_dir/src/second.hpp"
test "$(fingerprint)" != "$baseline_fingerprint"
echo "PASS: a new header changes the full-build cache fingerprint"

cat > "$test_dir/src/version.hpp" <<'EOF'
#define REQUIRED_AVCODEC 61
#if LIBAVCODEC_VERSION_MAJOR < REQUIRED_AVCODEC
int old_api();
#else
int new_api();
#endif
EOF
baseline_fingerprint="$(fingerprint)"
echo 'int ordinary_change();' >> "$test_dir/src/version.hpp"
echo '#include <vector>' >> "$test_dir/src/version.hpp"
test "$(fingerprint)" = "$baseline_fingerprint"
echo "PASS: existing includes and ordinary edits preserve version-condition fingerprint"

cat > "$test_dir/src/version.hpp" <<'EOF'
#define REQUIRED_AVCODEC 62
#if LIBAVCODEC_VERSION_MAJOR < REQUIRED_AVCODEC
int old_api();
#else
int new_api();
#endif
EOF
test "$(fingerprint)" != "$baseline_fingerprint"
echo "PASS: dependency version macro changes invalidate the full-build baseline"
baseline_fingerprint="$(fingerprint)"

cat > "$test_dir/src/version.hpp" <<'EOF'
#define REQUIRED_AVCODEC 62
#if LIBAVCODEC_VERSION_MAJOR >= REQUIRED_AVCODEC
int old_api();
#else
int new_api();
#endif
EOF
test "$(fingerprint)" != "$baseline_fingerprint"
echo "PASS: changing a version comparison invalidates the full-build baseline"
baseline_fingerprint="$(fingerprint)"

cat >> "$test_dir/src/version.hpp" <<'EOF'
// #if UNRELATED_COMMENT
/*
#define COMMENTED_OUT 123
*/
EOF
test "$(fingerprint)" = "$baseline_fingerprint"
echo "PASS: commented-out directives do not change the fingerprint"

cat > "$test_dir/src/order.hpp" <<'EOF'
#if LIBAVUTIL_VERSION_MAJOR >= 60
#define USE_NEW_API 1
#else
#define USE_NEW_API 0
#endif
EOF
baseline_fingerprint="$(fingerprint)"
cat > "$test_dir/src/order.hpp" <<'EOF'
#if LIBAVUTIL_VERSION_MAJOR >= 60
#define USE_NEW_API 0
#else
#define USE_NEW_API 1
#endif
EOF
test "$(fingerprint)" != "$baseline_fingerprint"
echo "PASS: directive order matters even when the unique directive set is unchanged"

cat > "$test_dir/src/multiline.hpp" <<'EOF'
#if LIBAVCODEC_VERSION_MAJOR >= 61 && \
    LIBAVUTIL_VERSION_MAJOR >= 60
#endif
EOF
baseline_fingerprint="$(fingerprint)"
cat > "$test_dir/src/multiline.hpp" <<'EOF'
#if LIBAVCODEC_VERSION_MAJOR >= 61 && \
    LIBAVUTIL_VERSION_MAJOR >= 61
#endif
EOF
test "$(fingerprint)" != "$baseline_fingerprint"
echo "PASS: multiline version-condition changes invalidate the baseline"
