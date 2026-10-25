/* SPDX-License-Identifier: BSD-2-Clause */
#include "fdt.h"

uint32_t fdt_be32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
           (uint32_t)b[2] << 8 | b[3];
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t bounded_len(const uint8_t *p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) n++;
    return n;
}
static const void *scan(const void *blob, size_t cap, const char *want_path,
                        const char *want_name, uint32_t *result_size, int *valid) {
    const uint8_t *b = blob, *found = NULL;
    uint32_t found_size = 0;
    *valid = 0;
    if (!b || cap < 40 || fdt_be32(b) != 0xd00dfeed) return NULL;
    uint32_t total = fdt_be32(b + 4), off = fdt_be32(b + 8);
    uint32_t strings = fdt_be32(b + 12), reserve = fdt_be32(b + 16);
    uint32_t string_size = fdt_be32(b + 32), struct_size = fdt_be32(b + 36);
    if (total < 40 || total > cap || total > FDT_MAX_SIZE ||
        fdt_be32(b + 20) != 17 || fdt_be32(b + 24) > 17 ||
        off < 40 || (off & 3) || off > total || struct_size > total - off ||
        strings < 40 || strings > total || string_size > total - strings ||
        reserve < 40 || (reserve & 7) || reserve > total) return NULL;
    /* Validate the reservation terminator without dereferencing its addresses. */
    uint32_t r = reserve;
    for (;;) {
        if (r > total || total - r < 16) return NULL;
        int nonzero = 0;
        for (unsigned i = 0; i < 16; i++) nonzero |= b[r + i];
        r += 16;
        if (!nonzero) break;
    }
    uint32_t end = off + struct_size;
    /* Pinned Depthcharge dt_flatten excludes its trailing FDT_END from
       size_dt_struct. Accept exactly that layout, within the full blob. */
    if (end <= total && total - end >= 4 && strings == end + 4 &&
        fdt_be32(b + end) == 9) end += 4;
    if (!(end <= strings || strings + string_size <= off) ||
        !(r <= off || end <= reserve) ||
        !(r <= strings || strings + string_size <= reserve)) return NULL;
    char path[256];
    size_t lengths[32], length = 0;
    unsigned depth = 0, roots = 0;
    path[0] = 0;
    while (off <= end && end - off >= 4) {
        uint32_t token = fdt_be32(b + off);
        off += 4;
        if (token == 1) {
            if (depth == 32) return NULL;
            size_t n = bounded_len(b + off, end - off);
            if (n == end - off || n > 250) return NULL;
            lengths[depth] = length;
            if (!depth) {
                if (roots++ || n) return NULL;
                path[length++] = '/';
            } else {
                if (!n || length + n + 2 > sizeof(path)) return NULL;
                if (length > 1) path[length++] = '/';
                for (size_t i = 0; i < n; i++) path[length++] = b[off + i];
            }
            path[length] = 0;
            depth++;
            off += (uint32_t)((n + 1 + 3) & ~(size_t)3);
            if (off > end) return NULL;
        } else if (token == 2) {
            if (!depth) return NULL;
            length = lengths[--depth];
            path[length] = 0;
        } else if (token == 3) {
            if (!depth || end - off < 8) return NULL;
            uint32_t n = fdt_be32(b + off), name = fdt_be32(b + off + 4);
            off += 8;
            if (name >= string_size || n > end - off ||
                bounded_len(b + strings + name, string_size - name) == string_size - name)
                return NULL;
            if (want_path && equal(path, want_path) &&
                equal((const char *)b + strings + name, want_name)) {
                if (found) return NULL; /* ambiguous duplicate */
                found = b + off;
                found_size = n;
            }
            if (n > UINT32_MAX - 3) return NULL;
            off += (n + 3) & ~3u;
            if (off > end) return NULL;
        } else if (token == 4) {
            continue;
        } else if (token == 9) {
            if (depth || roots != 1 || off != end) return NULL;
            *valid = 1;
            if (result_size) *result_size = found_size;
            return found;
        } else return NULL;
    }
    return NULL;
}
int fdt_valid(const void *blob, size_t capacity) {
    int valid;
    scan(blob, capacity, NULL, NULL, NULL, &valid);
    return valid;
}
const void *fdt_property(const void *blob, size_t capacity,
                         const char *path, const char *name, uint32_t *size) {
    int valid;
    if (!size || !path || !name) return NULL;
    *size = 0;
    const void *p = scan(blob, capacity, path, name, size, &valid);
    return valid ? p : NULL;
}
