#!/bin/sh
# Usage: tests/automation/run.sh <FlyPlayer> <project> <script.auto> [extra FlyPlayer args]
# Exits non-zero if the run fails an `expect`/`compare` check or does not finish.
player=$1; project=$2; script=$3; shift 3
out=$("$player" -play "$project" --noloading --res 640x360 --fixed-dt 0.016667 --log-stdout --automation "$script" "$@" 2>&1)
status=$?
printf '%s\n' "$out" | grep -E '^\[automation\]'
[ $status -eq 0 ] || { echo "automation run failed (exit $status)"; exit 1; }
printf '%s\n' "$out" | grep -q '^\[automation\] finished' || { echo "automation run did not finish"; exit 1; }
