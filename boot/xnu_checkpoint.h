/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef XNU_CHECKPOINT_H
#define XNU_CHECKPOINT_H
#include <stddef.h>
#include <stdint.h>
#define XNU_ARENA_BYTES (24u * 1024u * 1024u)
struct xnu_handoff { uint64_t entry, args; };
struct xnu_expected { uint64_t pc, args, virt, phys, memory, pages, flags;
    uint64_t mode, tcr, mair, vbar, sp0, sp1, end; };
int xnu_load(const uint8_t *, size_t, void *, size_t, uint64_t, uint64_t,
             struct xnu_handoff *, struct xnu_expected *);
int xnu_capture_valid(const uint64_t *, const struct xnu_expected *);
int xnu_tables_valid(const uint64_t *, const struct xnu_expected *);
int xnu_range_allowed(const void *, uint64_t, uint64_t);
struct xnu_handoff xnu_prepare(void);
int xnu_checkpoint_complete(void);
extern uint64_t xnu_capture[24];
#endif
