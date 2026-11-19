#include "i18n.h"
#include "downloads.h"
#include "platform.h"

#include <SDL.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef __vita__
#include <openssl/evp.h>
#endif

#define DOWNLOADS_DIR UAPLAY_DATA_DIR "/downloads"
#define DOWNLOADS_INDEX DOWNLOADS_DIR "/index.dat"
#define DOWNLOADS_INDEX_TMP DOWNLOADS_DIR "/index.tmp"
#define DOWNLOADS_MAGIC 0x55414431u            /* "UAD1" */
#define PLAYLIST_LIMIT (4u * 1024u * 1024u)
#define SEGMENT_LIMIT (64u * 1024u * 1024u)
/* Direct files come in Range pieces, each a short turn at the network
   gate, so posters and source searches are never held up for long. */
#define DIRECT_CHUNK (2ull * 1024ull * 1024ull)
/* Weak Wi-Fi drops out for a moment: retry a segment with growing pauses. */
#define SEGMENT_ATTEMPTS 6
#define PARALLEL_REQUESTS 4
#define PARALLEL_SEGMENT_LIMIT (8u * 1024u * 1024u)
#define HLS_KEYS_MAX 16
/* The player's HLS choice: a variant no taller than the screen. */
#ifdef __vita__
#define VARIANT_MAX_HEIGHT 576
#define VARIANT_FALLBACK_BANDWIDTH 2500000L
#else
#define VARIANT_MAX_HEIGHT 720
#define VARIANT_FALLBACK_BANDWIDTH 5000000L
#endif
#define STREAM_USER_AGENT "Mozilla/5.0 (Nintendo Switch; UAPlay/1.0)"

static Download items[DOWNLOADS_MAX];
static int item_count;
static unsigned next_id = 1;
static SDL_mutex *lock;
static SDL_cond *wake;
static SDL_Thread *worker;
static int quitting;
static unsigned active_id;
/* Protected by the download lock and reset for each new job. */
static Uint32 speed_sample_at, speed_updated_at;
static unsigned long long speed_sample_bytes;
static double download_rate;
/* Set to stop the running transfer: the item was removed, or the app quits. */
static SDL_atomic_t abort_active;
static SDL_atomic_t user_paused;
#ifndef __vita__
/* The Vita network gate / profiler are console-specific. On Switch the
   download worker has its own gate. This protects video playback without
   serializing the catalog and poster requests on faster Switch networks. */
static SDL_atomic_t downloads_paused_flag;
static void net_background_acquire(int priority) {
    (void)priority;
    while (SDL_AtomicGet(&downloads_paused_flag) &&
           !SDL_AtomicGet(&abort_active)) SDL_Delay(100);
}
static void net_background_release(int priority) { (void)priority; }
static void net_profile_record(char kind, void *curl, const char *url,
                               unsigned waited, int result, unsigned bytes,
                               const char *error) {
    (void)kind; (void)curl; (void)url; (void)waited;
    (void)result; (void)bytes; (void)error;
}
static void net_tls_options(void *curl) { (void)curl; }
void downloads_pause(int paused) { SDL_AtomicSet(&downloads_paused_flag, paused != 0); }
#else
void downloads_pause(int paused) { net_background_pause(paused); }
#endif


const char *stream_referer(const char *url) {
    if (strstr(url, "zetvideo.net")) return "https://zetvideo.net/";
    if (strstr(url, "ashdi.vip")) return "https://ashdi.vip/";
    if (strstr(url, "hdvbua.pro")) return "https://hdvbua.pro/";
    if (movy_stream_url(url)) return MOVY_ORIGIN "/";
    return "";
}

static void legacy_part_path(unsigned id, char *out, size_t size) {
    snprintf(out, size, DOWNLOADS_DIR "/%u.part", id);
}

static int finished_path(const Download *job, const char *ext, char *out, size_t size) {
    char title[sizeof(job->movie.title) + 2];
    const char *source = job->movie.original[0] ? job->movie.original : job->movie.title;
    size_t length = 0;
    for (; length < sizeof(job->movie.title) && source[length]; length++) {
        unsigned char c = (unsigned char)source[length];
        title[length] = (c < 32 || c == 127 || strchr("<>:\"/\\|?*", c)) ? '_' : (char)c;
    }
    title[length] = '\0';

    /* DOS device names remain reserved even with a file extension. */
    size_t stem = strcspn(title, ".");
    while (stem && title[stem - 1] == ' ') stem--;
    if ((stem == 3 && (!strncasecmp(title, "CON", 3) || !strncasecmp(title, "PRN", 3) ||
                       !strncasecmp(title, "AUX", 3) || !strncasecmp(title, "NUL", 3))) ||
        (stem == 4 && (!strncasecmp(title, "COM", 3) || !strncasecmp(title, "LPT", 3)) &&
         title[3] >= '0' && title[3] <= '9')) {
        memmove(title + 1, title, length + 1);
        title[0] = '_';
        length++;
    }

    char episode[32] = "";
    if (job->movie.series && job->episode > 0)
        snprintf(episode, sizeof(episode), " - S%02dE%02d", job->season, job->episode);

    for (unsigned copy = 1; copy != 0; copy++) {
        char suffix[24] = "";
        if (copy > 1) snprintf(suffix, sizeof(suffix), " (%u)", copy);

        /* Leave space for the extension and keep the existing index layout. */
        size_t overhead = sizeof(DOWNLOADS_DIR "/") + strlen(episode) +
                          strlen(suffix) + 1 + strlen(ext);
        if (size <= overhead) return 0;
        size_t end = length < size - overhead ? length : size - overhead;
        while (end && ((unsigned char)title[end] & 0xc0) == 0x80) end--;
        while (end && (title[end - 1] == '.' || title[end - 1] == ' ')) end--;
        const char *name = title;
        if (!end) {
            name = "UAPlay";
            end = strlen(name);
            if (end > size - overhead) return 0;
        }

        snprintf(out, size, DOWNLOADS_DIR "/%.*s%s%s.%s",
                 (int)end, name, episode, suffix, ext);
        struct stat info;
        if (stat(out, &info) != 0) return errno == ENOENT;
    }
    return 0;
}

/* ---------- index ---------- */

/* Caller holds lock. */
static void save_index(void) {
    FILE *file = fopen(DOWNLOADS_INDEX_TMP, "wb");
    if (!file) return;
    unsigned header[3] = { DOWNLOADS_MAGIC, (unsigned)sizeof(Download), (unsigned)item_count };
    int ok = fwrite(header, sizeof(header), 1, file) == 1 &&
             (!item_count || fwrite(items, sizeof(Download), (size_t)item_count, file) ==
                             (size_t)item_count);
    if (fclose(file) == 0 && ok) {
        remove(DOWNLOADS_INDEX);
        rename(DOWNLOADS_INDEX_TMP, DOWNLOADS_INDEX);
    }
}

static void load_index(void) {
    FILE *file = fopen(DOWNLOADS_INDEX, "rb");
    if (!file) return;
    unsigned header[3];
    if (fread(header, sizeof(header), 1, file) == 1 && header[0] == DOWNLOADS_MAGIC &&
        header[1] == sizeof(Download) && header[2] <= DOWNLOADS_MAX)
        item_count = (int)fread(items, sizeof(Download), header[2], file);
    fclose(file);
    for (int i = 0; i < item_count; i++) {
        Download *d = &items[i];
        const size_t old_prefix = sizeof(UAPLAY_PREVIOUS_DATA_DIR "/downloads/") - 1;
        if (!strncmp(d->file, UAPLAY_PREVIOUS_DATA_DIR "/downloads/", old_prefix)) {
            /* A migrated index can contain absolute paths from the previous directory. */
            char path[sizeof(d->file)];
            int length = snprintf(path, sizeof(path), DOWNLOADS_DIR "/%s", d->file + old_prefix);
            if (length >= 0 && (size_t)length < sizeof(path))
                memcpy(d->file, path, (size_t)length + 1);
        }
        if (d->id >= next_id) next_id = d->id + 1;
        /* Interrupted downloads start again from the beginning. */
        if (d->state == DOWNLOAD_ACTIVE || (d->state == DOWNLOAD_PAUSED && d->file[0])) {
            char part[64];
            legacy_part_path(d->id, part, sizeof(part));
            remove(part);
            size_t length = strlen(d->file);
            if (length > 5 && !strcmp(d->file + length - 5, ".part"))
                remove(d->file);
            d->file[0] = 0;
            if (d->state == DOWNLOAD_ACTIVE) d->state = DOWNLOAD_QUEUED;
            d->permille = 0;
            d->bytes = 0;
        }
        struct stat info;
        if (d->state == DOWNLOAD_DONE && stat(d->file, &info) != 0) {
            d->state = DOWNLOAD_FAILED;
            snprintf(d->error, sizeof(d->error), TR("Файл не знайдено","File not found"));
        }
    }
}

static Download *find(unsigned id) {
    for (int i = 0; i < item_count; i++)
        if (items[i].id == id) return &items[i];
    return NULL;
}

/* Progress from the worker; returns 0 once the transfer should stop. */
static int report(unsigned id, int permille, unsigned long long bytes) {
    if (SDL_AtomicGet(&abort_active)) return 0;
    SDL_LockMutex(lock);
    Download *d = find(id);
    if (d) {
        Uint32 now = SDL_GetTicks();
        Uint32 elapsed = now - speed_sample_at;
        if (bytes < d->bytes) {
            speed_sample_at = now;
            speed_sample_bytes = bytes;
            download_rate = 0;
        } else if (bytes > speed_sample_bytes && elapsed >= 750) {
            double sample = (bytes - speed_sample_bytes) * 1000.0 / elapsed;
            download_rate = download_rate > 0 ? .65 * download_rate + .35 * sample : sample;
            speed_sample_at = now;
            speed_sample_bytes = bytes;
            speed_updated_at = now;
        }
        d->permille = permille < 0 ? -1 : permille > 1000 ? 1000 : permille;
        d->bytes = bytes;
    }
    SDL_UnlockMutex(lock);
    return 1;
}

/* ---------- HTTP ---------- */

typedef struct {
    char *data;
    size_t used, capacity, limit;
} Buffer;

typedef struct {
    FILE *file;
    unsigned long long written;
    int failed;
} FileSink;

typedef struct {
    unsigned id;
    int direct;               /* report byte progress of this transfer */
    unsigned long long base;  /* bytes written before this transfer */
    Uint32 reported_at;
    const char *range;        /* "first-last" byte range to ask for */
    int ungated;              /* skip the network gate (one long transfer) */
    long status;              /* HTTP status of the answer */
    unsigned long long range_total;  /* full size from Content-Range */
    unsigned long long range_first, range_last;
} Transfer;

static size_t to_buffer(char *data, size_t size, size_t count, void *opaque) {
    Buffer *b = opaque;
    size_t bytes = size * count;
    if (b->used + bytes + 1 > b->limit) return 0;
    if (b->used + bytes + 1 > b->capacity) {
        size_t next = (b->used + bytes + 1) * 2;
        if (next > b->limit) next = b->limit;
        char *grown = realloc(b->data, next);
        if (!grown) return 0;
        b->data = grown;
        b->capacity = next;
    }
    memcpy(b->data + b->used, data, bytes);
    b->used += bytes;
    b->data[b->used] = 0;
    return bytes;
}

static size_t to_file(char *data, size_t size, size_t count, void *opaque) {
    FileSink *sink = opaque;
    size_t written = fwrite(data, size, count, sink->file);
    if (written != count) sink->failed = 1;
    sink->written += (unsigned long long)written * size;
    return written * size;
}

static int on_progress(void *opaque, curl_off_t total, curl_off_t now,
                       curl_off_t upload_total, curl_off_t upload_now) {
    (void)upload_total; (void)upload_now;
    Transfer *t = opaque;
    if (SDL_AtomicGet(&abort_active)) return 1;
    /* Without a Content-Length only the bytes so far are known. */
    if (t->direct && SDL_GetTicks() - t->reported_at > 500) {
        t->reported_at = SDL_GetTicks();
        report(t->id, total > 0 ? (int)(now * 1000 / total) : -1,
               t->base + (unsigned long long)now);
    }
    return 0;
}

static size_t on_header(char *data, size_t size, size_t count, void *opaque) {
    Transfer *t = opaque;
    size_t bytes = size * count;
    if (bytes >= 5 && !memcmp(data, "HTTP/", 5)) t->range_total = 0;
    if (bytes > 14 && bytes < 128 && !strncasecmp(data, "Content-Range:", 14)) {
        char header[128];
        memcpy(header, data, bytes);
        header[bytes] = 0;
        if (sscanf(header + 14, " bytes %llu-%llu/%llu",
                   &t->range_first, &t->range_last, &t->range_total) != 3)
            t->range_total = 0;
    }
    return bytes;
}

static int wait_for_resume(void) {
    while (SDL_AtomicGet(&user_paused) && !SDL_AtomicGet(&abort_active))
        SDL_Delay(50);
    return !SDL_AtomicGet(&abort_active);
}

/* The worker owns the handle, including all pause and resume calls. */
static CURLcode perform_direct(CURL *curl) {
    CURLM *multi = curl_multi_init();
    if (!multi) return CURLE_OUT_OF_MEMORY;
    CURLcode result = CURLE_FAILED_INIT;
    CURLMcode status = curl_multi_add_handle(multi, curl);
    int running = 0, paused = 0;
    if (status == CURLM_OK) status = curl_multi_perform(multi, &running);
    while (status == CURLM_OK && running) {
        if (SDL_AtomicGet(&abort_active)) {
            result = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        int pause = SDL_AtomicGet(&user_paused);
        if (pause != paused) {
            result = curl_easy_pause(curl, pause ? CURLPAUSE_RECV : CURLPAUSE_CONT);
            if (result != CURLE_OK) break;
            result = CURLE_FAILED_INIT;
            paused = pause;
        }
        status = curl_multi_poll(multi, NULL, 0, 100, NULL);
        if (status == CURLM_OK) status = curl_multi_perform(multi, &running);
    }
    int queued = 0;
    CURLMsg *message = curl_multi_info_read(multi, &queued);
    if (message && message->msg == CURLMSG_DONE) result = message->data.result;
    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    return result;
}

/* One GET through the background gate. Slow Wi-Fi gets a stall limit
   rather than a total timeout, so large files can take as long as needed. */
static int fetch(CURL *curl, const char *url, curl_write_callback write, void *sink,
                 Transfer *transfer, char *effective, size_t effective_size,
                 char *error, size_t error_size) {
    curl_easy_reset(curl);
    char curl_error[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, STREAM_USER_AGENT);
    const char *referer = stream_referer(url);
    if (*referer) curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, transfer);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, transfer);
    if (transfer->range) curl_easy_setopt(curl, CURLOPT_RANGE, transfer->range);
#ifdef UAPLAY_CONSOLE
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
#endif
    net_tls_options(curl);
    transfer->status = 0;
    transfer->range_total = 0;
    if (!wait_for_resume()) {
        snprintf(error, error_size, TR("Скасовано","Canceled"));
        return 0;
    }
    Uint32 wait_started = SDL_GetTicks();
    if (!transfer->ungated) net_background_acquire(0);
    unsigned waited = SDL_GetTicks() - wait_started;
    CURLcode result = transfer->direct ? perform_direct(curl) : curl_easy_perform(curl);
    if (!transfer->ungated) net_background_release(0);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    transfer->status = status;
    curl_off_t received = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &received);
    net_profile_record('D', curl, url, waited, (int)result, (unsigned)received,
                       curl_error[0] ? curl_error : curl_easy_strerror(result));
    if (effective) {
        char *final_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final_url);
        snprintf(effective, effective_size, "%s", final_url ? final_url : url);
    }
    if (result != CURLE_OK) {
        snprintf(error, error_size, "%s", SDL_AtomicGet(&abort_active) ? TR("Скасовано","Canceled") :
                 curl_error[0] ? curl_error : curl_easy_strerror(result));
        return 0;
    }
    if (status >= 400) {
        snprintf(error, error_size, TR("Сервер відповів HTTP %ld","Server returned HTTP %ld"), status);
        return 0;
    }
    return 1;
}

#ifndef __vita__
typedef struct {
    CURL *curl;
    const char *url;
    unsigned id;
    Buffer data;
    unsigned long long first, last, total;  /* total is zero for HLS */
    int ok;
} DownloadRequest;

static int fetch_request(void *opaque) {
    DownloadRequest *r = opaque;
    char range[64], error[96];
    Transfer transfer = { .id = r->id };
    if (r->total) {
        snprintf(range, sizeof(range), "%llu-%llu", r->first, r->last);
        transfer.range = range;
    }
    r->ok = !SDL_AtomicGet(&abort_active) &&
            fetch(r->curl, r->url, to_buffer, &r->data, &transfer, NULL, 0, error, sizeof(error));
    if (r->total)
        r->ok = r->ok && transfer.status == 206 && transfer.range_total == r->total &&
                transfer.range_first == r->first && transfer.range_last == r->last &&
                r->data.used == r->last - r->first + 1;
    else
        r->ok = r->ok && transfer.status == 200 && r->data.used != 0;
    return 0;
}

/* The worker owns output and joins all transfers before writing any bytes. */
static int fetch_batch(DownloadRequest *requests, int count, CURL **extra) {
    SDL_Thread *threads[PARALLEL_REQUESTS - 1] = { 0 };
    for (int i = 1; i < count; i++) {
        if (!extra[i - 1]) extra[i - 1] = curl_easy_init();
        if (extra[i - 1]) {
            requests[i].curl = extra[i - 1];
            threads[i - 1] = SDL_CreateThread(fetch_request, "download", &requests[i]);
        }
    }
    fetch_request(&requests[0]);
    int ok = requests[0].ok;
    for (int i = 1; i < count; i++) {
        if (threads[i - 1]) SDL_WaitThread(threads[i - 1], NULL);
        ok = ok && requests[i].ok;
    }
    return ok;
}

static int write_batch(const DownloadRequest *requests, int count, FILE *out,
                       unsigned long long *written, char *error, size_t error_size) {
    for (int i = 0; i < count; i++) {
        const Buffer *data = &requests[i].data;
        if (fwrite(data->data, 1, data->used, out) != data->used) {
            snprintf(error, error_size, TR("Не вдалося записати файл (немає місця?)","Could not write the file. Check available space."));
            return 0;
        }
        *written += data->used;
    }
    return 1;
}

static void clear_batch(DownloadRequest *requests, int count) {
    for (int i = 0; i < count; i++) free(requests[i].data.data);
}

static void close_batch_handles(CURL **extra) {
    for (int i = 0; i < PARALLEL_REQUESTS - 1; i++)
        if (extra[i]) curl_easy_cleanup(extra[i]);
}
#endif

/* ---------- HLS ---------- */

/* Value of NAME=... in an #EXT tag line, quoted or not. */
static int attribute(const char *line, const char *name, char *out, size_t size) {
    size_t length = strlen(name);
    for (const char *p = strchr(line, ':'); p && *p; p++) {
        if ((*p == ':' || *p == ',') && !strncmp(p + 1, name, length) && p[1 + length] == '=') {
            const char *value = p + 2 + length;
            size_t n = 0;
            if (*value == '"') {
                value++;
                while (value[n] && value[n] != '"') n++;
            } else {
                while (value[n] && value[n] != ',') n++;
            }
            if (n >= size) n = size - 1;
            memcpy(out, value, n);
            out[n] = 0;
            return 1;
        }
    }
    out[0] = 0;
    return 0;
}

/* A playlist reference made absolute against the playlist's address. */
static void resolve_url(const char *base, const char *ref, char *out, size_t size) {
    if (strstr(ref, "://")) { snprintf(out, size, "%s", ref); return; }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) { snprintf(out, size, "%s", ref); return; }
    if (ref[0] == '/' && ref[1] == '/') {
        snprintf(out, size, "%.*s%s", (int)(scheme_end - base + 1), base, ref);
        return;
    }
    const char *host_end = strchr(scheme_end + 3, '/');
    if (!host_end) host_end = base + strlen(base);
    if (ref[0] == '/') {
        snprintf(out, size, "%.*s%s", (int)(host_end - base), base, ref);
        return;
    }
    const char *query = strchr(base, '?');
    const char *dir_end = base + (query ? (size_t)(query - base) : strlen(base));
    while (dir_end > host_end && dir_end[-1] != '/') dir_end--;
    if (dir_end <= host_end) snprintf(out, size, "%.*s/%s", (int)(host_end - base), base, ref);
    else snprintf(out, size, "%.*s%s", (int)(dir_end - base), base, ref);
}

/* Next line of a playlist (in place, without CR/LF); NULL at the end. */
static char *next_line(char **cursor) {
    char *line = *cursor;
    if (!line || !*line) return NULL;
    char *end = strchr(line, '\n');
    if (end) { *end = 0; *cursor = end + 1; }
    else *cursor = line + strlen(line);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    return line;
}

/* Master playlist: the variant to download, or 0 with an error. */
static int choose_variant(char *text, const char *base, char *out, size_t size,
                          char *error, size_t error_size) {
    char best[2048] = "", lowest[2048] = "", best_audio[64] = "", lowest_audio[64] = "";
    int best_height = -1, lowest_height = 1 << 30, any_height = 0;
    long best_bandwidth = -1, lowest_bandwidth = 1L << 30;
    char audio_groups[8][64];
    int audio_group_count = 0;
    char *cursor = text, *line, info[1024] = "";
    while ((line = next_line(&cursor))) {
        if (!strncmp(line, "#EXT-X-MEDIA:", 13)) {
            char type[16], uri[8], group[64];
            attribute(line, "TYPE", type, sizeof(type));
            /* An audio rendition with its own URI is a separate stream. */
            if (!strcmp(type, "AUDIO") && attribute(line, "URI", uri, sizeof(uri)) &&
                attribute(line, "GROUP-ID", group, sizeof(group)) && audio_group_count < 8)
                snprintf(audio_groups[audio_group_count++], 64, "%s", group);
        } else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            snprintf(info, sizeof(info), "%s", line);
        } else if (info[0] && line[0] && line[0] != '#') {
            char value[64], audio[64];
            long bandwidth = attribute(info, "BANDWIDTH", value, sizeof(value)) ? atol(value) : 0;
            int height = 0;
            if (attribute(info, "RESOLUTION", value, sizeof(value))) {
                char *x = strchr(value, 'x');
                height = x ? atoi(x + 1) : 0;
            }
            attribute(info, "AUDIO", audio, sizeof(audio));
            if (height) any_height = 1;
            int fits = height ? height <= VARIANT_MAX_HEIGHT : bandwidth <= VARIANT_FALLBACK_BANDWIDTH;
            int better = height ? height > best_height ||
                                  (height == best_height && bandwidth > best_bandwidth)
                                : bandwidth > best_bandwidth;
            if (fits && better) {
                best_height = height;
                best_bandwidth = bandwidth;
                resolve_url(base, line, best, sizeof(best));
                snprintf(best_audio, sizeof(best_audio), "%s", audio);
            }
            int smaller = height ? height < lowest_height : bandwidth < lowest_bandwidth;
            if (smaller) {
                lowest_height = height ? height : lowest_height;
                lowest_bandwidth = bandwidth;
                resolve_url(base, line, lowest, sizeof(lowest));
                snprintf(lowest_audio, sizeof(lowest_audio), "%s", audio);
            }
            info[0] = 0;
        }
    }
    (void)any_height;
    const char *chosen = best[0] ? best : lowest;
    const char *audio = best[0] ? best_audio : lowest_audio;
    if (!chosen[0]) {
        snprintf(error, error_size, TR("Порожній список якостей HLS","No HLS quality variants available"));
        return 0;
    }
    for (int i = 0; i < audio_group_count; i++)
        if (audio[0] && !strcmp(audio, audio_groups[i])) {
            snprintf(error, error_size, TR("Окрема аудіодоріжка: завантаження не підтримується","Downloads with a separate audio stream are unsupported"));
            return 0;
        }
    snprintf(out, size, "%s", chosen);
    return 1;
}

typedef struct {
    char url[2048];
    int method;               /* 0 none, 1 AES-128 */
    unsigned char iv[16];
    int has_iv;
    unsigned char key[16];
    int key_loaded;
} HlsKey;

typedef struct {
    char *url;
    int key;                  /* index in keys[] or -1 */
    long long sequence;
    int init;                 /* EXT-X-MAP initialisation section */
} HlsSegment;

static int hex_to_iv(const char *text, unsigned char *iv) {
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (strlen(text) != 32) return 0;
    for (int i = 0; i < 16; i++) {
        unsigned value;
        if (sscanf(text + i * 2, "%2x", &value) != 1) return 0;
        iv[i] = (unsigned char)value;
    }
    return 1;
}

#ifdef __vita__
static int decrypt(Buffer *b, const HlsKey *key, const unsigned char *iv) {
    unsigned char *plain = malloc(b->used + 16);
    if (!plain) return 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = 0, n = 0, tail = 0;
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key->key, iv) &&
        EVP_DecryptUpdate(ctx, plain, &n, (unsigned char *)b->data, (int)b->used) &&
        EVP_DecryptFinal_ex(ctx, plain + n, &tail)) {
        memcpy(b->data, plain, (size_t)(n + tail));
        b->used = (size_t)(n + tail);
        ok = 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    free(plain);
    return ok;
}
#endif

static int download_hls(CURL *curl, unsigned id, char *playlist, const char *base,
                        FILE *out, int *fragmented, unsigned long long *written,
                        char *error, size_t error_size) {
    static HlsKey keys[HLS_KEYS_MAX];
    int key_count = 0, current_key = -1, segment_count = 0, capacity = 0, ok = 0;
    HlsSegment *segments = NULL;
    long long sequence = 0;
#ifndef __vita__
    CURL *extra[PARALLEL_REQUESTS - 1] = { 0 };
    int parallel = 1;
#endif
    char *cursor = playlist, *line, url[2048];
    while ((line = next_line(&cursor))) {
        if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) sequence = atoll(line + 22);
        else if (!strncmp(line, "#EXT-X-BYTERANGE", 16)) {
            snprintf(error, error_size, TR("HLS з BYTERANGE не підтримується","HLS byte ranges are unsupported"));
            goto done;
        } else if (!strncmp(line, "#EXT-X-KEY:", 11)) {
            char method[32], value[2048];
            attribute(line, "METHOD", method, sizeof(method));
            if (!strcmp(method, "NONE")) { current_key = -1; continue; }
            if (strcmp(method, "AES-128")) {
                snprintf(error, error_size, TR("Шифрування %s не підтримується","Unsupported encryption: %s"), method);
                goto done;
            }
#ifndef __vita__
            snprintf(error, error_size, TR("Зашифрований потік не підтримується","Encrypted stream is unsupported"));
            goto done;
#endif
            if (key_count == HLS_KEYS_MAX) {
                snprintf(error, error_size, TR("Забагато ключів HLS","Too many HLS keys"));
                goto done;
            }
            HlsKey *key = &keys[key_count];
            memset(key, 0, sizeof(*key));
            key->method = 1;
            attribute(line, "URI", value, sizeof(value));
            resolve_url(base, value, key->url, sizeof(key->url));
            if (attribute(line, "IV", value, sizeof(value))) key->has_iv = hex_to_iv(value, key->iv);
            current_key = key_count++;
        } else if (line[0] == '#' && strncmp(line, "#EXT-X-MAP:", 11)) {
            continue;
        } else if (line[0]) {
            int init = !strncmp(line, "#EXT-X-MAP:", 11);
            char value[2048];
            if (init) {
                if (!attribute(line, "URI", value, sizeof(value))) continue;
                *fragmented = 1;
            } else snprintf(value, sizeof(value), "%s", line);
            if (segment_count == capacity) {
                capacity = capacity ? capacity * 2 : 256;
                HlsSegment *grown = realloc(segments, sizeof(HlsSegment) * (size_t)capacity);
                if (!grown) { snprintf(error, error_size, TR("Недостатньо пам'яті","Out of memory")); goto done; }
                segments = grown;
            }
            resolve_url(base, value, url, sizeof(url));
            HlsSegment *s = &segments[segment_count];
            s->url = strdup(url);
            if (!s->url) { snprintf(error, error_size, TR("Недостатньо пам'яті","Out of memory")); goto done; }
            s->key = current_key;
            s->sequence = init ? sequence : sequence++;
            s->init = init;
            segment_count++;
        }
    }
    if (!segment_count) {
        snprintf(error, error_size, TR("Порожній список сегментів HLS","No HLS segments available"));
        goto done;
    }
    Buffer data = { 0 };
    data.limit = SEGMENT_LIMIT;
    for (int i = 0; i < segment_count; i++) {
#ifndef __vita__
        if (parallel && segment_count - i >= 2) {
            int count = segment_count - i;
            if (count > PARALLEL_REQUESTS) count = PARALLEL_REQUESTS;
            DownloadRequest requests[PARALLEL_REQUESTS] = { 0 };
            requests[0].curl = curl;
            for (int j = 0; j < count; j++) {
                requests[j].id = id;
                requests[j].url = segments[i + j].url;
                requests[j].data.limit = PARALLEL_SEGMENT_LIMIT;
            }
            parallel = fetch_batch(requests, count, extra);
            if (parallel && !write_batch(requests, count, out, written, error, error_size)) {
                clear_batch(requests, count);
                free(data.data);
                goto done;
            }
            if (parallel) i += count - 1;
            clear_batch(requests, count);
            if (parallel) {
                if (!report(id, (i + 1) * 1000 / segment_count, *written)) {
                    snprintf(error, error_size, TR("Скасовано","Canceled"));
                    free(data.data);
                    goto done;
                }
                continue;
            }
        }
#endif
        HlsSegment *s = &segments[i];
        if (s->key >= 0 && !keys[s->key].key_loaded) {
            Buffer key_data = { 0 };
            key_data.limit = 4096;
            Transfer t = { .id = id };
            if (!fetch(curl, keys[s->key].url, to_buffer, &key_data, &t, NULL, 0, error,
                       error_size) || key_data.used != 16) {
                if (!error[0] || key_data.used != 16)
                    snprintf(error, error_size, TR("Не вдалося отримати ключ HLS","Could not fetch the HLS key"));
                free(key_data.data);
                free(data.data);
                goto done;
            }
            memcpy(keys[s->key].key, key_data.data, 16);
            keys[s->key].key_loaded = 1;
            free(key_data.data);
        }
        int fetched = 0;
        for (int attempt = 0; attempt < SEGMENT_ATTEMPTS && !fetched; attempt++) {
            if (SDL_AtomicGet(&abort_active)) break;
            if (attempt) SDL_Delay(1000u * (unsigned)attempt);
            data.used = 0;
            error[0] = 0;
            Transfer t = { .id = id, .base = *written };
            fetched = fetch(curl, s->url, to_buffer, &data, &t, NULL, 0, error, error_size);
        }
        if (!fetched) {
            if (!error[0]) snprintf(error, error_size, TR("Скасовано","Canceled"));
            free(data.data);
            goto done;
        }
#ifdef __vita__
        if (s->key >= 0) {
            unsigned char iv[16];
            if (keys[s->key].has_iv) memcpy(iv, keys[s->key].iv, 16);
            else {
                memset(iv, 0, sizeof(iv));
                for (int b = 0; b < 8; b++)
                    iv[15 - b] = (unsigned char)((unsigned long long)s->sequence >> (8 * b));
            }
            if (!decrypt(&data, &keys[s->key], iv)) {
                snprintf(error, error_size, TR("Не вдалося розшифрувати сегмент","Could not decrypt the segment"));
                free(data.data);
                goto done;
            }
        }
#endif
        if (data.used && fwrite(data.data, 1, data.used, out) != data.used) {
            snprintf(error, error_size, TR("Не вдалося записати файл (немає місця?)","Could not write the file. Check available space."));
            free(data.data);
            goto done;
        }
        *written += data.used;
        if (!report(id, (i + 1) * 1000 / segment_count, *written)) {
            snprintf(error, error_size, TR("Скасовано","Canceled"));
            free(data.data);
            goto done;
        }
    }
    free(data.data);
    ok = 1;
done:
#ifndef __vita__
    close_batch_handles(extra);
#endif
    for (int i = 0; i < segment_count; i++) free(segments[i].url);
    free(segments);
    return ok;
}

/* ---------- one item ---------- */

/* Reads a small file that turned out to be a playlist. */
static char *read_small_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *text = malloc(PLAYLIST_LIMIT + 1);
    size_t n = text ? fread(text, 1, PLAYLIST_LIMIT, file) : 0;
    fclose(file);
    if (text) text[n] = 0;
    return text;
}

static int download_one(CURL *curl, const Download *job, char *file, size_t file_size,
                        char *error, size_t error_size) {
    char effective[2048], url[2048];
    const char *part = job->file;
    if (!*part) {
        snprintf(error, error_size, TR("Не вдалося вибрати ім'я файлу","Could not choose a filename"));
        return 0;
    }
    snprintf(url, sizeof(url), "%s", job->url);
    FILE *out = fopen(part, "wb");
    if (!out) {
        snprintf(error, error_size, TR("Не вдалося створити файл","Could not create the file"));
        return 0;
    }
    static char buffer[256 * 1024];
    setvbuf(out, buffer, _IOFBF, sizeof(buffer));
    int ok = 0, fragmented = 0;
    int hls = movy_stream_url(url) || strstr(url, "m3u8") || strstr(url, ".m3u");
    unsigned long long written = 0;
    char *playlist = NULL;
#ifndef __vita__
    CURL *extra[PARALLEL_REQUESTS - 1] = { 0 };
    int parallel = 1;
#endif
    if (!hls) {
        /* Ask for the first byte: a 206 answer means the server takes
           ranges and tells the size; otherwise fetch the file in one go. */
        Buffer chunk = { 0 };
        chunk.limit = DIRECT_CHUNK + 64 * 1024;
        Transfer probe = { .id = job->id, .range = "0-0" };
        int probed = fetch(curl, url, to_buffer, &chunk, &probe, effective, sizeof(effective),
                           error, error_size);
        unsigned long long total = probe.status == 206 ? probe.range_total : 0;
        if (!probed && probe.status != 200) { free(chunk.data); goto done; }
        if (total) snprintf(url, sizeof(url), "%s", effective);
        FileSink sink = { out, 0, 0 };
        while (total && sink.written < total) {
#ifndef __vita__
            if (parallel && total - sink.written > DIRECT_CHUNK) {
                DownloadRequest requests[PARALLEL_REQUESTS] = { 0 };
                int count = 0;
                unsigned long long offset = sink.written;
                requests[0].curl = curl;
                while (count < PARALLEL_REQUESTS && offset < total) {
                    DownloadRequest *request = &requests[count++];
                    request->url = url;
                    request->id = job->id;
                    request->first = offset;
                    offset += total - offset < DIRECT_CHUNK ? total - offset : DIRECT_CHUNK;
                    request->last = offset - 1;
                    request->total = total;
                    request->data.limit = DIRECT_CHUNK + 1;
                }
                parallel = fetch_batch(requests, count, extra);
                if (parallel && !write_batch(requests, count, out, &sink.written, error, error_size)) {
                    clear_batch(requests, count);
                    free(chunk.data);
                    goto done;
                }
                clear_batch(requests, count);
                if (parallel) {
                    if (!report(job->id, (int)(sink.written * 1000 / total), sink.written)) {
                        snprintf(error, error_size, TR("Скасовано","Canceled"));
                        free(chunk.data);
                        goto done;
                    }
                    continue;
                }
            }
#endif
            char range[64];
            unsigned long long last = sink.written + DIRECT_CHUNK - 1;
            if (last >= total) last = total - 1;
            snprintf(range, sizeof(range), "%llu-%llu", sink.written, last);
            int fetched = 0;
            for (int attempt = 0; attempt < SEGMENT_ATTEMPTS && !fetched; attempt++) {
                if (SDL_AtomicGet(&abort_active)) break;
                if (attempt) SDL_Delay(1000u * (unsigned)attempt);
                chunk.used = 0;
                error[0] = 0;
                Transfer t = { .id = job->id, .range = range };
                fetched = fetch(curl, url, to_buffer, &chunk, &t, NULL, 0, error, error_size) &&
                          t.status == 206 && t.range_total == total &&
                          t.range_first == sink.written && t.range_last == last &&
                          chunk.used == last - sink.written + 1;
                if (!fetched && !error[0]) snprintf(error, error_size, TR("Сервер перервав передачу","The server interrupted the transfer"));
            }
            if (!fetched) {
                if (SDL_AtomicGet(&abort_active)) snprintf(error, error_size, TR("Скасовано","Canceled"));
                free(chunk.data);
                goto done;
            }
            if (fwrite(chunk.data, 1, chunk.used, out) != chunk.used) {
                snprintf(error, error_size, TR("Не вдалося записати файл (немає місця?)","Could not write the file. Check available space."));
                free(chunk.data);
                goto done;
            }
            sink.written += chunk.used;
            if (!report(job->id, (int)(sink.written * 1000 / total), sink.written)) {
                snprintf(error, error_size, TR("Скасовано","Canceled"));
                free(chunk.data);
                goto done;
            }
        }
        free(chunk.data);
        if (!total) {
            /* No ranges: one long transfer, outside the gate so it does
               not hold every other request back until it ends. */
            Transfer t = { .id = job->id, .direct = 1, .ungated = 1 };
            error[0] = 0;
            if (!fetch(curl, url, to_file, &sink, &t, effective, sizeof(effective), error,
                       error_size)) goto done;
        }
        if (sink.failed) {
            snprintf(error, error_size, TR("Не вдалося записати файл (немає місця?)","Could not write the file. Check available space."));
            goto done;
        }
        written = sink.written;
        /* A link without .m3u8 can still answer with a playlist. */
        if (written < PLAYLIST_LIMIT) {
            fflush(out);
            playlist = read_small_file(part);
            if (playlist && !strncmp(playlist, "#EXTM3U", 7)) {
                hls = 1;
                snprintf(url, sizeof(url), "%s", effective);
                fclose(out);
                out = fopen(part, "wb");
                if (!out) { snprintf(error, error_size, TR("Не вдалося створити файл","Could not create the file")); goto done; }
                setvbuf(out, buffer, _IOFBF, sizeof(buffer));
                written = 0;
            } else {
                free(playlist);
                playlist = NULL;
            }
        }
    }
    if (hls) {
        /* Master playlists can nest; follow at most a few levels. */
        for (int level = 0; level < 3; level++) {
            if (!playlist) {
                Buffer text = { 0 };
                text.limit = PLAYLIST_LIMIT;
                Transfer t = { .id = job->id };
                if (!fetch(curl, url, to_buffer, &text, &t, effective, sizeof(effective), error,
                           error_size)) { free(text.data); goto done; }
                playlist = text.data;
                snprintf(url, sizeof(url), "%s", effective);
            }
            if (!playlist || strncmp(playlist, "#EXTM3U", 7)) {
                snprintf(error, error_size, TR("Відповідь не схожа на HLS","The response is not an HLS playlist"));
                goto done;
            }
            if (!strstr(playlist, "#EXT-X-STREAM-INF")) break;
            char variant[2048];
            int chosen = choose_variant(playlist, url, variant, sizeof(variant), error, error_size);
            free(playlist);
            playlist = NULL;
            if (!chosen) goto done;
            snprintf(url, sizeof(url), "%s", variant);
        }
        if (!playlist) {
            snprintf(error, error_size, TR("Забагато рівнів HLS","Too many nested HLS playlists"));
            goto done;
        }
        if (!download_hls(curl, job->id, playlist, url, out, &fragmented, &written, error,
                          error_size)) goto done;
    }
    ok = 1;
done:
#ifndef __vita__
    close_batch_handles(extra);
#endif
    free(playlist);
    if (out && fclose(out) != 0 && ok) {
        snprintf(error, error_size, TR("Не вдалося записати файл (немає місця?)","Could not write the file. Check available space."));
        ok = 0;
    }
    if (!ok || !wait_for_resume()) { remove(part); return 0; }
    const char *ext = hls && !fragmented ? "ts" : "mp4";
    if (!finished_path(job, ext, file, file_size) || rename(part, file) != 0) {
        snprintf(error, error_size, TR("Не вдалося зберегти файл","Could not save the file"));
        remove(part);
        return 0;
    }
    report(job->id, 1000, written);
    return 1;
}

static int download_worker(void *opaque) {
    (void)opaque;
    CURL *curl = curl_easy_init();
    for (;;) {
        SDL_LockMutex(lock);
        Download *next = NULL;
        while (!quitting) {
            for (int i = 0; i < item_count && !next; i++)
                if (items[i].state == DOWNLOAD_QUEUED) next = &items[i];
            if (next) break;
            SDL_CondWait(wake, lock);
        }
        if (quitting) { SDL_UnlockMutex(lock); break; }
        next->state = DOWNLOAD_ACTIVE;
        next->permille = 0;
        next->bytes = 0;
        next->error[0] = 0;
        next->file[0] = 0;
        if (!finished_path(next, "part", next->file, sizeof(next->file)))
            next->file[0] = 0;
        static Download job;
        job = *next;
        active_id = job.id;
        speed_sample_at = speed_updated_at = SDL_GetTicks();
        speed_sample_bytes = 0;
        download_rate = 0;
        SDL_AtomicSet(&abort_active, 0);
        SDL_AtomicSet(&user_paused, 0);
        save_index();
        SDL_UnlockMutex(lock);

        char file[160] = "", error[96] = "";
        int ok = curl && download_one(curl, &job, file, sizeof(file), error, sizeof(error));
        if (!curl) snprintf(error, sizeof(error), TR("curl недоступний","curl is unavailable"));

        SDL_LockMutex(lock);
        active_id = 0;
        Download *item = find(job.id);
        if (!item) {
            /* Removed while downloading. */
            if (ok) remove(file);
        } else if (quitting) {
            item->file[0] = 0;
            if (ok) remove(file);
            if (item->state != DOWNLOAD_PAUSED) item->state = DOWNLOAD_QUEUED;
            item->permille = 0;
            item->bytes = 0;
        } else if (ok) {
            item->state = DOWNLOAD_DONE;
            item->permille = 1000;
            snprintf(item->file, sizeof(item->file), "%s", file);
        } else {
            item->file[0] = 0;
            item->state = DOWNLOAD_FAILED;
            snprintf(item->error, sizeof(item->error), "%s", error);
        }
        save_index();
        SDL_UnlockMutex(lock);
    }
    if (curl) curl_easy_cleanup(curl);
    return 0;
}

/* ---------- API ---------- */

void downloads_init(void) {
    mkdir(UAPLAY_DATA_DIR, 0777);
    mkdir(DOWNLOADS_DIR, 0777);
    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    if (!lock || !wake) return;
    load_index();
    worker = SDL_CreateThread(download_worker, "downloads", NULL);
}

void downloads_shutdown(void) {
    if (!lock) return;
    SDL_LockMutex(lock);
    quitting = 1;
    SDL_AtomicSet(&abort_active, 1);
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
    if (worker) SDL_WaitThread(worker, NULL);
    worker = NULL;
}

int downloads_add(const Movie *movie, int season, int episode, const char *label,
                  const char *url) {
    if (!lock || !url || !*url) return -1;
    SDL_LockMutex(lock);
    int result = 1;
    Download *d = NULL;
    for (int i = 0; i < item_count && !d; i++)
        if (items[i].movie.id == movie->id && items[i].movie.series == movie->series &&
            items[i].season == season && items[i].episode == episode) d = &items[i];
    if (d && d->state != DOWNLOAD_FAILED) result = 0;
    else if (!d && item_count == DOWNLOADS_MAX) result = -1;
    else {
        if (!d) {
            d = &items[item_count++];
            memset(d, 0, sizeof(*d));
            d->id = next_id++;
        }
        d->movie = *movie;
        d->season = season;
        d->episode = episode;
        snprintf(d->label, sizeof(d->label), "%s", label ? label : "");
        snprintf(d->url, sizeof(d->url), "%s", url);
        d->state = DOWNLOAD_QUEUED;
        d->permille = 0;
        d->bytes = 0;
        d->error[0] = 0;
        save_index();
        SDL_CondSignal(wake);
    }
    SDL_UnlockMutex(lock);
    return result;
}

int downloads_list(DownloadInfo *out, int max) {
    if (!lock) return 0;
    SDL_LockMutex(lock);
    int count = 0;
    for (int i = 0; i < item_count && count < max; i++) {
        const Download *d = &items[i];
        DownloadInfo *info = &out[count++];
        info->id = d->id;
        snprintf(info->title, sizeof(info->title), "%s", d->movie.title);
        snprintf(info->label, sizeof(info->label), "%s", d->label);
        snprintf(info->error, sizeof(info->error), "%s", d->error);
        info->state = d->state;
        info->permille = d->permille;
        info->season = d->season;
        info->episode = d->episode;
        info->bytes = d->bytes;
    }
    SDL_UnlockMutex(lock);
    return count;
}

int downloads_get(unsigned id, Download *out) {
    if (!lock) return 0;
    SDL_LockMutex(lock);
    Download *d = find(id);
    if (d) *out = *d;
    SDL_UnlockMutex(lock);
    return d != NULL;
}

void downloads_remove(unsigned id) {
    if (!lock) return;
    SDL_LockMutex(lock);
    for (int i = 0; i < item_count; i++) {
        if (items[i].id != id) continue;
        /* The worker deletes a running download's files when it stops. */
        if (id == active_id) SDL_AtomicSet(&abort_active, 1);
        else if (items[i].state == DOWNLOAD_DONE) remove(items[i].file);
        memmove(&items[i], &items[i + 1], sizeof(Download) * (size_t)(item_count - i - 1));
        item_count--;
        save_index();
        break;
    }
    SDL_UnlockMutex(lock);
}

int downloads_toggle_pause(unsigned id) {
    if (!lock) return -1;
    SDL_LockMutex(lock);
    Download *d = find(id);
    int state = -1;
    if (d && (d->state == DOWNLOAD_ACTIVE || d->state == DOWNLOAD_QUEUED ||
              d->state == DOWNLOAD_PAUSED)) {
        int running = id == active_id;
        int pause = d->state != DOWNLOAD_PAUSED;
        state = d->state = pause ? DOWNLOAD_PAUSED : running ? DOWNLOAD_ACTIVE : DOWNLOAD_QUEUED;
        if (running) {
            SDL_AtomicSet(&user_paused, pause);
            speed_sample_at = speed_updated_at = SDL_GetTicks();
            speed_sample_bytes = d->bytes;
            download_rate = 0;
        }
        save_index();
        SDL_CondSignal(wake);
    }
    SDL_UnlockMutex(lock);
    return state;
}

int downloads_progress(DownloadProgress *progress) {
    *progress = (DownloadProgress){ .seconds_left = -1 };
    if (!lock) return 0;
    SDL_LockMutex(lock);
    Download *d = active_id ? find(active_id) : NULL;
    int active = d && d->state == DOWNLOAD_ACTIVE;
    if (active) {
        progress->id = d->id;
        const char *name = d->file + (d->file[0] ? sizeof(DOWNLOADS_DIR "/") - 1 : 0);
        const char *extension = strrchr(name, '.');
        if (extension)
            snprintf(progress->name, sizeof(progress->name), "%.*s", (int)(extension - name), name);
        progress->permille = d->permille;
        progress->bytes = d->bytes;

        if (SDL_GetTicks() - speed_updated_at < 10000)
            progress->bytes_per_second = download_rate;
        if (progress->bytes_per_second > 0 && d->permille > 0 && d->permille < 1000) {
            double remaining = (double)d->bytes * (1000 - d->permille) / d->permille;
            double seconds = remaining / progress->bytes_per_second;
            if (seconds < 864000) progress->seconds_left = (int)seconds + 1;
        }
    }
    SDL_UnlockMutex(lock);
    return active;
}

int downloads_busy(void) {
    if (!lock) return 0;
    SDL_LockMutex(lock);
    int busy = active_id != 0;
    for (int i = 0; i < item_count && !busy; i++)
        busy = items[i].state == DOWNLOAD_QUEUED || items[i].state == DOWNLOAD_ACTIVE;
    SDL_UnlockMutex(lock);
    return busy;
}

void downloads_retry(unsigned id) {
    if (!lock) return;
    SDL_LockMutex(lock);
    Download *d = find(id);
    if (d && d->state == DOWNLOAD_FAILED) {
        d->state = DOWNLOAD_QUEUED;
        d->permille = 0;
        d->bytes = 0;
        d->error[0] = 0;
        save_index();
        SDL_CondSignal(wake);
    }
    SDL_UnlockMutex(lock);
}

void downloads_play_path(const Download *d, char *out, size_t size) {
#ifdef __vita__
    /* FFmpeg would read "ux0:" as a protocol name. */
    snprintf(out, size, "file:%s", d->file);
#else
    snprintf(out, size, "%s", d->file);
#endif
}
