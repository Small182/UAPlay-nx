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

Poster thumbnails are loaded directly from TMDB's image CDN at runtime and cached
in RAM during the session. They need network access; the offline metadata snapshot
cannot contain posters. A small placeholder is shown when an image is unavailable.

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

This native prototype implements **a tested HTTP adapter for the current UAFresh
sources UAFlix and UASerials**. The plugin itself deliberately excludes the old
`uakino.best` provider, which gave us an HTTP 403. We now search providers through
UAFresh's public proxy and follow known player embeds (for example Zetvideo) to
resolve direct HLS streams for `libmpv`.

An end-to-end host-side smoke test on 2026-11-09 found 14 pages for *Avatar*
and successfully resolved the UAFlix *Avatar (2009)* player to an `.m3u8` URL.
Run it on a Linux PC using:

```sh
cc -O2 -Isource source/catalog.c tests/uafresh_smoke.c \
    $(pkg-config --cflags --libs libcurl) -o /tmp/lampa-uafresh-smoke
/tmp/lampa-uafresh-smoke
```

**Limitations:** this does not execute the complete JavaScript plugin; it does
not yet implement all UAFresh providers, obfuscated player links, episode
selection, authorization, or geo-restricted streams. Successful resolution on a
PC does not prove actual decoding on Switch/Eden. Never ship passwords/cookies
in source control.

## Notes

- TMDB provides **metadata**, not movies or video streams.
- Bookmarks, subtitle/track selection and a full plugin bridge remain to be implemented.
- Uses the TMDB API and image CDN; **This product uses the TMDB API but is not
  endorsed or certified by TMDB.**
- Hardware decoding and performance must be verified on an actual Switch.
- Includes a DejaVu Sans font for Unicode/Ukrainian rendering, derived from the
  DejaVu Fonts project (Bitstream Vera derivative; see its distribution license).

## Related source

- [Lampa source (TMDB metadata)](https://github.com/yumata/lampa-source)
- [UAFresh plugin](https://uakino.ofreshofresh.workers.dev/uakino.js)
- [libmpv examples](https://github.com/mpv-player/mpv-examples)
