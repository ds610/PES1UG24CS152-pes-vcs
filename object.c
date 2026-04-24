// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ─────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ───────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob"; break;
        case OBJ_TREE:   type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 1. Create header
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    size_t full_len = header_len + 1 + len;

    // 2. Build full object
    uint8_t *full = malloc(full_len);
    if (!full) return -1;

    memcpy(full, header, header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);

    // 3. Hash
    compute_hash(full, full_len, id_out);

    // 4. Dedup
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // 5. Prepare directories
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);

    // 🔥 FIX: ensure all dirs exist
    mkdir(".pes", 0755);
    mkdir(OBJECTS_DIR, 0755);
    mkdir(shard_dir, 0755);

    // 6. Paths
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/.tmp_XXXXXX", OBJECTS_DIR, hex);

    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        perror("mkstemp failed");
        free(full);
        return -1;
    }

    // 7. Write
    ssize_t written = write(fd, full, full_len);
    free(full);

    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // 8. Flush
    if (fsync(fd) < 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    // 9. Rename
    if (rename(tmp_path, final_path) < 0) {
        unlink(tmp_path);
        return -1;
    }

    // 10. Sync dir
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// ─────────────────────────────────────────────────────────

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    uint8_t *raw = malloc(file_size);
    if (!raw) {
        fclose(f);
        return -1;
    }

    if (fread(raw, 1, file_size, f) != (size_t)file_size) {
        fclose(f);
        free(raw);
        return -1;
    }
    fclose(f);

    // Verify hash
    ObjectID computed;
    compute_hash(raw, file_size, &computed);

    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(raw);
        return -1;
    }

    // Find separator
    uint8_t *null_ptr = memchr(raw, '\0', file_size);
    if (!null_ptr) {
        free(raw);
        return -1;
    }

    // Detect type
    if (strncmp((char *)raw, "blob ", 5) == 0) {
        *type_out = OBJ_BLOB;
    } else if (strncmp((char *)raw, "tree ", 5) == 0) {
        *type_out = OBJ_TREE;
    } else if (strncmp((char *)raw, "commit ", 7) == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        free(raw);
        return -1;
    }

    size_t data_len = file_size - (null_ptr - raw) - 1;
    uint8_t *data_start = null_ptr + 1;

    void *out = malloc(data_len);
    if (!out) {
        free(raw);
        return -1;
    }

    memcpy(out, data_start, data_len);

    *data_out = out;
    *len_out = data_len;

    free(raw);
    return 0;
}
