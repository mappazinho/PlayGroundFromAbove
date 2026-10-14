#!/bin/bash
# Regression suite: re-runs the new path (sliding window) of every scenario whose baseline frames exist in bench_out
# and prints one line per scenario: timing of the base run (from its result.txt) vs the new run, and the pixel verdict.
cd "$(dirname "$0")/.."
printf "%-10s %12s %12s  %s\n" scenario base_ms new_ms verdict
for ini in bench/*_snw.ini; do
  name=$(basename "$ini" _snw.ini)
  [ -f bench_out/${name}_base/result.txt ] || continue
  rm -rf bench_out/${name}_snw
  (cd Release && timeout 900 ./PGFA-v1.0.0-AVX2.exe -bench "C:\\Users\\tempu\\Desktop\\pianofromabove\\PlayGroundFromAbove\\bench\\${name}_snw.ini") >/dev/null 2>&1
  base=$(grep -o "frame_ms: mean=[0-9.]*" bench_out/${name}_base/result.txt | cut -d= -f2)
  new=$(grep -o "frame_ms: mean=[0-9.]*" bench_out/${name}_snw/result.txt | cut -d= -f2)
  verdict=$(grep -E "^compare:" bench_out/${name}_snw/result.txt | sed 's/ (baseline.*//')
  st=$(grep -E "^status=" bench_out/${name}_snw/result.txt)
  printf "%-10s %12s %12s  %s %s\n" "$name" "$base" "$new" "$verdict" "$st"
done
