# Third-party code and programs in PotentialCrates

PotentialCrates is a fork of **Crate Digger** by sd88me ([sd88me/mpc-vst-cratedigger](https://github.com/sd88me/mpc-vst-cratedigger), MIT). Its full history is kept in this repo.

## In the source

| Part | Where | Licence |
|---|---|---|
| Crate Digger's streaming core (Discogs search, yt-dlp resolve, ffmpeg decode) | `src/core/yt_stream_plugin.c`, `gain_state.h`, `yt_dlp_daemon.py` | MIT: the Crate Digger contributors (`LICENSE`) and Charles Vestal's [schwung-webstream](https://github.com/charlesvestal/schwung-webstream) (`UPSTREAM_LICENSE.txt`, `UPSTREAM_THIRD_PARTY_NOTICES.md`) |
| The plugin interface the core is written to | `src/core/plugin_api_v1.h` | MIT (as above) |
| Discogs filter tables | `src/cratedig_tables.h` | MIT (as above) |
| The engine build scripts | `scripts/` | MIT (as above) |
| The VST2 wrapper and skin tools (not in this repo: fetched at build time) | [open-mpc/mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins) | MIT |

## In the release zips (the engine bundle, `Content/engine/bin`)

These programs are copied unchanged from Crate Digger 2.0.0's release by `tools/fetch_engine.sh`, which checks them against pinned SHA-256 sums. Crate Digger builds them with the scripts in `scripts/`.

| Program | Licence | Source |
|---|---|---|
| yt-dlp | The Unlicense (`Content/engine/LICENSES/yt-dlp-UNLICENSE.txt`) | <https://github.com/yt-dlp/yt-dlp> |
| ffmpeg, ffprobe | GPL-3.0 (`Content/engine/LICENSES/GPL-3.0.txt`): the "gpl" static builds of yt-dlp's FFmpeg-Builds | <https://github.com/yt-dlp/FFmpeg-Builds> (each build's FFmpeg source is published there); <https://ffmpeg.org/legal.html> |
| Python 3.11 | PSF License (`Content/engine/bin/python3/lib/python3.11/LICENSE.txt`) | <https://www.python.org/> |

ffmpeg runs as a separate program; PotentialCrates' own code is not linked with it.

PotentialCrates isn't affiliated with or endorsed by Akai Professional, Discogs or YouTube. Searches go to Discogs, and playback is streamed through yt-dlp: use it within those services' terms.
