/*
 * media_test.c - host test for src/media.c.
 *
 * Builds a FAT32-like fixture under /tmp/opencode/media_test/fixture and
 * exercises scanning, preview caching and preview stats.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/media.h"

#define FIXTURE "/tmp/opencode/media_test/fixture"
#define PREVIEW_W 320
#define PREVIEW_H 240

static int checks;
static int failures;

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void ok(int condition, const char *what) {
    ++checks;
    if (condition) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

static void check_int(int got, int want, const char *what) {
    ++checks;
    if (got == want) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s (got %d, want %d)\n", what, got, want);
        ++failures;
    }
}

static void check_long(long got, long want, const char *what) {
    ++checks;
    if (got == want) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s (got %ld, want %ld)\n", what, got, want);
        ++failures;
    }
}

static void check_name(const MediaList *list, int index, const char *want,
                       const char *what) {
    ++checks;
    if (index >= 0 && index < list->count &&
        strcmp(list->items[index].name, want) == 0) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s (got \"%s\", want \"%s\")\n", what,
               (index >= 0 && index < list->count) ? list->items[index].name
                                                   : "<missing>",
               want);
        ++failures;
    }
}

static void check_video(const MediaList *list, int index, int want,
                        const char *what) {
    ++checks;
    if (index >= 0 && index < list->count &&
        (list->items[index].is_video != 0) == (want != 0)) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s (is_video wrong or index missing)\n", what);
        ++failures;
    }
}

/* ------------------------------------------------------------------ */
/* Filesystem fixture helpers                                          */
/* ------------------------------------------------------------------ */

static int join(char *out, size_t outlen, const char *dir, const char *name) {
    int n = snprintf(out, outlen, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

static int remove_tree(const char *path) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        return 0; /* nothing to remove */
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dp = opendir(path);
        if (dp != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dp)) != NULL) {
                char child[512];
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                if (join(child, sizeof(child), path, ent->d_name) == 0) {
                    remove_tree(child);
                }
            }
            closedir(dp);
        }
        return rmdir(path);
    }
    return unlink(path);
}

static int make_dir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    perror(path);
    return -1;
}

static int make_empty_file(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

static int write_pattern_preview(const char *path, uint16_t pixel) {
    size_t count = (size_t)PREVIEW_W * PREVIEW_H;
    uint16_t *buf = malloc(count * sizeof(*buf));
    FILE *fp;
    size_t wrote;

    if (buf == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        buf[i] = pixel;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        free(buf);
        perror(path);
        return -1;
    }
    wrote = fwrite(buf, sizeof(*buf), count, fp);
    free(buf);
    {
        int closed = fclose(fp) == 0;
        if (wrote != count || !closed) {
            return -1;
        }
    }
    return 0;
}

static int write_truncated_preview(const char *path) {
    unsigned char buf[100];
    FILE *fp;
    size_t wrote;

    memset(buf, 0x5A, sizeof(buf));
    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    wrote = fwrite(buf, 1, sizeof(buf), fp);
    {
        int closed = fclose(fp) == 0;
        if (wrote != sizeof(buf) || !closed) {
            return -1;
        }
    }
    return 0;
}

static int set_mtime(const char *path, long seconds) {
    struct timespec times[2];
    times[0].tv_sec = (time_t)seconds;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    return utimensat(AT_FDCWD, path, times, 0);
}

/* ------------------------------------------------------------------ */
/* Test                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    MediaList list = {0};
    char preview_path[512];
    char preview_dir[512];
    int result;
    const uint16_t *first;
    const uint16_t *again;
    const uint16_t *reloaded;
    int w;
    int h;

    /* ---- fixture ------------------------------------------------- */
    remove_tree(FIXTURE);
    if (make_dir("/tmp/opencode/media_test") != 0 ||
        make_dir(FIXTURE) != 0) {
        fprintf(stderr, "cannot create fixture directory\n");
        return 2;
    }
    if (join(preview_dir, sizeof(preview_dir), FIXTURE, ".preview") != 0 ||
        make_dir(preview_dir) != 0) {
        fprintf(stderr, "cannot create preview directory\n");
        return 2;
    }

    ok(make_empty_file(FIXTURE "/IMG_0001.jpg") == 0,
       "fixture: create IMG_0001.jpg");
    ok(make_empty_file(FIXTURE "/IMG_0002.jpg") == 0,
       "fixture: create IMG_0002.jpg");
    ok(make_empty_file(FIXTURE "/VID_0003.mp4") == 0,
       "fixture: create VID_0003.mp4");
    ok(make_empty_file(FIXTURE "/IMG_0010.JPG") == 0,
       "fixture: create IMG_0010.JPG");
    ok(make_empty_file(FIXTURE "/notes.txt") == 0,
       "fixture: create notes.txt");
    ok(make_empty_file(FIXTURE "/.hidden.jpg") == 0,
       "fixture: create hidden .hidden.jpg");
    /* A directory whose name looks like media must not be listed. */
    ok(make_dir(FIXTURE "/IMG_0004.jpg") == 0,
       "fixture: create directory IMG_0004.jpg");
    ok(join(preview_path, sizeof(preview_path), preview_dir,
            "IMG_0001.565") == 0 &&
           write_pattern_preview(preview_path, 0x1234) == 0,
       "fixture: write full preview IMG_0001.565");
    ok(write_truncated_preview(FIXTURE "/.preview/IMG_0002.565") == 0,
       "fixture: write truncated preview IMG_0002.565");

    /* Explicit mtimes: IMG_0010.JPG and IMG_0002.jpg tie at 2000 so the
     * name-descending tie break is exercised. */
    ok(set_mtime(FIXTURE "/IMG_0001.jpg", 1000) == 0, "fixture: mtime 0001");
    ok(set_mtime(FIXTURE "/IMG_0002.jpg", 2000) == 0, "fixture: mtime 0002");
    ok(set_mtime(FIXTURE "/VID_0003.mp4", 3000) == 0, "fixture: mtime 0003");
    ok(set_mtime(FIXTURE "/IMG_0010.JPG", 2000) == 0, "fixture: mtime 0010");
    ok(set_mtime(FIXTURE "/notes.txt", 5000) == 0, "fixture: mtime notes");

    /* ---- scan ---------------------------------------------------- */
    result = media_scan(&list, FIXTURE);
    check_int(result, 4, "media_scan returns 4 items");
    check_int(list.count, 4, "media_scan leaves count == 4");
    check_name(&list, 0, "VID_0003.mp4", "order[0] is newest VID_0003.mp4");
    check_video(&list, 0, 1, "VID_0003.mp4 is_video == 1");
    check_name(&list, 1, "IMG_0010.JPG",
               "order[1] is IMG_0010.JPG (mtime tie, name descending)");
    check_video(&list, 1, 0, "uppercase .JPG is_video == 0");
    check_name(&list, 2, "IMG_0002.jpg",
               "order[2] is IMG_0002.jpg (mtime tie, name descending)");
    check_name(&list, 3, "IMG_0001.jpg", "order[3] is oldest IMG_0001.jpg");
    if (list.count == 4) {
        check_long(list.items[0].mtime, 3000, "mtime is copied from stat");
        check_long(list.items[3].mtime, 1000, "oldest mtime is 1000");
    }

    /* Re-scan must free the old contents and rebuild cleanly. */
    result = media_scan(&list, FIXTURE);
    check_int(result, 4, "second media_scan still returns 4 items");

    /* ---- preview cache ------------------------------------------- */
    first = media_preview(FIXTURE, "IMG_0001.jpg", &w, &h);
    ok(first != NULL, "media_preview(IMG_0001) returns non-NULL");
    check_int(w, PREVIEW_W, "preview width is 320");
    check_int(h, PREVIEW_H, "preview height is 240");
    ok(first != NULL && first[0] == 0x1234,
       "preview first pixel is the 0x1234 pattern");
    ok(first != NULL && first[(size_t)PREVIEW_W * PREVIEW_H - 1] == 0x1234,
       "preview last pixel is the 0x1234 pattern");

    ok(media_preview(FIXTURE, "IMG_0001.jpg", NULL, NULL) == first,
       "NULL w/h accepted and cache returns the same pointer");
    ok(first != NULL && first[0] == 0x1234, "cached content is unchanged");

    /* Rewrite the file but keep the cache: the old bytes must still come
     * back (proves the file is not re-read for the same name). */
    ok(write_pattern_preview(FIXTURE "/.preview/IMG_0001.565", 0x5678) == 0,
       "rewrite preview with the 0x5678 pattern");
    again = media_preview(FIXTURE, "IMG_0001.jpg", &w, &h);
    ok(again == first, "cache hit returns the same buffer pointer");
    ok(again != NULL && again[0] == 0x1234,
       "cache hit does not re-read the rewritten file");

    media_preview_invalidate();
    reloaded = media_preview(FIXTURE, "IMG_0001.jpg", &w, &h);
    ok(reloaded != NULL, "preview loads again after invalidate");
    ok(reloaded != NULL && reloaded[0] == 0x5678,
       "invalidate forces a reload of the rewritten content");

    /* ---- failures ------------------------------------------------ */
    ok(media_preview(FIXTURE, "IMG_0002.jpg", &w, &h) == NULL,
       "truncated preview (100 bytes) returns NULL");
    ok(media_preview(FIXTURE, "IMG_0099.jpg", &w, &h) == NULL,
       "missing preview returns NULL");
    reloaded = media_preview(FIXTURE, "IMG_0001.jpg", NULL, NULL);
    ok(reloaded != NULL && reloaded[0] == 0x5678,
       "valid preview still loads after failed lookups");

    /* ---- stats --------------------------------------------------- */
    {
        int have = -1;
        int total = -1;
        media_preview_stats(FIXTURE, &have, &total);
        check_int(have, 1, "media_preview_stats have == 1 (complete only)");
        check_int(total, 4, "media_preview_stats total == 4");
    }

    /* ---- nonexistent directory ----------------------------------- */
    result = media_scan(&list, "/tmp/opencode/media_test/does-not-exist");
    check_int(result, -1, "scan of nonexistent directory returns -1");
    check_int(list.count, 0, "failed scan leaves count == 0");
    ok(list.items == NULL, "failed scan leaves no item array");

    /* Scan the valid fixture again after the failure. */
    check_int(media_scan(&list, FIXTURE), 4,
              "media_scan recovers after a failed scan");
    media_free(&list);
    check_int(list.count, 0, "media_free resets count");
    ok(list.items == NULL, "media_free releases the item array");

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
