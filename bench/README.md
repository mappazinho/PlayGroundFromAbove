# PGFA bench: headless, deterministic performance and pixel tests

`PGFA-v1.0.0-AVX2.exe -bench <script.ini>` loads one MIDI with **default settings** (the user's `config.xml` is neither
read nor written, no audio device is opened), plays it with a **fixed time step** (no clock is read), writes a few small
text files and exits. One run = one command, one `result.txt` to read. No screenshots to page through, no log digging.

## Quick start

```bash
bench/mk.sh  mytest 'F:\midi\song.mid' dx11 fps=60 frames=300 start=40 dumpevery=30   # writes mytest_base.ini + mytest_snw.ini
bench/run.sh mytest                                                                   # runs both, prints the key lines
bench/run.sh mytest snwonly                                                           # re-run only the new path against the old baseline frames
bench/scan.sh dani 'F:\midi\song.mid'                                                 # finds the densest stretches of a song
bench/forcetest.sh x 'F:\midi\song.mid' dx11 start=20                                 # forces the sliding window on for light frames too
```

`_base` renders with the regular note path (`sliding=0`) and dumps frames; `_snw` renders with the sliding note window
and **compares every dumped frame pixel by pixel** against the base run (`baseline=`), saving `fNNNNN_diff.png` for any
frame that differs. `status=OK` + `compare: N identical, 0 different` is the pass condition.

## Script keys (`key=value`, `#` comments)

| key | meaning | default |
|---|---|---|
| `midi` | file to load (required) | |
| `out` | output directory (required) | |
| `renderer` | `dx11` or `dx12` | `dx12` |
| `width`, `height` | render window client size | 1280x720 |
| `fps` | time step per frame = 1/fps song seconds (x speed). Use the real 60 for realistic per-frame event counts, 10-20 to scan quickly | 60 |
| `frames`, `warmup` | frames to run, frames excluded from the statistics | 300, 30 |
| `start` | song second of frame 0 | 0 |
| `nspeed=a[:b]` | note zoom in percent, constant or swept linearly over the run | 100 |
| `speed=a[:b]` | playback speed in percent, constant or swept | 100 |
| `jump=frame:sec` | seek to a song second at a frame (repeatable) | |
| `dump=1,5,9` / `dumpevery=N` | frames saved as `fNNNNN.png` + `.bgra` (raw) and hashed | none |
| `hashevery=N` | hash every Nth frame into `hashes.txt` | 0 |
| `baseline=<dir>` | compare dumped frames with the raw frames of an earlier run | |
| `sliding=0/1/2` | regular path / sliding window / forced sliding window (also for light frames). Unset = settings default | unset |
| `dual=1`, `horizontal=1` | piano roll mode | off |
| `keys=0` | hide the keyboard | on |
| `ui=1` | draw menu/toolbar/stats (off keeps frames deterministic) | off |
| `hide=0,3`, `hideat=frame:track` | hide tracks from the start / mid-run | |
| `label` | free text shown in the result | |

## Outputs (in `out`)

* `result.txt` - settings, `frame_ms` (mean / p50 / p95 / p99 / max), per-phase averages
  (`logic`, `advance`, `notes` -> `collect`, `endscene`, `present`), `snw_ms` (sync / cull / emit),
  rows per frame, compare lines, `status=`.
  A failed load writes `status=LOAD_FAILED` and exits with code 3 (never a dialog).
* `frames.csv` - per frame: wall ms, event rows scanned, whether the frame included a screenshot.
* `hashes.txt` - 64-bit hash of every captured frame.
* `fNNNNN.png` / `.bgra` / `_diff.png` - captured frames (view the png with any image viewer), raw frames for compares,
  diff images (white = differing pixels).

## Notes

* A run is deterministic: same script, same pixels (alpha is ignored). Frames with a screenshot are excluded from the timing.
* Phase times are the CPU time spent in that part of the frame; `present` includes waiting for the GPU.
* `PGFA_SLIDING=0|1|2` in the environment overrides the settings checkbox (the bench sets it from `sliding=`).
* `PGFA_FRAME_BREAKDOWN=1` (outside the bench) appends the same per-phase averages once a second to `%TEMP%\pfa_framebd.log`.
* `PGFA_NOTE_RUN_STATS=1` logs how many note rows sit in constant-spacing "note trains" (measurement only).
* Keep scenario scripts in this folder; outputs go to `bench_out\` (git-ignored).
