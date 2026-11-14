/*
 * media.h - media directory listing and premade thumbnail cache.
 *
 * Media lives on the FAT32 boot partition (normally
 * /boot/firmware/Camera).  The Python daemon writes one 320x240 RGB565
 * preview next to the media, under .preview/, named after the media file
 * without its extension:
 *
 *     IMG_0001.jpg  ->  .preview/IMG_0001.565
 *     VID_0002.mp4  ->  .preview/VID_0002.565
 *
 * A preview is exactly 320 * 240 * 2 = 153600 bytes, row major, native
 * little-endian uint16_t.  The UI never decodes JPEG or video: it only ever
 * blits these buffers.
 *
 * The list is rebuilt only when media_scan() is called.  There is no
 * allocation on the per-frame path.
 *
 * Not thread safe: the UI is single threaded.
 */
#ifndef MEDIA_H
#define MEDIA_H

#include <stddef.h>
#include <stdint.h>

#define MEDIA_PREVIEW_W 320
#define MEDIA_PREVIEW_H 240
#define MEDIA_PREVIEW_BYTES \
    ((size_t)MEDIA_PREVIEW_W * (size_t)MEDIA_PREVIEW_H * sizeof(uint16_t))

typedef struct {
    char name[64]; /* base name with extension, e.g. "IMG_0001.jpg" */
    int is_video;  /* 1 for .mp4 */
    long mtime;    /* st_mtime, for sorting */
} MediaItem;

typedef struct {
    MediaItem *items;
    int count;
    int capacity;
    char dir[256];
} MediaList;

/*
 * Rescan `dir`.  Keeps only .jpg/.jpeg/.mp4 (case-insensitive), newest first
 * by mtime, ties broken by name descending (so IMG_0010 sorts before
 * IMG_0009).  Existing contents are freed first.
 *
 * Returns the item count, or -1 on error (the list is left empty, never
 * half-built).  `list` must be zero-initialised before the first call.
 */
int media_scan(MediaList *list, const char *dir);

/* Same as media_scan() but drops videos: the native UI is photo-only. */
int media_scan_images(MediaList *list, const char *dir);

/* Free the items and reset the list.  Safe to call more than once. */
void media_free(MediaList *list);

/*
 * Load <dir>/.preview/<name without extension>.565 into an internal
 * one-entry cache.  Returns a pointer to 320*240 uint16, or NULL when the
 * file is missing or short.  Repeated calls for the same file do not re-read
 * it.  `w` and `h` may be NULL.
 */
const uint16_t *media_preview(const char *dir, const char *name, int *w,
                              int *h);

/* Drop the cached preview (after a new capture). */
void media_preview_invalidate(void);

/*
 * How many preview files exist vs how many media files: `have` counts only
 * complete 153600-byte .565 files, `total` counts the media files a scan
 * would list.  Both are set to 0 on error.  Either out pointer may be NULL.
 */
void media_preview_stats(const char *dir, int *have, int *total);

#endif /* MEDIA_H */
