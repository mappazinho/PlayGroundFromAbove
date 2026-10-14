#!/bin/bash
# usage: scan.sh <name> <midi path (windows form)> [frames]
# Plays the whole song at 10 steps/s with the regular path and prints the densest 5-second stretches
# (rows/frame, wall ms), so a scenario can be started at the interesting place.
cd "$(dirname "$0")/.."
name=$1; midi=$2; frames=${3:-1500}
root='C:\Users\tempu\Desktop\pianofromabove\PlayGroundFromAbove'
cat > bench/scan_${name}.ini <<EOF
label=scan $name
midi=$midi
out=${root}\\bench_out\\scan_${name}
renderer=dx11
fps=10
frames=$frames
warmup=0
start=0
sliding=0
EOF
rm -rf bench_out/scan_${name}
(cd Release && timeout 1500 ./PGFA-v1.0.0-AVX2.exe -bench "${root}\\bench\\scan_${name}.ini")
grep -E "^(renderer|frame_ms|rows/frame)" bench_out/scan_${name}/result.txt
awk -F, 'NR>1{b=int($1/50); s[b]+=$3; n[b]++; w[b]+=$2} END{for(i in n) printf "%d %.0f %.1f\n", i*5, s[i]/n[i], w[i]/n[i]}' bench_out/scan_${name}/frames.csv | sort -k2 -n -r | head -6 | awk '{printf "song %ds..%ds: rows/frame=%s wall=%sms\n", $1, $1+5, $2, $3}'
