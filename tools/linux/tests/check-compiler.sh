#!/bin/bash

# Verify compiler retention, fallback installation and error propagation without installing packages.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/../check-compiler"

test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT

# Simulate configure/link outcomes and record whether the compiler is probed again.
cmake() {
  printf '%s\n' "$*" >> "$test_dir/cmake-calls"
  if [[ "$probe_state" == link-failure && "$1" != --build ]]; then
    return 0
  fi
  if [[ "$probe_state" == working ||
        ( "$probe_state" == missing && -f "$test_dir/installed" ) ]]; then
    return 0
  fi
  echo "Simulated compiler failure"
  return 1
}

install_compiler() {
  printf '%s\n' "$*" >> "$test_dir/package-calls"
  if [[ "$probe_state" == install-failure ]]; then
    return 1
  fi
  touch "$test_dir/installed"
}

apt-get() { install_compiler apt-get "$@"; }
pacman() { install_compiler pacman "$@"; }
dnf() { install_compiler dnf "$@"; }
zypper() { install_compiler zypper "$@"; }

run_case() {
  local name="$1" expected_status="$2" expected_install="$3"
  rm -f "$test_dir/installed" "$test_dir/package-calls" "$test_dir/cmake-calls"
  local status=0
  # Run independently so errexit remains active inside the installer.
  (
    set -e
    ensure_build_compiler
  ) > "$test_dir/output" 2>&1 &
  local pid=$!
  wait "$pid" || status=$?
  local actual_status=failure
  if [[ "$status" == 0 ]]; then
    actual_status=success
  fi
  if [[ "$actual_status" != "$expected_status" ]]; then
    cat "$test_dir/output"
    echo "FAIL: $name (exit $status)" >&2
    exit 1
  fi
  if [[ "$expected_install" == none ]]; then
    test ! -f "$test_dir/package-calls"
  else
    grep -Fx -- "$expected_install" "$test_dir/package-calls"
    test "$(wc -l < "$test_dir/package-calls")" -eq 1
  fi
  echo "PASS: $name"
}

unset CC CXX
ID=debian
probe_state=working
run_case "usable default compiler is kept" success none
export CC=clang CXX=clang++
run_case "usable explicit alternative is kept" success none
probe_state=broken
run_case "broken explicit compiler is not overridden" failure none
probe_state=link-failure
run_case "explicit compiler with a linker failure is rejected" failure none
probe_state=broken
unset CC
run_case "CXX alone is respected" failure none
unset CXX
export CC=broken-cc
run_case "CC alone is respected" failure none
unset CC

probe_state=missing
for ID in ubuntu debian arch fedora opensuse-leap opensuse-tumbleweed; do
  case "$ID" in
    ubuntu|debian) expected="apt-get install -yq --no-install-recommends gcc g++" ;;
    arch) expected="pacman -S --noconfirm --needed gcc" ;;
    fedora) expected="dnf -y --setopt=install_weak_deps=False install gcc gcc-c++" ;;
    opensuse-*) expected="zypper --non-interactive install --no-recommends gcc gcc-c++" ;;
  esac
  run_case "$ID installs a missing compiler and rechecks it" success "$expected"
  test "$(grep -c -- '-S ' "$test_dir/cmake-calls")" -eq 2
done

ID=debian
probe_state=broken
run_case "still-broken default compiler reports failure" failure \
  "apt-get install -yq --no-install-recommends gcc g++"
probe_state=install-failure
run_case "package installation failure is propagated" failure \
  "apt-get install -yq --no-install-recommends gcc g++"
test "$(grep -c -- '-S ' "$test_dir/cmake-calls")" -eq 1
