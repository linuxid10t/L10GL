#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT

quake_root="$fixture/L10GL-Quake"
winquake="$quake_root/WinQuake"
pak="$winquake/id1/pak0.pak"
binary="$winquake/glquake.l10gl"
output="$fixture/results"
mkdir -p "$winquake/id1"
printf 'PACK' > "$pak"
truncate -s 18689235 "$pak"

cat > "$binary" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' 'GL_RENDERER: L10GL/swrast'
printf '%s\n' 'Video mode 320x240 (16bpp) initialized.'
printf '%s\n' 'Q13: brush textures through 128x128 use picmip 1; large/model textures retain picmip 2'
case " $* " in
    *' +timedemo demo1 '*)
        capture=${L10GL_SWRAST_DUMP//%04d/0001}
        : > "$capture"
        printf '%s\n' 'U 1 64 64 3' > "$L10GL_TEXTURE_TRACE"
        printf '%s\n' '969 frames   7.3 seconds 132.9 fps'
        ;;
    *)
        printf '%s\n' 'U 1 64 64 3' 'D 1' 'U 2 64 64 3' > "$L10GL_TEXTURE_TRACE"
        printf '%s\n' '[02]the Slipgate Complex'
        printf '%s\n' 'Q13: map transition released 1 mipmapped textures'
        printf '%s\n' '[02]Castle of the Damned'
        printf '%s\n' 'Q13: map transition released 1 mipmapped textures'
        printf '%s\n' '[02]the Slipgate Complex'
        printf '%s\n' 'CL_SignonReply: 4'
        ;;
esac
trap 'exit 0' TERM
while :; do sleep 1; done
EOF
chmod +x "$binary"

output_log=$(L10GL_QUAKE_BIN="$binary" "$repo_root/tools/quake-swrast-gate" \
    --quake-dir "$quake_root" --output-dir "$output" --skip-fetch --skip-build --timeout 5 2>&1)
grep -Fq 'PASS: demo1 reported 969 frames' <<< "$output_log"
grep -Fq 'PASS: E1M1-to-E1M2-to-E1M1 completed sign-on' <<< "$output_log"
grep -Fq 'ViRGE Q10 budget:' <<< "$output_log"
[[ -f "$output/quake.log" ]]
[[ -f "$output/frame0001.ppm" ]]
[[ -f "$output/textures.tsv" ]]
[[ -f "$output/transition.log" ]]
[[ -f "$output/transition-textures.tsv" ]]

printf 'quake swrast gate fixture test: PASS\n'
