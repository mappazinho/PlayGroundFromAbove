## ⚠️ BETA Release - You WILL experience crashes or random bugs.
### Public standalone BETA release of PGFA - v0.10.0 for Windows

Two builds are provided for each CPU level: the normal build, and a **Packed** build that packs big MIDIs by default (see below).

What's new since v0.9.0:

**MIDI loading**
* Much faster MIDI parser (parallel two-pass track parsing and a k-way merge for post-processing)
* Fixed loading hanging at the end of parsing, or failing outright after loading another MIDI
* Big files are memory-mapped instead of copied into RAM (up to about 8.8 GB less at peak on huge files)
* Much lower memory use while loading: smaller event rows, 16-bit note-off offsets, merge instead of sort
* Identical notes (same track, channel, key, velocity, start and end) are stored once and repeated by the player
* Row times are computed from ticks when a file has only a few tempo changes, instead of being stored
* Fixed keys staying stuck after polyphony bombs

**Packed mode (experimental)**
* The whole song is kept in a compact packed form (about 0.1 - 1.2 bytes per note, e.g. a 3.3 GB / 433M-note MIDI packs to 42 MB)
* Only a window of the song around the playhead is expanded for the renderer and audio, and the window slides along as you play or seek
* Enable with `PGFA_PACKED=1`; the Packed builds turn it on by default for MIDIs of 100 MB or more (`PGFA_PACKED=0` turns it off, `PGFA_PACKED_MIN_MB` changes the threshold)
* `PGFA_PACKED_NOTES` sets how many notes a window holds (default 12,000,000)

**Rendering and GPU stability**
* Fixed a GPU hang (the whole PC freezing for a few seconds, then nothing drawing) when switching between fullscreen and windowed on some NVIDIA RTX cards
* Fixed DirectX 12 recovery after a driver reset: no more falling back to DirectX 11 or failing with "Error calling DirectX"
* Fixed a crash in the recovery path when it ran twice in a row
* Fixed a crash when the GPU upload buffer could not be mapped while loading a very large MIDI
* Fixed stale blur and bloom texture states after a resize or fullscreen switch
* Device-removed reasons and the GPU operation that hung are now written to the log
* New build option `PGFA_FORCE_LOGGING` makes a diagnostic build that always logs; the normal builds keep the "Enable logging" checkbox and leave it off by default

**UI**
* Redesigned start screen recent-files card with a blurred background
* Small text is crisper at lower window sizes (768p)
* Captions now appear just above the piano keyboard instead of covering the toolbar

**Fixes**
* Fixed an error shown in `crash_log.txt` on closing the player
* Hardened key drawing against invalid per-key note indices (suspected cause of a rare crash during playback)
* Synths (OmniMIDI, SVMS and other KDMAPI providers) are no longer unloaded once loaded. Unloading them could crash inside `MMDevAPI.dll` the next time an audio device changed
* New always-on `startup_log.txt` next to the exe, covering launch up to the first MIDI loading and playing
* The crash dialog is now a short summary; the full report (stack, registers, loaded and recently unloaded modules) stays in `crash_log.txt`. The dialog links to GitHub Issues and asks you to turn on logging if it is off
* The crash dialog's buttons are now "Quit" and "Try to continue" (no more Retry). "Try to continue" actually continues: a crash in the player goes back to the start screen, and a crash on another thread stops only that thread
* Fixed the DirectX 11 renderer failing to recreate its swap chain (E_ACCESSDENIED, falling back to the slower bitblt mode) after a device reset
* Fixed the "Transition" keys mode stuttering on dense MIDIs with the visual (image) buffer on: every frame of the 88 -> 128 key animation threw away the prepared note data and restarted preparing the whole song in the background
* Fixed notes being cut off diagonally in DirectX 11 with the visual (image) buffer on: the DirectX 11 chunk shader was an outdated build that drew each chunk as an oversized triangle over the chunk below it

Known issues:
* Packed mode: the picture freezes for about 0.3 - 0.4 s each time the window moves to the next section, and the audio probably has a short gap there
* Packed mode: markers, lyrics, track names and controller state from before a window are not carried over, and some statistics (notes played, etc.) restart per window
