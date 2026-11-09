# UAPlay for Nintendo Switch — native homebrew

Native movies and serial UAFlix player for Switch using TMDB movie/series metadata and a native HTTP adapter for [UAFresh](https://uakino.ofreshofresh.workers.dev/uakino.js). 
## Install

1. Open Releases and download the latest UAPlay
2. Extract `UAPlay.nro` to the SD card at `sdmc:/switch/UAPlay.nro`.
3. Start Homebrew Menu in **full application mode** (hold R while opening a game), select UAPlay.

TMDB works out of the box using the public client key from the [official Lampa source](https://github.com/yumata/lampa-source/blob/main/src/core/tmdb/tmdb.js). To override this key put `tmdb.key` at `sdmc:/switch/lampa-nx/tmdb.key`. If the emulator lacks network access, a small built-in metadata snapshot keeps the main categories browseable, but it is not updated live.

## Current native features

- Movies, TV, top-rated and genre categories (action, comedy, drama, horror, sci-fi, animation, romance, documentary).
- Ukrainian TMDB titles, metadata descriptions, search via Switch software keyboard, live TMDB posters with in-session cache.
- Local-only **favorites**, **watch history**, and **resume position** stored at `sdmc:/switch/uaflix/library.tsv`.
- Source search via **UAFlix and UASerials**. Experimental UAKino.best integration has been disabled at the user's request. The native app requests UAFlix **catalogue and episode pages** directly from the Switch first, falling back to the existing UAFresh Worker when direct HTTP fails. For **embedded video players** such as Zetvideo, the app tries the Worker first because a direct request can return a deceptive HTTP 200 containing an nginx 404 page; if the Worker is unavailable it tries direct access. UAFlix results are parsed from actual search-result cards (not its site navigation categories), filtered by film/series type and labeled using their real titles. Embedded video resolution includes Zetvideo HLS and bounded nested iframe players.
- Native **season/episode browser** for UAFresh sources exposing episode-page links. Actual video playback depends on the episode's stream being discoverable; UAFlix web playback does not require a login.
- Player pause, ±10 second seek, **separate audio-track and subtitle-track lists** built from libmpv `track-list` with language/title and selected-track state.
- Netflix-TV-inspired navigation concept (native UAPlay branding): top-level category tabs, selected-title hero with synopsis/actions, matching poster shelf, full collection browser, full-page settings and title details. No Netflix assets or proprietary UI code are included.
- A consistent, controller-first **1280 × 720 design system**: 32 px safe-area gutters, an 8 px rhythm, a reserved 52 px footer, reusable 50 px list rows at 58 px intervals, and typography at 31/22/19/15 px. Posters preserve an approximately 2:3 aspect ratio; selection outlines no longer change image dimensions. Long titles wrap at Unicode-safe word boundaries at a fixed font size. All touch hitboxes are aligned to their drawn controls. The footer uses a white-on-charcoal Switch controller legend with round ABXY icons and squared shoulder-button icons.
- The 8-row list viewport supports page-by-page navigation via D-pad and analog stick for long source, season, episode and track lists. Navigation tabs, content actions, error notifications and player controls use common color, radius and alignment rules. Footer button hints are contextual and tied to actions implemented on the current screen.
- Dark full-width video overlay with Back, volume adjustment, timeline scrubbing, pause/play, ±10-second seek, settings, audio and subtitles. The overlay auto-hides after **3 seconds** and reappears when any button is pressed or the video is tapped.
- Switch touch controls for movie cards, source/season/episode lists, video overlay buttons, and timeline. Touch works with mouse clicks in Eden as well.
- Playback settings overlay: speed, volume, brightness, contrast, subtitle text size, subtitle timing, and **HLS quality targets** Auto/360p/480p/720p/1080p. mpv selects a stream variant by bitrate ceiling (≈0.8/1.5/3/6 Mbps); the actual video height depends on variants supplied by the provider. Changing the preset reloads the stream and tries to restore playback position.
- Adds an external WebVTT subtitle track from UAFresh embedded players when the provider supplies one (for example Zetvideo's Ukrainian subtitles).
- Settings/information screen.
- **No UAFlix sign-in screen**. Public series episodes are attempted without requiring an account; the earlier custom login keyboard and diagnostic "safe mode" message have been removed from UI.
- Custom cyan-on-midnight **UAPlay** icon featuring the user's supplied diagonal filmstrip, sprocket holes, play symbol, and stars; homebrew display name is **UAPlay**.
- Seven Noto Sans fallback fonts (Tamil, Telugu, Devanagari, Malayalam, Bengali, Arabic, Thai) are bundled for non-Latin titles; they are distributed under the SIL Open Font License 1.1 (Google Noto Fonts).

## Controls

| Screen | Buttons |
|---|---|
| Catalog | D-pad or either analog stick choose, L/R change section, A details, X search, Y refresh, Minus settings, Plus exit |
| Details | A find UAFresh streams, X toggle favorite, Y mark watched, B back |
| UAFresh sources | Up/down select, A choose source (for TV with structured episodes: seasons), B back |
| Seasons | Up/down choose season, A episodes, B sources |
| Episodes | Up/down select episode, A resolve/play, B seasons |
| Video | A pause, D-pad/stick left/right seek ±10 seconds, **hold R with left/right to seek ±60 seconds**, X subtitle list, Y audio list, Minus video settings, B stop/save position |
| Track selection | Up/down choose audio or subtitles, A apply, B return to video |
| Video settings | Up/down choose property, left/right adjust, B return |
| Settings | Up/down choose Interface/Player/Sources, A open/change, B return |

## UAFresh adapter

This is **not** the original UAFresh JavaScript plugin running inside Lampa. That plugin relies on the browser and the global `Lampa.*` runtime. The native adapter implements selected functionality directly in C. **UAKino.best is disabled.** When TMDB is temporarily inaccessible, the app can display a small offline home catalogue. If the user searches while offline, results are filtered by the requested Ukrainian/English title from that limited snapshot rather than incorrectly showing unrelated movies. Offline mode is identified by a small label instead of a toast covering the poster shelf.

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
