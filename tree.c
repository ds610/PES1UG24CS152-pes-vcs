// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];

        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;

        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: given a slice of index entries all sharing the same
// directory prefix at `depth` path components, build a tree object and
// write it to the store. Returns 0 on success, -1 on error.
//
// `entries`  – array of IndexEntry pointers for this subtree
// `count`    – number of entries in the slice
// `depth`    – how many '/' path components have already been consumed
// `id_out`   – receives the ObjectID of the written tree object
static int write_tree_level(IndexEntry **entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Find the path component at this depth
        const char *path = entries[i]->path;

        // Skip past `depth` slashes to get the relevant portion
        const char *rel = path;
        for (int d = 0; d < depth; d++) {
            rel = strchr(rel, '/');
            if (!rel) return -1;
            rel++; // skip the '/'
        }

        // Is this a file directly in this directory, or a subdirectory?
        const char *slash = strchr(rel, '/');

        if (!slash) {
            // Direct file entry — add it straight to the tree
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i]->mode;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            memcpy(te->hash.hash, entries[i]->hash.hash, HASH_SIZE);
            i++;
        } else {
            // Subdirectory — collect all entries that share this dir name
            size_t dir_name_len = slash - rel;
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Find how many consecutive entries belong to this subdir
            int j = i;
            while (j < count) {
                const char *r2 = entries[j]->path;
                for (int d = 0; d < depth; d++) {
                    r2 = strchr(r2, '/');
                    if (!r2) break;
                    r2++;
                }
                if (!r2) break;
                const char *s2 = strchr(r2, '/');
                if (!s2) break; // file, not in this subdir
                size_t len2 = s2 - r2;
                if (len2 != dir_name_len || strncmp(r2, dir_name, dir_name_len) != 0) break;
                j++;
            }

            // Recurse to build the subtree
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, depth + 1, &sub_id) < 0)
                return -1;

            // Add a directory entry pointing to the subtree object
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            memcpy(te->hash.hash, sub_id.hash, HASH_SIZE);

            i = j;
        }
    }

    // Serialize and write the tree object
    void *data;
    size_t data_len;
    if (tree_serialize(&tree, &data, &data_len) < 0) return -1;

    int rc = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    // Load the index
    Index idx;
    if (index_load(&idx) < 0) return -1;

    if (idx.count == 0) {
        // Empty tree — serialize and write an empty tree object
        Tree empty;
        empty.count = 0;
        void *data;
        size_t data_len;
        if (tree_serialize(&empty, &data, &data_len) < 0) return -1;
        int rc = object_write(OBJ_TREE, data, data_len, id_out);
        free(data);
        return rc;
    }

    // Build an array of pointers for the recursive helper
    IndexEntry **entries = malloc(sizeof(IndexEntry *) * idx.count);
    if (!entries) return -1;
    for (int i = 0; i < idx.count; i++)
        entries[i] = &idx.entries[i];

    int rc = write_tree_level(entries, idx.count, 0, id_out);
    free(entries);
    return rc;
}
