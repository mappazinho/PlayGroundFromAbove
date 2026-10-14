#!/bin/bash
# Fallback modes (tick based, corruptor) and DX12 strip modes; regular path vs sliding window, pixel compare.
cd "$(dirname "$0")/.."
D="F:\\midi\\Danidanijr's Darkness V2 ~ Danidanijr & Orangepaprika 67.mid"
common="fps=20 frames=100 warmup=10 start=88 dumpevery=10"
run() { # name renderer extra...
  name=$1; rend=$2; shift 2
  bench/mk.sh "$name" "$D" $rend $common "$@"
  echo "##### $name  ($rend $*)"
  bench/run.sh "$name" 2>&1 | grep -E "^(==|frame_ms|compare|status|frame [0-9]+: (DIFF|no base))"
}
run tick dx11 tick=1
run corrupt dx11 corrupt=0.25
run tickh dx11 tick=1 horizontal=1
run corrh dx11 corrupt=0.25 horizontal=1
run h12 dx12 horizontal=1
run d12 dx12 dual=1 nspeed=100:250
