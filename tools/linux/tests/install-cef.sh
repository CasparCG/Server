#!/bin/bash

# Verify CEF package priority, version filtering and download policy using fake package managers.
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/../install-cef"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT

# Supply package metadata and record install requests without modifying the host.
apt-cache() {
  if [[ "$2" == "$available_package" ]]; then
    echo "  Candidate: $available_version"
  else
    echo "  Candidate: (none)"
  fi
}
apt-get() { echo "apt-get $*" >> "$test_dir/calls"; }
add-apt-repository() { echo "add-apt-repository $*" >> "$test_dir/calls"; }
pacman() {
  if [[ "$1" == -Si ]]; then
    echo "Version : $available_version"
  else
    echo "pacman $*" >> "$test_dir/calls"
  fi
}
dnf() {
  if [[ "$1" == -q ]]; then
    [[ "$*" == "-q list --showduplicates cef-devel" ]] || return 1
    echo "cef-devel.x86_64 $available_version rawhide"
  else
    echo "dnf $*" >> "$test_dir/calls"
  fi
}
zypper() {
  if [[ "$2" == info ]]; then
    if [[ "$3" == "$available_package" ]]; then
      echo "Version : $available_version"
    fi
  else
    echo "zypper $*" >> "$test_dir/calls"
  fi
}

check_install() {
  local expected="$1"
  : > "$test_dir/calls"
  install_cef
  if [[ "$expected" == none ]]; then
    test ! -s "$test_dir/calls"
  else
    grep -F -- "$expected" "$test_dir/calls"
  fi
  echo "PASS: $ID CEF $available_version ($expected)"
}

system_cef=AUTO
ID=ubuntu
available_package=casparcg-cef-api15000-dev
available_version=150.0.22
check_install "apt-get install -yq --no-install-recommends casparcg-cef-api15000-dev"
grep -Fx 'add-apt-repository -y ppa:casparcg/ppa' "$test_dir/calls"
ID=debian
check_install "apt-get install -yq --no-install-recommends casparcg-cef-api15000-dev"
available_package=libcef-dev
check_install "apt-get install -yq --no-install-recommends libcef-dev"
available_version=146.0
check_install none
available_package=none
check_install none
for ID in arch fedora opensuse-leap opensuse-tumbleweed; do
  available_version=2:154.0.1-1
  available_package=cef-devel
  case "$ID" in
    arch) expected="pacman -S --noconfirm --needed cef" ;;
    fedora) expected="dnf -y --setopt=install_weak_deps=False install cef-devel" ;;
    opensuse-*) expected="zypper --non-interactive install --no-recommends cef-devel" ;;
  esac
  check_install "$expected"
  available_version=146.0
  check_install none
done
ID=ubuntu
system_cef=OFF
check_install none
