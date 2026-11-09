#ifndef UAFLIX_PLAYBACK_H
#define UAFLIX_PLAYBACK_H

#include <mpv/client.h>

#define TRACKS_MAX 48
typedef struct {
    int id;
    int selected;
    char label[192];
} MediaTrack;

int playback_tracks(mpv_handle *player, int subtitles, MediaTrack *out, int maximum);
void playback_select_track(mpv_handle *player, int subtitles, int id);
void playback_set_value(mpv_handle *player, const char *property, double value);

#endif
