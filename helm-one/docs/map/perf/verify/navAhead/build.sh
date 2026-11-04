#!/bin/sh
# 把真实的 vmap_route_nav.c 编成两个宿主机二进制（AHEAD=64 / AHEAD=32），
# 其余源码完全同一份。32 版只是把宏 sed 成 32 的临时副本（不写回仓库）。
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
VENDOR=$(cd "$HERE/../../../../.." && pwd)
VMAP="$VENDOR/boards/sf32lb52/my_vendor/ui/bicycle/src/vmap"
OUT=${OUT_DIR:-/tmp/navAhead}

mkdir -p "$OUT"
sed 's/#define VMAP_NAV_MATCH_AHEAD_SEGS   64u/#define VMAP_NAV_MATCH_AHEAD_SEGS   32u/' \
    "$VMAP/vmap_route_nav.c" > "$OUT/vmap_route_nav_ahead32.c"

grep -q 'VMAP_NAV_MATCH_AHEAD_SEGS   32u' "$OUT/vmap_route_nav_ahead32.c" || {
    echo "sed failed: AHEAD 32 variant not produced" >&2; exit 1; }

CFLAGS="-O2 -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -I$VMAP -I$HERE -I$HERE/fake"
SRCS="$HERE/nav_host.c $HERE/route_stub.c $VMAP/vmap_geo.c"

# shellcheck disable=SC2086
gcc $CFLAGS -DVMAP_NAV_SRC="\"$VMAP/vmap_route_nav.c\"" \
    -o "$OUT/nav64" $SRCS -lm
# shellcheck disable=SC2086
gcc $CFLAGS -DVMAP_NAV_SRC="\"$OUT/vmap_route_nav_ahead32.c\"" \
    -o "$OUT/nav32" $SRCS -lm

echo "built: $OUT/nav64 (AHEAD=64)  $OUT/nav32 (AHEAD=32)"
