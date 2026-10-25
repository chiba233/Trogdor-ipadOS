/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PROBE_FDT_H
#define PROBE_FDT_H
#include <stddef.h>
#include <stdint.h>
#define FDT_MAX_SIZE (2u * 1024u * 1024u)
uint32_t fdt_be32(const void *p);
int fdt_valid(const void *blob, size_t capacity);
const void *fdt_property(const void *blob, size_t capacity,
                         const char *path, const char *name, uint32_t *size);
#endif
