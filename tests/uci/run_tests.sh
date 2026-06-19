#!/usr/bin/env sh
# UCI protocol transcript tests for Crafty.
# Usage (from repo root):  sh tests/uci/run_tests.sh [path-to-engine]
# Each test pipes a transcript to the engine and greps its stdout.
# Every transcript must end with "quit\n" so the engine terminates.
set -u
ENGINE="${1:-source/crafty_test.exe}"
fail=0

# expect <description> <transcript-with-\n-escapes> <egrep-pattern>
expect() {
  desc=$1; transcript=$2; pattern=$3
  out=$(printf '%b' "$transcript" | "$ENGINE" 2>/dev/null)
  if printf '%s\n' "$out" | grep -Eq "$pattern"; then
    echo "PASS: $desc"
  else
    echo "FAIL: $desc -- expected /$pattern/"
    fail=1
  fi
}

# --- Task 1: smoke (engine builds and runs) ---
expect "engine builds and runs" 'quit\n' 'Crafty v25\.2'

exit $fail
