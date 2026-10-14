#!/bin/bash
# usage: run.sh <name> [base]  : runs <name>_base (unless "snwonly") then <name>_snw and prints the key lines
cd "$(dirname "$0")/.."
name=$1
modes="base snw"
[ "$2" = "snwonly" ] && modes="snw"
for mode in $modes; do
  rm -rf bench_out/${name}_${mode}
  (cd Release && timeout 900 ./PGFA-v1.0.0-AVX2.exe -bench "C:\\Users\\tempu\\Desktop\\pianofromabove\\PlayGroundFromAbove\\bench\\${name}_${mode}.ini")
  echo "== $name $mode"
  grep -E "^(renderer|zoom|frame_ms|phase_ms|rows/frame|compare|status)" bench_out/${name}_${mode}/result.txt
done
grep -E "DIFFERENT|no baseline" bench_out/${name}_snw/result.txt | head -5
