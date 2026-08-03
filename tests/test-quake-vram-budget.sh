#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT

cat > "$fixture/trace" <<'EOF'
# 256x128 ARGB1555 is square-replicated to 256x256 (131072 bytes).
U 1 256 128 3
# 64x64 ARGB4444 (8192 bytes).
U 2 64 64 4
# Re-upload name 1 at 128x128; replacement is 32768 bytes, not cumulative.
U 1 128 128 3
D 2
EOF

output=$("$repo_root/tools/quake-vram-budget" --trace "$fixture/trace")
grep -Fq '4 events, peak 2 live textures using 139264 bytes' <<< "$output"
grep -Fq '2211840 bytes remain' <<< "$output"

# Total free bytes can exceed a request while no contiguous block can satisfy
# it. Pin the backend's first-fit fragmentation behavior, not just live sums.
cat > "$fixture/fragmented" <<'EOF'
U 1 256 256 1
U 2 256 256 1
U 3 256 256 1
U 4 256 256 1
U 5 256 256 1
U 6 256 256 1
U 7 256 256 1
D 2
D 4
U 8 512 512 2
EOF
if fragmented_output=$("$repo_root/tools/quake-vram-budget" \
    --trace "$fixture/fragmented" --width 1 --height 1 --vram-mib 2 2>&1); then
    printf 'fragmented heap unexpectedly fit\n' >&2
    exit 1
fi
grep -Fq 'cannot allocate 524288 bytes (786424 total free, 262144 largest block)' \
    <<< "$fragmented_output"

printf 'quake VRAM budget fixture test: PASS\n'
