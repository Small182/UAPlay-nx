#ifndef UAPLAY_WALLPAPERS_H
#define UAPLAY_WALLPAPERS_H

#include <GLES2/gl2.h>

#define WALLPAPERS_MAX 512
typedef struct {
    char name[128];
    char url[384];
    char thumbnail[384];
    char id[12];
    char tags[160];
    int width, height;
    unsigned bytes;
} WallpaperItem;

/* Start after SDL and OpenGL initialization; stop before the GL context goes away. */
void wallpapers_init(void);
void wallpapers_shutdown(void);
/* Network and disk work runs on an SDL worker; texture uploads run on the UI thread. */
void wallpapers_poll(void);
void wallpapers_open(void);
/* Browse SFW Wallhaven search results without an API key. */
int wallpapers_browse(int filter,const char *query,int page);
int wallpapers_current_page(void);
int wallpapers_last_page(void);
int wallpapers_revision(void);
int wallpapers_count(void);
const WallpaperItem *wallpapers_item(int index);
int wallpapers_busy(void);
int wallpapers_apply(int index);
void wallpapers_reset(void);
GLuint wallpapers_texture(void);
const char *wallpapers_selected(void);
const char *wallpapers_message(void);

#endif
