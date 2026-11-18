# Building PotentialCrates

You don't need to build it to use it: download a zip from [Releases](../../releases). This is for people who want to change it.

Everything the release does is in `.github/workflows/release.yml`. By hand, on Linux or WSL with `gcc`, `curl`, `unzip`, Python 3 and `pip install ziglang pillow numpy playwright` (then `python -m playwright install chromium`):

```
git clone https://github.com/open-mpc/mpc-vst-plugins kit
git -C kit checkout 52c1fb072966d7de77e97dd9eec0544548b042dc
git -C kit apply ../mpc-vst-potentialcrates/shared/mpc-vst-plugins-label-fit.patch
K=$PWD/kit; cd mpc-vst-potentialcrates

# the three looks' skins and params.h
mkdir -p vst/build
gcc -O2 -I $K/tools/vendor/force-shadow/tools -o vst/build/shadow_art $K/tools/shadow_art.c -lm
python3 shared/looks/build_looks.py vst $K

# tests on your computer (AddressSanitizer)
bash $K/tools/test_port.sh vst/vst.json
gcc -fsanitize=address,undefined -g -O1 -std=gnu11 -DYT_POSIX_SPAWN -I src -I src/core -I vst/build -I $K/wrapper \
  tests/test_crates.c src/core/yt_stream_plugin.c -lm -lpthread -o vst/build/test_crates && vst/build/test_crates

# the engine bundle (yt-dlp, Python, ffmpeg) for both CPUs
bash tools/fetch_engine.sh

# the plugin for a Gen1 MPC (32-bit ARM); Gen2: -target aarch64-linux-gnu.2.31 -mcpu=cortex_a55
python3 -m ziglang cc -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a9 -mfpu=neon -O2 -fPIC -shared -fvisibility=hidden \
  -std=gnu11 -DYT_POSIX_SPAWN -I src -I src/core -I vst/build -I $K/wrapper src/crates.c src/core/yt_stream_plugin.c \
  $K/wrapper/vst2_wrap.c -lm -lpthread -Wl,--no-undefined -Wl,-s -o vst/build/potentialcrates.so

# the installer zip, with the engine
python3 $K/tools/release.py --so vst/build/potentialcrates.so --skin "vst/build/skin/PotentialBreaks - VST - PotentialCrates" \
  --entry vst/build/pluginlist-entry.xml --version X.Y.Z --id potentialcrates --license MIT \
  --extra vst/build/engine/armv7/Content:Content --user-data channels.txt -o dist
```

**Notes**
- **Parameters are append-only.** `vst/params.lock` keeps saved projects working, so add new parameters at the end of the list.
- **The looks:** `vst/layout.base.conf` is the only layout to edit. `shared/looks/make_looks.py` turns it into the three looks' layouts. A button used on more than one page shares one picture, so give it the same size everywhere.
- **The core** (`src/core/`) is Crate Digger's. Keep changes there small and marked, so fixes from Crate Digger can be brought over.
- **The engine bundle** comes from Crate Digger's release, pinned by checksum in `tools/fetch_engine.sh`. Crate Digger builds it with `scripts/` (Docker).
- **The MANUAL page:** `vst/manual.txt` is the on-screen manual. A line `= NAME` starts a page (NAME is its button), `# text` is a heading, `- text` a bullet. `shared/looks/make_looks.py` turns it into the MANUAL tab in each look and stops the build if a page doesn't fit. The page names must match the options of the parameter `manual_page` in `vst/params.json`, in order. `shared/potential_manual.h` holds the page number in the engine; it isn't part of the saved state.
