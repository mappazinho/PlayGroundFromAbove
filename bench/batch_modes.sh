#!/bin/bash
# Dense stretch of Danidanijr's Darkness V2 under different settings; regular path vs sliding window, pixel compare.
cd "$(dirname "$0")/.."
D="F:\\midi\\Danidanijr's Darkness V2 ~ Danidanijr & Orangepaprika 67.mid"
common="fps=20 frames=120 warmup=10 start=88 dumpevery=10"
run() { # name rendererr extra...
  name=$1; shift
  bench/mk.sh "$name" "$D" dx11 $common "$@"
  echo "##### $name  ($*)"
  bench/run.sh "$name" 2>&1 | grep -E "^(==|frame_ms|compare|status|frame [0-9]+: (DIFF|no base))"
}
run hide hide=0,3,5 hideat=60:7
run dual dual=1
run horiz horizontal=1
run nokeys keys=0
run big width=1920 height=1080
run small width=640 height=360
