#!/bin/bash
# usage: mk.sh <name> <midi> <renderer> <extra lines...>   -> writes bench/<name>_base.ini and bench/<name>_snw.ini
# (base = regular path, snw = sliding note window compared against the base run's frames)
name=$1; midi=$2; rend=$3; shift 3
root='C:\Users\tempu\Desktop\pianofromabove\PlayGroundFromAbove'
cd "$(dirname "$0")/.."
for mode in base snw; do
  f=bench/${name}_${mode}.ini
  {
    echo "label=$name $mode"
    echo "midi=$midi"
    echo "out=${root}\\bench_out\\${name}_${mode}"
    echo "renderer=$rend"
    if [ $mode = base ]; then echo "sliding=0"; else echo "sliding=${SNW:-1}"; echo "baseline=${root}\\bench_out\\${name}_base"; fi
    for l in "$@"; do echo "$l"; done
  } > $f
done
