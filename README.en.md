# UAPlay — Video Catalog for Nintendo Switch

**UAPlay** is a native Nintendo Switch homebrew application featuring a Ukrainian-language catalog of movies, TV series, cartoons, animated series, and anime. It uses metadata from [TMDB](https://www.themoviedb.org/) and searches for available video streams from independent third-party sources.

The application is written in C using **libnx, SDL2, and libmpv**. It is not a website running in a browser: its interface and playback do not require Chromium, WebKit, or TorrServer.

[Українська](README.md) | **English**

> **Current release: [UAPlay 0.5.0.5 beta](https://github.com/Small182/UAPlay-nx/releases/tag/UAPlay_0.5.0.5-beta).** This project is under active development, and video availability depends on third-party services. A successful build does not guarantee that every video will play on a Switch.

## Download and installation

Download both files from the [0.5.0.5 beta release](https://github.com/Small182/UAPlay-nx/releases/tag/UAPlay_0.5.0.5-beta). Older versions are available on the [Releases page](https://github.com/Small182/UAPlay-nx/releases).

| File | Purpose |
|---|---|
| `UAPlay.nro` | The main application. **Required** to run UAPlay. |
| `UAPlay.nsp` | An optional shortcut for launching UAPlay from the Switch HOME Menu. |

1. Create the `switch` folder on your SD card if it does not already exist.
2. Copy **`UAPlay.nro`** to the exact path **`sdmc:/switch/UAPlay.nro`**.
3. Open the Homebrew Menu in full application mode (for example, by holding **R** while launching a game), then select UAPlay.
4. **Optional:** Install `UAPlay.nsp` using a compatible homebrew installer to launch UAPlay directly from the HOME Menu.

**Important:** `UAPlay.nsp` is a *forwarder*, not a standalone copy of the application. If `UAPlay.nro` is removed or moved, the shortcut will stop working. Installing unofficial NSP files requires an appropriately configured CFW environment and involves risks to your console and access to Nintendo services.

When updating, **replace only `UAPlay.nro`**. Do not delete user-data folders. The NSP usually does not need to be reinstalled as long as its target path and shortcut remain unchanged.

## Features

- **Ukrainian-language catalog:** Movies, TV series, cartoons, animated series, anime, and genre collections.
- **Home screen:** Horizontal poster collections with ratings and a “Усі” (*All*) button that opens the full list.
- **Search:** TMDB titles, the Switch on-screen keyboard, and filtering of irrelevant results.
- **Title details:** Synopsis, year, genres, and available metadata; choose a video provider before searching for streams.
- **Favorites and watch history:** Separate “На майбутнє” (*Watch Later*) and “Історія” (*History*) lists, playback-position saving, and the ability to clear watch history without deleting favorites.
- **TV series and anime:** Season and episode selection when the provider supplies structured information, with support for advancing to the next episode.
- **libmpv player:** Pause, seeking, volume control, and selection of available audio tracks, dubbed versions, and subtitles.
- **Video settings:** Playback speed, volume, subtitle size, and HLS quality presets (Auto / 360p / 480p / 720p / 1080p).
- **Controls:** Joy-Con / Pro Controller and touchscreen.
- **Appearance:** Light and dark themes with selectable accent colors.
- **Japanese font:** Japanese characters in anime titles are supported.

The catalog filters productions by TMDB country-of-origin metadata, including excluding entries associated with `RU`. Some results may be hidden when their country metadata cannot be verified.

## Video providers

You select a provider **before** searching for a playable stream. The current beta includes:

| Provider | Notes |
|---|---|
| UAFlix | Movies and TV series; some episode pages are supported. |
| UASerials | Search for Ukrainian-language content. |
| Eneyida | Experimental integration. |
| UAFilm | Experimental integration. |
| UAKino | Experimental search; some requests use an alternative mirror. |
| Mikai | Anime; playback availability depends on the video host. |
| AnimeON | Anime; some embedded players may respond with **HTTP 403**. |

**Not every search result can be played.** Third-party sites may change their HTML structures, require authentication, restrict access by region, or block direct requests. UAPlay does not bypass those restrictions.

## Controls

Button labels correspond to the **Nintendo Switch** layout, not the Xbox controller layout.

| Screen | Controls |
|---|---|
| Catalog | **D-pad / stick** — navigate; **A** — open; **B** — back; **L/R** — switch main sections. |
| Search and menus | **X** — search; **Y** — refresh catalog; **− (Minus)** — settings; **+ (Plus)** — exit. |
| Poster collection | Press **↑** on the rightmost poster to focus the **“Усі” (All)** button. |
| Categories | Press **→** on the last tab (“Документальне” / *Documentaries*, or “Аніме” / *Anime*) to focus **All**; press **←** on **All** to return to the tab on its left. |
| Title details | **A** — choose a provider / watch; **X** — add to favorites; **Y** — mark as watched. |
| Providers, seasons, episodes | **↑/↓** — select; **A** — confirm; **B** — back. Touch input is also supported. |
| Player | **A** — pause; **←/→** — seek by 10 seconds; **R + ←/→** — seek by 60 seconds; **↑/↓** — volume; **B** — close player. |
| Tracks and video | **X** — subtitles; **Y** — audio / dubbing; **− (Minus)** — video settings. |

The touch-enabled player provides playback controls, a seek bar, and a volume slider. The overlay hides automatically after a few seconds of inactivity.

## User data and updates

Favorites, watch history, and playback positions are stored **locally on the SD card**, not in the NRO. The main library file is `sdmc:/switch/uaflix/library.tsv` (a legacy path preserved for compatibility with earlier versions).

Replacing `UAPlay.nro` during an update **should not reset favorites**. Back up `sdmc:/switch/uaflix/` before formatting the SD card or reinstalling your system.

## Building from source

The project targets Nintendo Switch using devkitPro/devkitA64, with `libnx`, `SDL2`, `SDL2_ttf`, `SDL2_image`, `libmpv`, `libcurl`, and a network-enabled FFmpeg build for Switch.

The easiest way to obtain a compiled NRO is via [GitHub Actions](https://github.com/Small182/UAPlay-nx/actions/workflows/build.yml). The workflow builds `UAPlay.nro` in a devkitPro container and uploads it as a build artifact.

In a properly configured local devkitPro environment:

```sh
make -j2
```

Official release builds receive the TMDB key from the `TMDB_API_KEY` GitHub Actions secret during compilation. **The key is not stored in the public repository.** A local build without this setup may require a key at `sdmc:/switch/uaplay/tmdb.key`. Do not commit your API keys or include them in logs.

Some catalog and parser tests are located in [`tests/`](tests/). They do not replace testing on a real console.

## Limitations and legal notice

- UAPlay is an **experimental homebrew project** and is not an official Nintendo, TMDB, or third-party video-service application.
- Playback of a particular video, episode, dubbed version, or subtitle track depends on what the provider offers and what libmpv can read.
- **No video content is bundled with the application.** Only access content you have the right to use.

Metadata: *This product uses the TMDB API but is not endorsed or certified by TMDB.*

Fonts: DejaVu Sans (Bitstream Vera-derived license) and Google Noto family fonts (SIL Open Font License 1.1).
