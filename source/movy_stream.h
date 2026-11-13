#ifndef UAPLAY_MOVY_STREAM_H
#define UAPLAY_MOVY_STREAM_H

#include <stddef.h>

/* Serve one Movy session over loopback, using libcurl for upstream HLS. */
int movy_stream_open(const char *upstream,long bandwidth,char *url,size_t size);
void movy_stream_close(void);

#endif
