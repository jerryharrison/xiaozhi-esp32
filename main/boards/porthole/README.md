# Skipper Porthole

The round pocket screen: a Waveshare **ESP32-S3-Touch-AMOLED-1.75C** (CO5300 466×466 QSPI AMOLED,
CST9217 touch, AXP2101, ES8311 speaker codec, ES7210 mics, 32 MB flash, 8 MB octal PSRAM) running
the Skipper Pod firmware, with Skipper's avatar on the screen instead of the stock emoji/chat UI.

It is its own board (`CONFIG_BOARD_TYPE_SKIPPER_PORTHOLE`), derived from
`waveshare/esp32-s3-touch-amoled-1.75` (1.75C pins). Board selection is compile time, so Pod builds
contain none of this code and their sdkconfig is unchanged.

## Build and flash

    main/boards/porthole/build.sh            # -> build-porthole/merged-binary.bin, xiaozhi.bin, generated_assets.bin

Config: `sdkconfig.porthole` (a defaults fragment on top of `sdkconfig.defaults`; mirrors
`config.json`). The avatar renderer is `hardware/avatar-render` in skipper-ai (this repo is the
submodule `hardware/skipper-pod/firmware`). Set `AVATAR_RENDER_DIR` to build against another
checkout; after changing it, run `idf.py -B build-porthole reconfigure` (CMake caches the path).
If Homebrew's `python3` is newer than the Python ESP-IDF was installed with (no
`~/.espressif/python_env/idf6.0_pyX.Y_env` for it), `build.sh` puts a shim for an installed
version (3.12 on the Mac Studio) first on PATH, so plain `export.sh` users need `PATH=<dir with python3 -> python3.12>:$PATH`.

First flash: `merged-binary.bin` at 0x0. Updates without losing Wi-Fi (NVS at 0x9000 is left alone):

    esptool --chip esp32s3 -p PORT -b 460800 write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m \
      0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10d000 ota_data_initial.bin \
      0x200000 xiaozhi.bin 0xa00000 generated_assets.bin

## What's on screen (`avatar_display.cc`)

| Device | Avatar state |
|---|---|
| idle, starting | `steady` |
| connecting, listening | `listening` |
| bridge `llm` emotion `thinking` (until `neutral`) | `working` |
| speaking | the bridge's mood (`steady` by default), swell driven by the playback level |
| bridge `llm` emotion = a spec state (`steady`, `working`, `listening`, `storm`, `star`, `quiet`) | that state for the rest of the turn (`star` returns to `steady` after its hold) |
| bridge `llm` emotion `timer` (leave soon) | `timer`: the tide drains over 10 s with the amber rim sweep, then `steady` |
| bridge `llm` emotion `neutral` | clears the mood (`steady` while speaking) |
| error alert, fatal error | `storm` plus the message |
| Wi-Fi setup | `quiet` plus "Join the Wi-Fi / <hotspot> / then open / 192.168.4.1" |
| power save (battery, 60 s idle) | `quiet`, dimmed, 10 fps |

The sky is true black (pixels off). Device states reach the display through `AvatarStateLed` (this
board has no LEDs, so the `Led` hooks carry states and the thinking hint), and the playback level
comes from `PortholeAudioCodec::OutputData` (RMS × 3.5 per 10 ms, like the bridge's
`avatar-stream.js`, timed to when it leaves the I2S DMA queue). UI sounds don't drive the swell.

Rendering, at full resolution (466 px) and about 30 fps (10 fps in power save):

- Rows that can change are redrawn (`avatar_dirty.h`): the horizon band, the light's reach and,
  on the rim, the timer sweep and the volume level. Each is unioned with the previous frame, and
  the frame is redrawn in full when the sea/rim colours or overlays change. Away from the horizon
  only the light's reach circle changes, so those bands send just its columns.
- Bands of 16 rows go into three internal DMA buffers. Both cores, at priority 1 (below audio,
  wake word and networking), take a band's rows one at a time. A small flush task (priority 9,
  nearly idle) hands finished bands to the QSPI panel, so neither core waits on `draw_bitmap`.
- `avatar_prepare` runs once per frame. The renderer's own optimizations are described in
  `hardware/avatar-render/README.md`.
- `tools/dirty_check.c` proves on the host that partial frames, column clipping included, equal
  full renders pixel for pixel.

Measured with a temporary state cycle in a test build: steady 29.6, listening 27.5, working 29.7,
speaking with a voice level 29.6, storm 29.6, star 29, timer 29.7, and one tap ripple at a time
25 fps. A tap every 0.5 s gives about 23 fps.

If full-resolution frames average over 55 ms, the display switches to half resolution (233 px,
pixel-doubled). Every 5 s it logs `AvatarDisplay: N fps, render X ms ...`.

## Touch

- **Tap**: ripple (`avatar_tap`).
- **Volume**: drag along the rim like a dial (every 15° clockwise = +5, counter-clockwise = −5),
  or swipe up/down in the middle (every 40 px = ±10). The rim turns amber up to the level and the
  number shows for 1.5 s. Saved to NVS (`audio/output_volume`) through the codec, like the MCP tool
  `self.audio_speaker.set_volume`, which shows the same indicator.
- **Long press** (0.7 s): battery level and volume. A small amber dot low in the sea means the
  battery is at 15% or less.

## Files

- `porthole_board.cc`: board (power, panel, touch, codec, buttons, MCP tools).
- `avatar_display.{h,cc}`, `avatar_dirty.h`: the avatar UI.
- `porthole_font.h`: generated by `tools/gen_font.py` (Montserrat, SIL OFL).
- `avatar.lf`: places the renderer in IRAM.
- `tools/dirty_check.c`: host check for partial redraws (build line at the top of the file).
