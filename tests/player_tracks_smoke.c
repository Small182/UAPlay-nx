#include <stdio.h>
#include <string.h>

/* Host test for PlayerJS source and subtitle extraction, without network. */
#include "../source/catalog.c"

static int fail(const char *what) {
    fprintf(stderr,"Player track parsing failed: %s\n",what);
    return 1;
}
int main(void) {
    char stream[2048]={0};
    const char *film=
        "<script>new Playerjs({"
        "file:'{Українська}https://video.example/ua.m3u8;"
              "{English}https://video.example/en.m3u8',"
        "subtitle:'[Українські]https://subs.example/ua.vtt,"
                  "[English]https://subs.example/en.srt'"
        "});</script>";
    resolved_audio_count=resolved_subtitle_count=0;
    if(!extract_media(film,stream,sizeof(stream)))return fail("video not found");
    if(strcmp(stream,"https://video.example/ua.m3u8") ||
       uakino_resolved_audio_count()!=2 ||
       strcmp(uakino_resolved_audio_label(1),"English") ||
       strcmp(uakino_resolved_audio_url(1),"https://video.example/en.m3u8"))
        return fail("separate dub URLs");
    if(uakino_resolved_subtitle_count()!=2 ||
       strcmp(uakino_resolved_subtitle_label(0),"Українські") ||
       strcmp(uakino_resolved_subtitle_url(1),"https://subs.example/en.srt"))
        return fail("multiple named subtitle URLs");

    const char *quality=
        "<script>new Playerjs({file:\"[480p]{UA}https:\\/\\/cdn.example/ua480.mp4;"
        "{EN}https:\\/\\/cdn.example/en480.mp4,"
        "[720p]{UA}https://cdn.example/ua720.mp4;"
        "{EN}https://cdn.example/en720.mp4\"});</script>";
    resolved_audio_count=resolved_subtitle_count=0;
    if(!extract_media(quality,stream,sizeof(stream)) ||
       uakino_resolved_audio_count()!=2 ||
       strcmp(uakino_resolved_audio_url(0),"https://cdn.example/ua480.mp4") ||
       strcmp(uakino_resolved_audio_url(1),"https://cdn.example/en480.mp4"))
        return fail("quality groups and JS escaped URLs");

    const char *inline_subs=
        "<script>new Playerjs({file:'https://cdn.example/movie.m3u8"
        "(subtitle:[Uk]https://subs.example/uk.vtt"
        "[En]https://subs.example/en.ass)'});</script>";
    resolved_audio_count=resolved_subtitle_count=0;
    if(!extract_media(inline_subs,stream,sizeof(stream)) ||
       strcmp(stream,"https://cdn.example/movie.m3u8") ||
       uakino_resolved_subtitle_count()!=2 ||
       strcmp(uakino_resolved_subtitle_url(1),"https://subs.example/en.ass"))
        return fail("subtitle syntax inside media URL");

    const char *missing="<script>var subtitleSize=32;"
                        "var url='https://cdn.example/movie.m3u8';</script>";
    resolved_audio_count=resolved_subtitle_count=0;
    if(!extract_media(missing,stream,sizeof(stream)) ||
       uakino_resolved_subtitle_count()!=0)
        return fail("false subtitle property or fallback direct URL");

    puts("PlayerJS dubbed streams and subtitles: OK");
    return 0;
}
