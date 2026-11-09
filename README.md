# Lampa-nx — experimental native Nintendo Switch catalog + player

Native Switch prototype with **TMDB-based Lampa catalog**, Ukrainian titles,
search, movie/series details, experimental **UAKino / UAFresh** source adapter,
and `libmpv` playback. Uses libnx + SDL2 + SDL2_ttf + libcurl + libmpv; **no Chromium**.

## Installation

1. Open the latest successful [GitHub Actions build](https://github.com/Small182/Lampa-nx/actions).
2. Download the `Lampa-nx-catalog-uakino` artifact and extract `Lampa-nx.nro`.
3. Copy it to `sdmc:/switch/lampa-nx/Lampa-nx.nro`.
4. Start Homebrew Menu in **full application mode** (hold R when opening a game), then launch Lampa-nx.

The catalog uses the **public client API key embedded in the open-source
[Lampa TMDB implementation](https://github.com/yumata/lampa-source/blob/main/src/core/tmdb/tmdb.js)**.
You do not need to supply your own key. To override it, create the text file
`sdmc:/switch/lampa-nx/tmdb.key` containing your TMDB v3 API key.

Network sockets are initialized using libnx. If a Switch emulator cannot access
the internet, the application falls back to a bundled snapshot of the three
catalog categories, so the catalog UI can still be tested. Search and UAKino
still require a working network connection; an offline snapshot is not live data.

## Controls

| Screen | Buttons |
|---|---|
| Catalog | D-pad navigation, **L/R** categories, **A** details, **X** text search, **Y** refresh |
| Details | **A** query UAKino/UAFresh, **B** back |
| Sources | D-pad select, **A** resolve stream, **B** back |
| Playback | **A** pause/resume, left/right seek 10 seconds, **B** back |
| Catalog/non-player | **+** exit |

## UAKino integration status — important

The URL `https://uakino.ofreshofresh.workers.dev/uakino.js` is a **Lampa JavaScript plugin**, not a standalone native API. We cannot execute it as-is without Lampa's JavaScript runtime and its `Lampa.*` interfaces.

This native prototype therefore implements **only an experimental HTTP adapter**:
it requests UAKino search pages through the plugin's public UAFresh worker proxy,
enumerates candidate pages, and looks for direct MP4/HLS/DASH stream URLs to pass
to `libmpv`. It does **not** yet reproduce the full plugin's providers, obfuscated
players, episode selection, authentication, or runtime behavior.

At the time of development, the UAKino proxy test received a Cloudflare browser
challenge. **That means catalog browsing should work, but actual UAKino video
playback is not yet verified and may be blocked.** No claim of full UAKino
compatibility is made. Never ship passwords/cookies in source control.

## Notes

- TMDB provides **metadata**, not movies or video streams.
- Images/posters, bookmarks, subtitle/track selection and a full plugin bridge
  remain to be implemented.
- Hardware decoding and performance must be verified on an actual Switch.
- Includes a DejaVu Sans font for Unicode/Ukrainian rendering, derived from the
  DejaVu Fonts project (Bitstream Vera derivative; see its distribution license).

## Related source

- [Lampa source (TMDB metadata)](https://github.com/yumata/lampa-source)
- [UAFresh plugin](https://uakino.ofreshofresh.workers.dev/uakino.js)
- [libmpv examples](https://github.com/mpv-player/mpv-examples)
