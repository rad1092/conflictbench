#!/usr/bin/env bash
set -euo pipefail

# Linux CI only. Mount privilege is limited to this new 2 MiB tmpfs; the helper
# and all fixture I/O execute as the ordinary runner user.
if [[ "$(uname -s)" != Linux || $# != 1 ]]; then
  echo "Usage (Linux): RUNNER_TEMP=/existing/private/temp $0 /path/to/diskfull_test" >&2
  exit 2
fi
: "${RUNNER_TEMP:?RUNNER_TEMP must name the CI runner temporary directory}"
test_helper="$(realpath -e -- "$1")"
test_runner_temp="$(realpath -e -- "$RUNNER_TEMP")"
[[ -x "$test_helper" && -d "$test_runner_temp" && -w "$test_runner_temp" ]]
command -v mountpoint >/dev/null
command -v timeout >/dev/null

test_parent="$(mktemp -d -- "$test_runner_temp/conflictbench-diskfull.XXXXXX")"
test_mount="$test_parent/mount"
mkdir -- "$test_mount"

cleanup() {
  test_result=$?
  trap - EXIT HUP INT TERM
  if mountpoint -q -- "$test_mount"; then
    if ! sudo -n umount -- "$test_mount"; then
      echo "Cannot unmount owned test tmpfs: $test_mount; preserved for inspection." >&2
      exit 1
    fi
  fi
  # The underlying mount directory was empty before mounting. Do not recursively
  # delete anything if unexpected files remain or an unmount was unsuccessful.
  if ! rmdir -- "$test_mount" "$test_parent"; then
    echo "Owned test directory was not empty; preserved: $test_parent" >&2
    exit 1
  fi
  exit "$test_result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

sudo -n mount -t tmpfs -o "size=2m,nr_inodes=1024,mode=0700,uid=$(id -u),gid=$(id -g),nosuid,nodev,noexec" \
  conflictbench-diskfull "$test_mount"
mountpoint -q -- "$test_mount"
LC_ALL=C timeout --signal=TERM --kill-after=5s 30s "$test_helper" "$test_mount"
