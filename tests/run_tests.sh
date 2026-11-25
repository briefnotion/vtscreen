#!/bin/sh
# Build the tests with the strict flags and the sanitizers, and run them.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/vtscreen_tests.$$"
mkdir -p "$OUT"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$OUT/retained_tests" retained_tests.cpp
"$OUT/retained_tests"
rm -f "$OUT/retained_tests"
rmdir "$OUT"
