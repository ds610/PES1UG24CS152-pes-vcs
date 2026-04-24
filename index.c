// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c

#include "pes.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    if (!index) return -1;

    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;   /* no index yet is fine */

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long long mtime, size;

        int rc = sscanf(line, "%o %64s %llu %llu %255[^\n]",
                        &mode, hex, &mtime, &size, e->path);
        if (rc != 5) continue;

        e->mode      = (uint32_t)mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint64_t)size;

        if (hex_to_hash(hex, &e->hash) < 0) continue;

        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    if (!index) return -1;

    /* Work on a heap-allocated sorted copy so the caller's Index is untouched. */
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, (size_t)sorted->count, sizeof(IndexEntry),
          compare_index_entries);

    /* Write to a temp file then atomically rename over the real index. */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        perror("fopen");
        free(sorted);
        return -1;
    }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        const IndexEntry *e = &sorted->entries[i];
        hash_to_hex(&e->hash, hex);
        if (fprintf(f, "%o %s %llu %llu %s\n",
                    e->mode, hex,
                    (unsigned long long)e->mtime_sec,
                    (unsigned long long)e->size,
                    e->path) < 0) {
            fclose(f);
            free(sorted);
            return -1;
        }
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    return rename(tmp, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    if (!index || !path) return -1;

    /* 1. Read file contents. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size < 0) { fclose(f); return -1; }

    size_t alloc_size = (file_size > 0) ? (size_t)file_size : 1;
    void *contents = malloc(alloc_size);
    if (!contents) { fclose(f); return -1; }

    if (file_size > 0 &&
        fread(contents, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f); free(contents); return -1;
    }
    fclose(f);

    /* 2. Store as a blob object. */
    ObjectID hash;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &hash) < 0) {
        fprintf(stderr, "error: object_write failed for '%s'\n", path);
        free(contents);
        return -1;
    }
    free(contents);

    /* 3. Collect metadata. */
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "error: lstat failed for '%s': %s\n", path, strerror(errno));
        return -1;
    }

    uint32_t mode;
    if (S_ISDIR(st.st_mode))       mode = 0040000;
    else if (st.st_mode & S_IXUSR) mode = 0100755;
    else                           mode = 0100644;

    /* 4. Update existing entry or append a new one. */
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        entry = &index->entries[index->count++];
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    }

    entry->mode      = mode;
    entry->hash      = hash;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size      = (uint64_t)st.st_size;

    /* 5. Persist. */
    return index_save(index);
}
