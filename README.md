## ⚠️ Note: Currently in BETA stage. You WILL experience bugs or crashes.
If you experience a crash or bug, and know how to replicate it then follow these steps.
1. Enable logging in Settings > Viz
2. Restart PGFA
3. Replicate bug
4. Create a new issue in this repo, explain how it happened and include logs such as:
   * **pfa_heartbeat** - located in %temp%
   * **pfa_msgs** - located in %temp%
   * **pfa_prerender** - located in %temp%
   * **crash_log** - located in root folder where PGFA lives      
*If you are unable to launch PGFA due to a startup crash, then you only need to include **startup_log**, which also exists in the root folder of PGFA.*

# PlayGroundFromAbove
*Absolutely diabolical.*

A modification of PFAviz by khang06 which contains further fixes and additions, such as:
* IMGUI UI for a simpler look
* Visual Buffers
* Bloom and blur shaders with controls
* DirectX11 Backport, and software rendering support
* Memory efficient multi-threaded parsing
* Prerendered audio
* Fixed note visuals
* Dual piano roll modes
* And more...

## Recommended Requirements
You will need a GPU that is *at least* better than a GTX 700 series card, and at LEAST supports DirectX11.

CPU, RAM, etc is kept the same as stock PFA.

## How to build

* Clone this repo
* Download and install Visual Studio 2026
  * Make sure to install the Clang compiler and tools!
* Download and install Direct X SDK
* Retarget project to your installed Windows SDK version
* Cross fingers
* Build! (Release, x64)

## Credits

[PianoFromAbove](https://github.com/brian-pantano/PianoFromAbove)

[PFAviz](https://github.com/khang06/PianoFromAbove)

[PFAViz-PreRendered-Audio](https://github.com/MrCHB1/PFAViz-PreRendered-Audio) - used for prerender audio here, modified

[Comet](https://github.com/MrCHB1/Comet) - based off the memory efficient parsing here

**BPFA** - inspiration for visual buffer mode






