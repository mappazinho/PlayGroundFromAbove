#!/bin/bash
# usage: forcetest.sh <name> <midi> [renderer] [extra ini lines...]
# Forces the sliding window on (even for light frames) and compares 30 frames with the regular path, while the note
# zoom sweeps 60% -> 250% and the playback speed 70% -> 150%. Prints the compare summary and the timing lines.
cd "$(dirname "$0")/.."
name=$1; midi=$2; rend=${3:-dx11}; shift 3
SNW=2 bench/mk.sh "$name" "$midi" "$rend" fps=30 frames=300 warmup=10 start=10 nspeed=60:250 speed=70:150 dumpevery=10 "$@"
bench/run.sh "$name" 2>&1 | grep -E "^(==|frame_ms|compare|status|frame [0-9]+: DIFF)"
