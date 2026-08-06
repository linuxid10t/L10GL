#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT

quake_root="$fixture/L10GL-Quake"
winquake="$quake_root/WinQuake"
pak="$winquake/id1/pak0.pak"
binary="$winquake/glquake.l10gl"
runner="$fixture/l10gl-run"
runner_log="$fixture/runner.log"
output="$fixture/results"

mkdir -p "$winquake/id1"
printf 'PACK' > "$pak"
truncate -s 18689235 "$pak"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$binary"
chmod +x "$binary"

cat > "$runner" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%q ' "$@" >> "$L10GL_Q13_RUNNER_LOG"
printf '\n' >> "$L10GL_Q13_RUNNER_LOG"
printf '%s\n' 'l10gl-run: selected virge at 0000:01:00.0'
printf '%s\n' 'GL_RENDERER: L10GL/virge'
printf '%s\n' 'Video mode 640x480 (16bpp) initialized.'
printf '%s\n' 'S3 ViRGE: P6 native mode: 640x480@60 RGB555, exact built-in timing'
printf '%s\n' 'S3 ViRGE: presentation: synchronized double buffer (L10GL_VSYNC=1)'
printf '%s\n' 'Q12: ViRGE RGBA alpha lightmaps active'
printf '%s\n' 'Q13: brush textures through 128x128 use picmip 1; large/model textures retain picmip 2'
printf '%s\n' 'S3 ViRGE: OpenGL texture sampling uses u*N-0.5 texel centers'
if [[ -n ${L10GL_KBD_DEV:-} ]]; then
    printf 'in_l10gl: keyboard device %s\n' "$L10GL_KBD_DEV"
    printf '%s\n' 'in_l10gl: keyboard input via stdin'
fi

case " $* " in
    *' +map e1m1 '*)
        if [[ ${L10GL_Q13_FIXTURE_PLAY_SIGNAL:-0} == 1 ]]; then
            printf '%s\n' 'Received signal 11, exiting...'
            exit 0
        fi
        printf '%s\n' 'SpawnServer: e1m1'
        printf '%s\n' 'SpawnServer: e1m2'
        ;;
    *' +timedemo demo1 '*)
        printf '%s\n' '969 frames   230.7 seconds 4.2 fps'
        printf '%s\n' 'Received signal 2, exiting...'
        ;;
    *)
        printf '%s\n' 'fixture: missing run discriminator' >&2
        exit 2
        ;;
esac
EOF
chmod +x "$runner"

output_log=$(printf '%s\n' yes yes yes yes | \
    SUDO_TTY=/dev/tty1 \
    L10GL_Q13_RUNNER_LOG="$runner_log" \
    "$repo_root/tools/quake-virge-gate" \
        --quake-dir "$quake_root" --output-dir "$output" --runner "$runner" \
        --skip-fetch --skip-build 2>&1)

grep -Fq 'PASS: E1M1 transitioned to E1M2' <<< "$output_log"
grep -Fq 'PASS: demo1 reported 969 frames in 230.7s (4.2 fps)' <<< "$output_log"
grep -Fq 'Q13 Phase 7 ViRGE acceptance: PASS' "$output/q13-report.txt"
grep -Fq 'timedemo demo1: 969 frames in 230.7 seconds (4.2 fps)' \
    "$output/q13-report.txt"
grep -Fq 'keyboard VT: /dev/tty1' "$output/q13-report.txt"
grep -Fq -- '-width 640 -height 480 -bpp 16' "$runner_log"
grep -Fq -- 'L10GL_MODESET=native L10GL_REFRESH=60 L10GL_VSYNC=1' "$runner_log"
grep -Fq -- 'L10GL_KBD_DEV=/dev/tty1' "$runner_log"
grep -Fq -- '+map e1m1' "$runner_log"
grep -Fq -- '+timedemo demo1' "$runner_log"
[[ -L "$output/run/id1/pak0.pak" ]]

signal_output="$fixture/signal-results"
set +e
signal_log=$(printf '%s\n' yes | \
    SUDO_TTY=/dev/tty1 \
    L10GL_Q13_RUNNER_LOG="$runner_log" \
    L10GL_Q13_FIXTURE_PLAY_SIGNAL=1 \
    "$repo_root/tools/quake-virge-gate" \
        --quake-dir "$quake_root" --output-dir "$signal_output" \
        --runner "$runner" --skip-fetch --skip-build 2>&1)
signal_status=$?
set -e
[[ $signal_status -ne 0 ]]
grep -Fq "play run ended via 'Received signal 11, exiting...'" \
    <<< "$signal_log"

set +e
non_vt_log=$(env -u L10GL_RUNNER -u SUDO_TTY \
    "$repo_root/tools/quake-virge-gate" \
        --quake-dir "$quake_root" --skip-fetch --skip-build </dev/null 2>&1)
non_vt_status=$?
set -e
[[ $non_vt_status -ne 0 ]]
grep -Fq 'must be invoked from a physical Linux VT (/dev/ttyN)' \
    <<< "$non_vt_log"

set +e
stale_binary_log=$(SUDO_TTY=/dev/tty1 \
    "$repo_root/tools/quake-virge-gate" \
        --quake-dir "$quake_root" --skip-fetch --skip-build \
        </dev/null 2>&1)
stale_binary_status=$?
set -e
[[ $stale_binary_status -ne 0 ]]
grep -Fq 'binary lacks sudo-proxy keyboard input support' \
    <<< "$stale_binary_log"
! grep -Fq 'must be invoked from a physical Linux VT' \
    <<< "$stale_binary_log"

printf 'quake virge gate fixture test: PASS\n'
