## PlayGroundFromAbove (PGFA) v1.0.0 for Windows

The executables are now named `PGFA-v1.0.0-AVX2.exe` and `PGFA-v1.0.0-SSE42.exe` (they were `PGFA-Beta-*`).

What's new since the "prerender settings window" build:

**Sliding note window (replaces Visual Buffer Notes)**
* New renderer path for very dense MIDIs: every key keeps a persistent list of its notes, and each frame only the notes that actually own a screen row are drawn. It is exactly the set the occlusion filter keeps, found without visiting the notes that are completely hidden
* Pixel-identical to the regular path (checked on many MIDIs, zoom levels, speeds, seeks, hidden tracks, DirectX 11 and 12) and roughly 8 - 11x faster on dense main-roll frames
* The dual and horizontal piano rolls read their sampled notes out of a global window too, 6 - 8x faster
* Turns itself on for frames with 150k or more event rows in view and falls back to the regular path for tick mode, the corruptor, bent notes and warp
* Fun menu checkbox "Sliding note window (fast dense MIDIs)"; `PGFA_SLIDING=0|1|2` overrides it
* Visual Buffer Notes is retired: old configs that had it on use the new path, the old code is dormant
* Dense frames also build their notes on several threads, and the occlusion filter runs in parallel

**Synth output: SuperVirtualMIDISynth**
* `SVMSAPI.dll` next to the exe is used for KDMAPI output (native session with exact-timestamp batching where the DLL supports it)
* New "KDMAPI Synth" dropdown in Settings > Audio: SuperVirtualMIDISynth or OmniMIDI (default SuperVirtualMIDISynth, falls back to the other if it is missing). Takes effect on restart, saved as `KDMAPISynth` in the config
* Free play, the splash MIDI, normal playback and the startup check all use the chosen provider; OmniMIDI is no longer force-loaded at startup
* `SVMS.dll` is no longer tried as a provider (it is a BASS shim without a KDMAPI API)
* Every song now starts from a freshly reset synth (all controllers, programs, volume, pan, reverb, bend range), so nothing carries over from the previous song

**Start screen**
* Close File now goes back to the full splash screen (it used to show an empty placeholder state)
* The playback toolbar is hidden on the splash screen
* With the Horizontal renderer, the splash no longer draws the horizontal strip over its own notes (DualRoll had the same problem)
* The ribbon bloom glow now shows along the bottom edge of the splash screen, where its notes land

**Free play**
* Much lighter at high note rates: heavy at 50k NPS before, now fine up to around 600k NPS
  * Release info is kept in a flat array instead of a hash map, the colour table is no longer rescanned every frame, expired notes are removed in one pass per key, and the per-key note building and scroll-off run on several threads
  * Each of the 16 channels of a colour track is its own colour slot, so about 1M notes can be on screen with their own colours instead of 65,536. Slots are reused oldest first, which keeps the colour upload small
  * Event rows of notes that scrolled off are recycled, so memory no longer grows for as long as you play (a per-note list that was never read or cleared is gone too)
* Audio timing: the free play scheduler mixed raw QPC ticks with microseconds, which made every scheduled note fire the moment it was queued. Timing is now in real microseconds
* The repeater (SLAM and held keys with a repeat rate) is fired by the audio thread on its own clock, so a lagging frame only delays the visuals and the notes you hear stay on time. The Audio FPS readout stays high while it runs
* Notes are sent to the audio thread in one batch per frame (one lock and one wake) instead of one per event

**UI**
* Redesigned playback toolbar: drawn transport icons, Vol / Speed / Notes sliders that carry their own labels (double-click resets to 100%), a seek bar that scales with the window, and a time readout
* "Bounce window to the beat" (with a hop height setting) hops the OS window on the beat, next to the existing stats bounce
* The stats Time line no longer runs past the song length after a lag spike (it could show 2:50.7 / 2:50.1)
* Library entries saved by older builds with mangled non-ASCII characters in their names are repaired when they are loaded

**Prerender audio**
* The prerender synth and audio device are created in the background while the MIDI parses, instead of freezing the first frame
* Seeking inside audio that is already rendered only moves the read position, with no restart or rebuild
* Resuming from pause keeps playing the audio that was already generated, and rebuilds only if the clock moved
* The song clock is held until prerender audio is actually playing, so the first notes no longer vanish and reappear
* Fixed the audio ring being reallocated while the generator thread was writing into it

**Stability and diagnostics**
* New shutdown watchdog: if closing hangs for 10 seconds, a "PGFA is not closing" dialog shows the last step reached and the stack of every thread, writes `shutdown_hang_log.txt`, and offers to terminate
* Teardown is logged step by step
* The shared text conversion buffers are thread-local (they were shared between threads)
* Per-frame cost breakdown with `PGFA_FRAME_BREAKDOWN=1`, and a measurement-only note-train statistic with `PGFA_NOTE_RUN_STATS=1` (both off by default)

**For developers**
* New headless benchmark: `PGFA-v1.0.0-AVX2.exe -bench <script.ini>` loads one MIDI with default settings (your config is untouched, no audio), steps a fixed clock, writes frame-time percentiles and per-phase times, and can dump frames and compare them pixel by pixel against a baseline. Scripts and scenarios are in `bench/` (see `bench/README.md`)

Known issues:
* Free play: above roughly 350k NPS the oldest released notes lose their colour slot, so old notes can change colour
* Free play: notes played with the mouse or keyboard are still queued from the frame loop, so a very long lag frame can still delay them
* The File menu's Close File and Render to Video items look enabled on a fresh launch with no song loaded
* GPU Overclock Artifacts is driven only by a real frame rate below 60 FPS, so it does not show in light free play
