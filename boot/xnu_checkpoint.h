/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef XNU_CHECKPOINT_H
#define XNU_CHECKPOINT_H
#include <stddef.h>
#include <stdint.h>
#define XNU_ARENA_BYTES (24u * 1024u * 1024u)
struct xnu_handoff { uint64_t entry, args; };
struct xnu_expected { uint64_t pc, args, virt, phys, memory, pages, flags;
    uint64_t mode, tcr, mair, vbar, sp0, sp1, end;
    uint64_t c_entry, const_args, boot_args, slide, static_cache, guard;
    uint64_t cpu_data, rtclock;
    uint64_t pe_state, protection, dt_root, dt_cursor, pe_init, identify, soc, node, cursor, tree, tree_size;
    uint64_t soc_data, pe_info, config_bits, debug_enabled, degradation, adt_checksum, soc_ranges, soc_base, diagnostics, kernel_end;
    uint64_t topology,topo_info,topo_cpus,topo_clusters,topo_offsets,topo_correct,topo_eventi,
        topo_page,topo_userpage,topo_stall,topo_reg,topo_affinity; };
int xnu_load(const uint8_t *, size_t, void *, size_t, uint64_t, uint64_t,
             struct xnu_handoff *, struct xnu_expected *);
int xnu_capture_valid(const uint64_t *, const struct xnu_expected *);
uint64_t xnu_soc_ranges_offset(const uint8_t *,size_t,uint64_t);
int xnu_pe_contract(const uint8_t *,size_t,uint64_t [8]);
uint64_t xnu_pe_checksum(const volatile uint8_t *,size_t);
int xnu_pe_return_valid(const uint64_t *,const uint64_t *,const struct xnu_expected *);
uint64_t xnu_diagnostic_code_mask(uint64_t,uint64_t);
int xnu_diagnostic_return_valid(const uint64_t *,const uint64_t *,const struct xnu_expected *,const uint64_t *);
int xnu_pe_valid(const struct xnu_expected *);
int xnu_platform_valid(const struct xnu_expected *);
int xnu_cpu_valid(const struct xnu_expected *);
int xnu_cinit_valid(const struct xnu_expected *);
int xnu_tables_valid(const uint64_t *, const struct xnu_expected *);
int xnu_range_allowed(const void *, uint64_t, uint64_t);
uint64_t xnu_topology_reg_offset(const uint8_t *,size_t);
uint8_t xnu_topology_initial_byte(unsigned,unsigned,const uint64_t *);
int xnu_topology_valid(const struct xnu_expected *);
int xnu_wfe_valid(const uint64_t *,const uint64_t *);
int xnu_system_restored(void);
struct xnu_handoff xnu_prepare(void);
int xnu_checkpoint_complete(void);
extern uint64_t xnu_capture[27];
#endif
