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
- Playback settings overlay: speed, volume, brightness, contrast, subtitle text size, subtitle timing, and **HLS quality targets** Auto/360p/480p/720p/1080p. mpv selects a stream variant by bitrate ceiling (≈0.8/1.5/3/6 Mbps); the actual video height depends on variants supplied by the provider. Changing the preset reloads the stream and tries to restore playback position.
- Adds an external WebVTT subtitle track from UAFresh embedded players when the provider supplies one (for example Zetvideo's Ukrainian subtitles).
- Settings/information screen.

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

This is **not** the original UAFresh JavaScript plugin running inside Lampa. 

Metadata is provided by TMDB: **This product uses the TMDB API but is not endorsed or certified by TMDB.** No copyrighted video is bundled; only links returned by third-party services are resolved. Use content you have the right to access.

Font: DejaVu Sans (Bitstream Vera-derived license), included to render Ukrainian text. Refer to [DejaVu Fonts](https://dejavu-fonts.github.io/).
