#!/bin/bash
# Forced-window correctness sweep over several real MIDIs (regular path vs sliding window, pixel compare).
cd "$(dirname "$0")/.."
run() { # name file start
  echo "##### $1"
  bench/forcetest.sh "$1" "F:\\midi\\$2" dx11 start=$3 2>&1 | tail -8
}
run bpm "BPM=RT Uncut Fix.mid" 30
run truthful "Truthful.mid" 15
run miami "Miami Hotline Vol. 3 F1 (FIXED).mid" 40
run hrk "HRK'S LAG TESTER 5.mid" 20
