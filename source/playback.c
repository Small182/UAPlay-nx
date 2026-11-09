#include "playback.h"
#include <stdio.h>
#include <string.h>

static const mpv_node *map_field(const mpv_node *item, const char *key) {
    if (!item || item->format != MPV_FORMAT_NODE_MAP) return NULL;
    mpv_node_list *map=item->u.list;
    for (int i=0; i<map->num; i++)
        if (!strcmp(map->keys[i],key)) return &map->values[i];
    return NULL;
}
static const char *string_field(const mpv_node *map, const char *key) {
    const mpv_node *field=map_field(map,key);
    return field && field->format==MPV_FORMAT_STRING ? field->u.string : "";
}
static int int_field(const mpv_node *map, const char *key) {
    const mpv_node *field=map_field(map,key);
    if (!field) return 0;
    if (field->format==MPV_FORMAT_INT64) return (int)field->u.int64;
    if (field->format==MPV_FORMAT_FLAG) return field->u.flag;
    return 0;
}
int playback_tracks(mpv_handle *player, int subtitles, MediaTrack *out, int maximum) {
    int count=0;
    if (subtitles && maximum>0) {
        out[count].id=0;
        out[count].selected=0;
        snprintf(out[count].label,sizeof(out[count].label),"Вимкнути субтитри");
        count++;
    }
    if (!player || maximum<1) return count;
    mpv_node node={0};
    if (mpv_get_property(player,"track-list",MPV_FORMAT_NODE,&node)<0) return count;
    if (node.format==MPV_FORMAT_NODE_ARRAY) {
        mpv_node_list *list=node.u.list;
        for (int i=0; i<list->num && count<maximum; i++) {
            const mpv_node *track=&list->values[i];
            if (strcmp(string_field(track,"type"),subtitles?"sub":"audio")) continue;
            MediaTrack *dst=&out[count++];
            dst->id=int_field(track,"id");
            dst->selected=int_field(track,"selected");
            const char *lang=string_field(track,"lang");
            const char *title=string_field(track,"title");
            snprintf(dst->label,sizeof(dst->label),"%s #%d  %s  %s%s",
                     subtitles?"Субтитри":"Аудіо",dst->id,lang,title,
                     dst->selected?" [активна]":"");
        }
    }
    mpv_free_node_contents(&node);
    return count;
}
int playback_select_track(mpv_handle *player, int subtitles, int id) {
    if (!player) return 0;
    char number[20];
    snprintf(number,sizeof(number),"%d",id);
    const char *property=subtitles?"sid":"aid";
    int result=mpv_set_property_string(player,property,id==0?"no":number);
    if(result<0)return 0;
    if(subtitles && id>0)
        mpv_set_property_string(player,"sub-visibility","yes");
    char *active=mpv_get_property_string(player,property);
    int ok=active && ((!strcmp(active,number)) ||
                      (id==0 && !strcmp(active,"no")));
    if(active)mpv_free(active);
    return ok;
}
void playback_set_value(mpv_handle *player, const char *property, double value) {
    if (!player) return;
    char number[32];
    snprintf(number,sizeof(number),"%.2f",value);
    const char *args[]={"set",property,number,NULL};
    mpv_command_async(player,0,args);
}
