# Vita Recorder
> **Fork notes (v0.2):** records straight to **MP4** in `ux0:video/VitaRecorder/`, with real frame timing, crash recovery, lower memory use and a batch of bug fixes. See [Changes in this fork](#changes-in-this-fork).

Vita Recorder is a plugin that allows to record video clips during your play sessions.<br>
The code is based off VITA2PC at which has been applied improvements and the streaming feature has been replaced with file recording.

# Current features set
* Allows to record clips of unlimited duration (given enough free storage is available).
* Records clips as MP4 files (MJPEG video) that play in VLC, mpv, ffmpeg and most PC players, with correct playback speed.
* Allows to downscale on CPU the output to 480x272 allowing for faster transcoding.
* Performs hardware MJPEG encoding with sceJpegEncoder (using spare VRAM, or regular RAM if VRAM is full).
* Allows to perform both asynchronous and synchronous recording (The first won't affect game performances but you may end up having some artifacts or some missing frames, the latter will lower game performances but will produce frame perfect clips).
* Allows to apply frameskip on synchronous recording.

# How to install
* Put `VitaRecorder.suprx` in your `tai` folder.
* Add the plugin under a section for the game you want to use it for (eg `*GTAVCECTY`) in your `config.txt` file. (Alternatively you can place it under `*ALL` in a section where `main` is disabled (check down for an example) but some apps may crash with this due to the resources requirements).
* If you want to use this plugin on commercial games, you'll need to install [ioPlus](https://github.com/CelesteBlue-dev/PSVita-RE-tools/blob/master/ioPlus/ioPlus-0.1/release/ioplus.skprx?raw=true) as well by adding it in your `*KERNEL` section in your `config.txt`.

Here's an example of a config.txt with the plugin installed in `*ALL` section:
```
*KERNEL
ux0:tai/PSVshell.skprx
ux0:tai/AnalogsEnhancer.skprx
ux0:tai/fd_fix.skprx
ux0:tai/kubridge.skprx
#ux0:tai/gxmdbg.skprx
ux0:tai/ioplus.skprx
*ALL
ux0:tai/WDNR.suprx
*main
ux0:tai/pngshot.suprx
ur0:tai/henkaku.suprx
*!main
*ALL
ux0:tai/VitaRecorder.suprx
*NPXS10015
ur0:tai/henkaku.suprx
*NPXS10016
ur0:tai/henkaku.suprx
```

# Controls
* L + Select = Open the Config Menu
* L + Start = Start/Stop Recording (Shortcut)
* Triangle = Close Config Menu and save settings (when in Config Menu)

# Output Videos
Videos are saved in `ux0:video/VitaRecorder/` named `TITLEID_YYYY-MM-DD_HH-MM-SS.mp4`.<br>
Each frame keeps its real timestamp, so videos play back at the right speed even when the recorder can't keep up with the game.<br>
The video track is MJPEG inside MP4. It plays on PC (VLC, mpv, ffmpeg, browsers that support MJPEG), but **not in the Vita's own Video app**, which only plays H.264. To get a small H.264 file for sharing:<br>
`ffmpeg -i GTAVCECTY_2026-11-24_21-02-33.mp4 -pix_fmt yuv420p -c:v libx264 -crf 20 out.mp4`

If the game crashes while recording, a `.mp4.idx` file is left next to the video. The next time the plugin loads it finishes that video automatically (you lose at most the last ~2 seconds).

Settings are saved in `ux0:data/VitaRecorder/config.bin`.

# Changes in this fork
**New**
* MP4 output to `ux0:video/VitaRecorder/` instead of raw `.mjpg` in `ux0:data`.
* Real per-frame timing (the old `.mjpg` files played back at a fixed 25 fps).
* Crash recovery for unfinished recordings.
* Settings are remembered between launches.
* "REC Indicator" option to hide the red R (in asynchronous mode it is otherwise visible in the video).
* Recording stats (frames, skipped frames, size) in the config menu.
* GitHub Actions build (`.github/workflows/build.yml`).

**Fixes**
* Error messages crashed the game: the error buffer was an `int` array and was passed to the text renderer as a pointer. Messages now show for 5 seconds.
* Buffer overflow reading the Title ID (256 bytes requested into a 16-byte buffer).
* Encoder memory overflow: the downscaled frame was written past the end of the encoder's memory block.
* "Best"/"High" quality produced empty videos: the JPEG output buffer was too small and failed frames were written with a negative size.
* Changing quality/downscaler/recorder type while recording could free memory the encoder was still using, or record two files at once. Settings are now locked while recording.
* Synchronous mode never retried after failing to open a file, and wrote to an invalid handle.
* The config menu was recorded into synchronous videos.
* The menu only blocked the first input sample and didn't block the analog sticks.
* Text drawing had no bounds checks and could write outside the framebuffer.
* The worker thread and semaphore were never cleaned up on unload, and an in-progress recording was never closed.
* Builds again with current VitaSDK (old code relied on implicit `int` and pointer/int conversions, which newer GCC rejects).

**Optimizations**
* The encoder's memory is only allocated while recording, so games get it back the rest of the time.
* Asynchronous mode encodes each new game frame once, instead of re-encoding the same frame in a busy loop.
* Faster downscaler (half as many reads from slow video memory).
* No more 5 MB taipool heap or bundled libjpeg-turbo; the plugin uses the system's built-in functions, which lowers memory use and removes the per-game memory workarounds.

# Plans for the future
At the time of writing, the plugin is in an experimental stage. Current plan for the future is:<br>
* Add raw audio recording.
* Create a kernel plugin variant. (udcd_uvc can be used as base, the idea is to allow multiapp recordings)
* Whenever possible (mostly small homebrew apps due to the high resources requirements), stick to SceLibMp4Recorder for MP4 recording. (AVC+AAC)

# Known Issues
* If the game has no spare VRAM or RAM, recording can't start ("not enough memory"). The software encoder fallback was removed in this fork.
* Games that change resolution mid-recording: frames at a different resolution than the one at record start are skipped. (eg: Everybody's Golf)
* No audio yet.
* System apps and Livearea are not supported (plan for those is to handle them directly with the future kernel version of the plugin instead of messing with SceSharedFb in user space)

# Notes
<b>This plugin is an entry for the [KyûHEN PSVITA homebrew contest](https://kyuhen.customprotocol.com/en/).</b>

# Credits
Special thanks to the distinguished patroners for their awesome support:
- @Sarkies_Proxy
- Badmanwazzy37
- Colin VanBuren
- drd7of14
- Freddy Parra
- Max
- Tain Sueiras
- The Vita3K Project
- Titi Clash
