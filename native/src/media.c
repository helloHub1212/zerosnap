#include "media.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Longest path we build: dir[256] + '/' + d_name[256] + NUL. */
#define MEDIA_PATH_MAX 512

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

/* Locale-independent, extension only case folding. */
static int ascii_caseeq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * Return 1 when `name` is a supported media file, setting *is_video.
 * Only the text after the last dot is examined.
 */
static int classify_name(const char *name, int *is_video) {
    const char *dot = strrchr(name, '.');
    const char *ext;

    if (dot == NULL || dot[1] == '\0') {
        return 0;
    }
    ext = dot + 1;
    if (ascii_caseeq(ext, "jpg") || ascii_caseeq(ext, "jpeg")) {
        if (is_video != NULL) {
            *is_video = 0;
        }
        return 1;
    }
    if (ascii_caseeq(ext, "mp4")) {
        if (is_video != NULL) {
            *is_video = 1;
        }
        return 1;
    }
    return 0;
}

/* snprintf wrapper that refuses truncated results. */
static int join_path(char *out, size_t outlen, const char *dir,
                     const char *name) {
    int n = snprintf(out, outlen, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= outlen) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* List handling                                                       */
/* ------------------------------------------------------------------ */

void media_free(MediaList *list) {
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->dir[0] = '\0';
}

/* Grow by doubling, exactly like the rest of the UI code. */
static int ensure_capacity(MediaList *list) {
    int cap;
    MediaItem *grown;

    if (list->count < list->capacity) {
        return 0;
    }
    cap = list->capacity > 0 ? list->capacity * 2 : 16;
    if (cap <= list->capacity) { /* overflow */
        return -1;
    }
    grown = realloc(list->items, (size_t)cap * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    list->items = grown;
    list->capacity = cap;
    return 0;
}

static int push_item(MediaList *list, const MediaItem *item) {
    if (ensure_capacity(list) != 0) {
        return -1;
    }
    list->items[list->count++] = *item;
    return 0;
}

/* Total order: newest first, then name descending. */
static int item_before(const MediaItem *a, const MediaItem *b) {
    if (a->mtime != b->mtime) {
        return a->mtime > b->mtime;
    }
    return strcmp(a->name, b->name) > 0;
}

static void merge_sort(MediaItem *items, MediaItem *scratch, int lo, int hi) {
    int mid;
    int i;
    int j;
    int k;

    if (hi - lo < 2) {
        return;
    }
    mid = lo + (hi - lo) / 2;
    merge_sort(items, scratch, lo, mid);
    merge_sort(items, scratch, mid, hi);

    i = lo;
    j = mid;
    k = lo;
    while (i < mid && j < hi) {
        /* Take the right run only when it is strictly earlier, so equal
         * keys keep their scan order. */
        if (item_before(&items[j], &items[i])) {
            scratch[k++] = items[j++];
        } else {
            scratch[k++] = items[i++];
        }
    }
    while (i < mid) {
        scratch[k++] = items[i++];
    }
    while (j < hi) {
        scratch[k++] = items[j++];
    }
    memcpy(items + lo, scratch + lo, (size_t)(hi - lo) * sizeof(*items));
}

static void insertion_sort(MediaItem *items, int count) {
    int i;

    for (i = 1; i < count; ++i) {
        MediaItem key = items[i];
        int j = i - 1;
        while (j >= 0 && item_before(&key, &items[j])) {
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = key;
    }
}

static void sort_items(MediaItem *items, int count) {
    MediaItem *scratch;

    if (count < 2) {
        return;
    }
    if ((size_t)count > SIZE_MAX / sizeof(*scratch)) {
        insertion_sort(items, count);
        return;
    }
    scratch = malloc((size_t)count * sizeof(*scratch));
    if (scratch == NULL) {
        /* 150 KB of media is more important than the last few ms of
         * sorting time; correctness is preserved. */
        insertion_sort(items, count);
        return;
    }
    merge_sort(items, scratch, 0, count);
    free(scratch);
}

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

int media_scan(MediaList *list, const char *dir) {
    DIR *dp;
    struct dirent *ent;
    int failed = 0;

    if (list == NULL) {
        return -1;
    }
    media_free(list);

    if (dir == NULL || dir[0] == '\0') {
        return -1;
    }
    {
        int n = snprintf(list->dir, sizeof(list->dir), "%s", dir);
        if (n < 0 || (size_t)n >= sizeof(list->dir)) {
            list->dir[0] = '\0';
            return -1;
        }
    }

    dp = opendir(dir);
    if (dp == NULL) {
        media_free(list);
        return -1;
    }

    while (!failed && (ent = readdir(dp)) != NULL) {
        const char *name = ent->d_name;
        MediaItem item;
        char path[MEDIA_PATH_MAX];
        struct stat st;
        int is_video;
        size_t nlen;

        /* Skips ".", "..", ".preview" and any other hidden entry. */
        if (name[0] == '.') {
            continue;
        }
        if (!classify_name(name, &is_video)) {
            continue;
        }
        nlen = strlen(name);
        if (nlen >= sizeof(item.name)) {
            continue; /* cannot represent it, do not truncate a file name */
        }
        if (join_path(path, sizeof(path), dir, name) != 0) {
            continue;
        }
        /* FAT32 cards can be yanked at any time: a vanished file is not an
         * error, just something we will not list. */
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        memcpy(item.name, name, nlen + 1);
        item.is_video = is_video;
        item.mtime = (long)st.st_mtime;

        if (push_item(list, &item) != 0) {
            failed = 1;
        }
    }
    closedir(dp);

    if (failed) {
        media_free(list);
        return -1;
    }

    sort_items(list->items, list->count);
    return list->count;
}

/* ------------------------------------------------------------------ */
/* Preview cache                                                       */
/* ------------------------------------------------------------------ */

static uint16_t *g_preview_pixels;
static int g_preview_valid;
static char g_preview_key[MEDIA_PATH_MAX];

/*
 * <dir>/.preview/<name minus last extension>.565
 * Returns -1 on an unrepresentable name/path instead of truncating.
 */
static int preview_path(char *out, size_t outlen, const char *dir,
                        const char *name) {
    const char *dot;
    size_t stem;
    int n;

    if (dir == NULL || dir[0] == '\0' || name == NULL || name[0] == '\0') {
        return -1;
    }
    /* `name` is a base name from media_scan(); never let it escape the
     * preview directory. */
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return -1;
    }
    dot = strrchr(name, '.');
    stem = dot != NULL ? (size_t)(dot - name) : strlen(name);
    if (stem == 0) {
        return -1;
    }

    n = snprintf(out, outlen, "%s/.preview/%.*s.565", dir, (int)stem, name);
    if (n < 0 || (size_t)n >= outlen) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static void report_size(int *w, int *h) {
    if (w != NULL) {
        *w = MEDIA_PREVIEW_W;
    }
    if (h != NULL) {
        *h = MEDIA_PREVIEW_H;
    }
}

int media_scan_images(MediaList *list, const char *dir) {
    int count = media_scan(list, dir);
    int out = 0;
    if (count <= 0) {
        return count;
    }
    for (int i = 0; i < count; ++i) {
        if (!list->items[i].is_video) {
            list->items[out++] = list->items[i];
        }
    }
    list->count = out;
    return out;
}

const uint16_t *media_preview(const char *dir, const char *name, int *w,
                              int *h) {
    char path[MEDIA_PATH_MAX];
    FILE *fp;
    struct stat st;
    size_t got;

    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }

    if (preview_path(path, sizeof(path), dir, name) != 0) {
        return NULL;
    }

    /* One entry, keyed by the resolved .565 path. */
    if (g_preview_valid && strcmp(g_preview_key, path) == 0) {
        report_size(w, h);
        return g_preview_pixels;
    }

    /* Whatever happens next, the old entry is gone.  A failed load must not
     * leave a stale cache entry behind. */
    g_preview_valid = 0;
    g_preview_key[0] = '\0';

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    /* Single stat, on the already-open descriptor: no stat/open race and no
     * double stat. */
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 ||
        (uint64_t)st.st_size < (uint64_t)MEDIA_PREVIEW_BYTES) {
        fclose(fp);
        return NULL;
    }
    if (g_preview_pixels == NULL) {
        g_preview_pixels = malloc(MEDIA_PREVIEW_BYTES);
        if (g_preview_pixels == NULL) {
            fclose(fp);
            return NULL;
        }
    }
    got = fread(g_preview_pixels, 1, MEDIA_PREVIEW_BYTES, fp);
    if (fclose(fp) != 0 || got != MEDIA_PREVIEW_BYTES) {
        return NULL;
    }

    snprintf(g_preview_key, sizeof(g_preview_key), "%s", path);
    g_preview_valid = 1;
    report_size(w, h);
    return g_preview_pixels;
}

void media_preview_invalidate(void) {
    g_preview_valid = 0;
    g_preview_key[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Stats                                                               */
/* ------------------------------------------------------------------ */

static int count_media(const char *dir) {
    DIR *dp;
    struct dirent *ent;
    int count = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        return 0;
    }
    while ((ent = readdir(dp)) != NULL) {
        char path[MEDIA_PATH_MAX];
        struct stat st;

        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!classify_name(ent->d_name, NULL)) {
            continue;
        }
        if (join_path(path, sizeof(path), dir, ent->d_name) != 0) {
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        ++count;
    }
    closedir(dp);
    return count;
}

static int count_previews(const char *dir) {
    char preview_dir[MEDIA_PATH_MAX];
    DIR *dp;
    struct dirent *ent;
    int count = 0;

    if (join_path(preview_dir, sizeof(preview_dir), dir, ".preview") != 0) {
        return 0;
    }
    dp = opendir(preview_dir);
    if (dp == NULL) {
        return 0;
    }
    while ((ent = readdir(dp)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        char path[MEDIA_PATH_MAX];
        struct stat st;

        if (ent->d_name[0] == '.') {
            continue;
        }
        if (dot == NULL || !ascii_caseeq(dot + 1, "565")) {
            continue;
        }
        if (join_path(path, sizeof(path), preview_dir, ent->d_name) != 0) {
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (st.st_size == (off_t)MEDIA_PREVIEW_BYTES) {
            ++count;
        }
    }
    closedir(dp);
    return count;
}

void media_preview_stats(const char *dir, int *have, int *total) {
    if (have != NULL) {
        *have = 0;
    }
    if (total != NULL) {
        *total = 0;
    }
    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    if (total != NULL) {
        *total = count_media(dir);
    }
    if (have != NULL) {
        *have = count_previews(dir);
    }
}
