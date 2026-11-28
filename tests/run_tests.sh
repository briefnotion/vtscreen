#!/bin/sh
# Build the tests with the strict flags and the sanitizers, and run them.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/vtscreen_tests.$$"
mkdir -p "$OUT"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$OUT/retained_tests" retained_tests.cpp
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$OUT/attr_tests" attr_tests.cpp
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$OUT/image_tests" image_tests.cpp
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -o "$OUT/sixel_tests" sixel_tests.cpp
# The tests of drawings made of shapes need vtpen's headers and its tests/regis_decode.h: in $VTPEN, or next to this repository.
VTPEN="${VTPEN:-../../vtpen}"
if [ -f "$VTPEN/include/vtpen_regis.h" ] && [ -f "$VTPEN/tests/regis_decode.h" ]; then
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -I"$VTPEN/include" -I"$VTPEN/tests" -o "$OUT/vector_tests" vector_tests.cpp
    VECTOR_TESTS=yes
else
    echo "vtpen not found (set VTPEN to its folder): the vector tests are skipped"
    VECTOR_TESTS=no
fi
"$OUT/retained_tests"
"$OUT/attr_tests"
"$OUT/image_tests"
"$OUT/sixel_tests"
if [ "$VECTOR_TESTS" = yes ]; then "$OUT/vector_tests"; fi
rm -f "$OUT/retained_tests" "$OUT/attr_tests" "$OUT/image_tests" "$OUT/sixel_tests" "$OUT/vector_tests"
rmdir "$OUT"
