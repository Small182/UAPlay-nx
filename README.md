# Uaflix for Nintendo Switch — native homebrew

Native controller-first app on **libnx + SDL2 + libmpv**, using TMDB movie/series metadata and a native HTTP adapter for [UAFresh](https://uakino.ofreshofresh.workers.dev/uakino.js). **No Chromium, WebKit, CUB sync or TorrServer.**

## Install

1. Open [GitHub Actions](https://github.com/Small182/Lampa-nx/actions) and download the latest successful **Uaflix-native** artifact.
2. Extract `Uaflix.nro` to the SD card at `sdmc:/switch/Uaflix.nro`.
3. Start Homebrew Menu in **full application mode** (hold R while opening a game), select Uaflix.

TMDB works out of the box using the public client key from the [official Lampa source](https://github.com/yumata/lampa-source/blob/main/src/core/tmdb/tmdb.js). To override this key put `tmdb.key` at `sdmc:/switch/lampa-nx/tmdb.key`. If the emulator lacks network access, a small built-in metadata snapshot keeps the main categories browseable, but it is not updated live.

## Current native features

- Movies, TV, top-rated and genre categories (action, comedy, drama, horror, sci-fi, animation, romance, documentary).
- Ukrainian TMDB titles, metadata descriptions, search via Switch software keyboard, live TMDB posters with in-session cache.
- Local-only **favorites**, **watch history**, and **resume position** stored at `sdmc:/switch/uaflix/library.tsv`.
- UAFresh searching from its current UAFlix and UASerials providers through the worker HTTP proxy; embedded video resolution including Zetvideo HLS; libmpv playback.
- Native **season/episode browser** for UAFresh sources exposing episode-page links (tested on UAFlix); play selected episode when its stream can be resolved.
- Player pause, ±10 second seek, **separate audio-track and subtitle-track lists** built from libmpv `track-list` with language/title and selected-track state.
- Rounded native interface and a transparent video control bar with Back, timeline scrubbing, pause/play, ±10-second seek, settings, audio and subtitles. The bar auto-hides after **3 seconds** and reappears when any button is pressed or the video is tapped.
- Switch touch controls for movie cards, source/season/episode lists, video overlay buttons, and timeline. Touch works with mouse clicks in Eden as well.
- Playback settings overlay: speed, volume, brightness, contrast, subtitle text size, subtitle timing, and **HLS bitrate preset** (auto/highest, economy, ≤1/2/4 Mbps). Changing bitrate reloads the stream and tries to restore playback position.
- Adds an external WebVTT subtitle track from UAFresh embedded players when the provider supplies one (for example Zetvideo's Ukrainian subtitles).
- Settings/information screen.
- Optional **UAFlix account sign-in** under Settings (`Minus` → `A`). The native app uses the same UAFresh worker `/login` endpoint as the supplied Lampa plugin, then sends its cookie to `/proxy` on UAFlix requests. This is important for series: the plugin explicitly warns that UAFlix serves only trailers without logging in on browser-style clients. `Y` signs out. Passwords are not saved, but the session cookie is stored in plaintext on the SD card at `sdmc:/switch/uaflix/uaflix.cookie`; anyone with access to the SD card can copy it. Do not use a password you share with other important accounts.
- Custom cyan-on-midnight **Uaflix** icon based on the user's supplied transparent geometric mark; homebrew display name is **Uaflix**.

## Controls

| Screen | Buttons |
|---|---|
| Catalog | D-pad or either analog stick choose, L/R change section, A details, X search, Y refresh, Minus settings, Plus exit |
| Details | A find UAFresh streams, X toggle favorite, Y mark watched, B back |
| UAFresh sources | Up/down select, A choose source (for TV with structured episodes: seasons), B back |
| Seasons | Up/down choose season, A episodes, B sources |
| Episodes | Up/down select episode, A resolve/play, B seasons |
| Video | A pause, D-pad/stick left/right seek, X subtitle list, Y audio list, Minus video settings, B stop/save position |
| Track selection | Up/down choose audio or subtitles, A apply, B return to video |
| Video settings | Up/down choose property, left/right adjust, B return |
| Settings | B return |

## UAFresh adapter

This is **not** the original UAFresh JavaScript plugin running inside Lampa. That plugin relies on the browser and the global `Lampa.*` runtime. The native adapter implements selected functionality directly in C. The older `uakino.best` source returned HTTP 403 and is deliberately excluded in the current plugin; UAFlix/UASerials work through the plugin's worker.

On 2026-11-09 a Linux test searched *Avatar*, received 14 results, and successfully resolved a direct HLS stream from UAFlix's embedded Zetvideo player. A separate test found 15 episodes across two seasons for UAFlix's *Avatar: The Last Airbender*. You can repeat these host-side tests with:

```sh
cc -O2 -Isource source/catalog.c tests/uafresh_smoke.c \
  $(pkg-config --cflags --libs libcurl) -o /tmp/uafresh-smoke
/tmp/uafresh-smoke
cc -O2 -Isource source/catalog.c tests/episodes_smoke.c \
  $(pkg-config --cflags --libs libcurl) -o /tmp/uaflix-episodes-smoke
/tmp/uaflix-episodes-smoke
```

## Important limitations

Nintendo-labelled ABXY now match the physical Switch buttons (the SDL controller events are remapped from their Xbox-style positions). Both sticks have deadzone and held-direction repeat. Playback initially requests an approximately 1 Mbit/s HLS variant to reduce software-decoder load on Switch; the video-settings menu can raise it. An mpv error or a 30-second startup timeout returns to the source list with a diagnostic instead of a permanently black screen. Some hosts can still reject streams despite a valid playlist.

This remains an **experimental native app**, not an exact feature-for-feature reimplementation of Lampa. Episode discovery depends on UAFresh providers exposing identifiable episode links; not all video hosts resolve to direct streams, and some pages are trailers only or geo/auth restricted. For example, the provider's **Avatar: The Last Airbender (2024)** season-1 episode-1 page contains only a YouTube trailer when fetched without an account; the supplied UAFresh plugin explicitly recommends signing in for UAFlix series. An account cannot guarantee availability if a video has been removed or is region-locked. The player offers audio/subtitle selection only for tracks actually present in the stream. HLS bitrate presets depend on what variants are offered by the source and mpv's demuxer; they may have no effect on a fixed-bitrate playlist. Native decoding and settings must be tested on a real Switch and Eden; successful CI compilation is not a runtime guarantee.

Metadata is provided by TMDB: **This product uses the TMDB API but is not endorsed or certified by TMDB.** No copyrighted video is bundled; only links returned by third-party services are resolved. Use content you have the right to access.

Font: DejaVu Sans (Bitstream Vera-derived license), included to render Ukrainian text. Refer to [DejaVu Fonts](https://dejavu-fonts.github.io/).
