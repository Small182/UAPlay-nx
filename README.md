# Lampa-nx (native Switch prototype)

An early **native** Nintendo Switch player prototype using SDL2 and libmpv (FFmpeg). No Chromium, WebKit, or Linux guest.

## First test

1. Download `Lampa-nx.nro` from the latest successful GitHub Actions artifact.
2. Copy to `sdmc:/switch/lampa-nx/Lampa-nx.nro`.
3. Launch via Atmosphère Homebrew Menu in **full application mode** (hold R when opening a game). The memory-limited Album applet is not suitable for video.
4. A public demo video (Big Buck Bunny) is played by default.

To test a video URL you are authorized to access, create the text file `sdmc:/switch/lampa-nx/stream.url` with a single direct `https://` or `http://` MP4/HLS URL on the first line. Restart the app.

**Controls:** A = pause/resume; left/right = seek 10 seconds; Y = show playback time; B = stop; Plus = exit.

## State / scope

- **Implemented:** SDL video surface, real libmpv playback API, Joy-Con controls, external URL configuration.
- **Not yet implemented:** Lampa catalog UI, UAKino source integration, bookmarks, stream quality selection.
- This is an integration milestone, not yet a replacement for the Chromium-based Lampa build.

We will add a native catalog and an adapter for the single requested UAKino extension. The extension is not directly executable as a Lampa plugin in this native build; its functionality must be adapted.

Playback relies on the installed libmpv codec support and the external stream's accessibility. Hardware decoding is requested automatically where supported; it must be verified on a real Switch.

## References

- [mpv official SDL/libmpv example](https://github.com/mpv-player/mpv-examples/blob/master/libmpv/sdl/main.c)
- [Switch SDL2 sample Makefile](https://github.com/switchbrew/switch-examples/blob/master/graphics/sdl2/sdl2-simple/Makefile)
- [SwitchWave](https://github.com/averne/SwitchWave)
