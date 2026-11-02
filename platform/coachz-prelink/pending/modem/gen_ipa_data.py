#!/usr/bin/env python3
"""Regenerate coachz_ipa_v42.h from a Linux tree at the pinned commit.

usage: python3 -I gen_ipa_data.py LINUX_TREE STUB_DIR OUTPUT_HEADER

Compiles drivers/net/ipa/reg/ipa_reg-v4.2.c, reg/gsi_reg-v4.0.c and data/ipa_data-v4.2.c on the host (STUB_DIR holds
minimal linux/*.h stand-ins) and links them with a generated program that indexes the real tables by the real enum
values of ipa_reg.h and gsi_reg.h. Register and field names are parsed from the sources; every number comes from the
compiled tables, so nothing is transcribed by hand.
"""
import re, subprocess, sys, tempfile
from pathlib import Path

COMMIT = '6628e7d4c6b25c8d18f1345e3a20d21829aae7e4'
tree, stubs, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
ipa = tree / 'drivers/net/ipa'
REG_MACRO = re.compile(r'^\s*REG(_STRIDE)?(_FIELDS)?\((\w+),\s*(\w+),\s*([^,)]+)(?:,\s*([^)]+))?\);', re.M)


def registers(path):
    text = path.read_text()
    fields = {}
    for m in re.finditer(r'static const u32 reg_(\w+)_fmask\[\] = \{(.*?)\n\};', text, re.S):
        fields[m.group(1)] = re.findall(r'\[(\w+)\]\s*=', m.group(2))
    regs = []
    for m in REG_MACRO.finditer(text):
        name, lower, has_fields = m.group(3), m.group(4), bool(m.group(2))
        regs.append((name, fields.get(lower, []) if has_fields else []))
    return regs


ipa_regs, gsi_regs = registers(ipa / 'reg/ipa_reg-v4.2.c'), registers(ipa / 'reg/gsi_reg-v4.0.c')
prog = ['#include <stdio.h>', '#include "ipa_reg.h"', '#include "gsi_reg.h"', '#include "ipa_data.h"', '#include "ipa_mem.h"',
        'static void reg_out(const char *p, const char *n, const struct regs *rs, unsigned id, unsigned nf, const unsigned *fi, const char *const *fn) {',
        '  const struct reg *r = rs->reg[id];',
        '  if (!r) { printf("/* %s_%s: not on this version */\\n", p, n); return; }',
        '  printf("#define %s_%s 0x%05x\\n", p, n, r->offset);',
        '  if (r->stride) printf("#define %s_%s_STRIDE 0x%x\\n", p, n, r->stride);',
        '  for (unsigned i = 0; i < nf; i++) if (fi[i] < r->fcount && r->fmask[fi[i]]) printf("#define %s_%s_%s 0x%08x\\n", p, n, fn[i], r->fmask[fi[i]]);',
        '}', 'int main(void) {']
for prefix, regs, table in (('CIPA', ipa_regs, 'ipa_regs_v4_2'), ('CGSI', gsi_regs, 'gsi_regs_v4_0')):
    for name, fields in regs:
        ids = ', '.join(fields) or '0'
        names = ', '.join('"%s"' % f for f in fields) or '""'
        prog.append('  { static const unsigned fi[] = { %s }; static const char *const fn[] = { %s };'
                    ' reg_out("%s", "%s", &%s, %s, %d, fi, fn); }' % (ids, names, prefix, name, table, name, len(fields)))
prog += [
    '  const struct ipa_data *d = &ipa_data_v4_2;',
    '  printf("\\n#define CIPA_VERSION %u\\n#define CIPA_BACKWARD_COMPAT 0x%x\\n#define CIPA_MODEM_ROUTE_COUNT %u\\n", d->version, d->backward_compat, d->modem_route_count);',
    '  printf("#define CIPA_CORE_CLOCK_RATE %u\\n#define CIPA_SMEM_SIZE 0x%x\\n", d->power_data->core_clock_rate, d->mem_data->smem_size);',
    '  printf("\\nstatic const struct cipa_qsb cipa_qsb[] = {\\n");',
    '  for (unsigned i = 0; i < d->qsb_count; i++) printf("    { %u, %u, %u },\\n", d->qsb_data[i].max_writes, d->qsb_data[i].max_reads, d->qsb_data[i].max_reads_beats);',
    '  printf("};\\n\\n/* ee, channel, endpoint, toward_ipa, tre, event, tlv, filter, resource group, checksum, qmap, aggregation,\\n'
    '   status, dma mode, dma endpoint, tx seq type, tx seq rep type, tx status endpoint, rx buffer size, pad align,\\n'
    '   aggr time limit, aggr hard limit, aggr close eof, holb drop; indexed by enum ipa_endpoint_name */\\n");',
    '  printf("static const struct cipa_endpoint cipa_endpoints[%u] = {\\n", d->endpoint_count);',
    '  for (unsigned i = 0; i < d->endpoint_count; i++) { const struct ipa_gsi_endpoint_data *e = &d->endpoint_data[i];',
    '    const struct ipa_endpoint_config *c = &e->endpoint.config;',
    '    if (!e->channel.tre_count && !e->endpoint_id && !e->channel_id) { printf("    { 0 },\\n"); continue; }',
    '    printf("    { %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, ", e->ee_id, e->channel_id, e->endpoint_id, e->toward_ipa,',
    '      e->channel.tre_count, e->channel.event_count, e->channel.tlv_count, e->endpoint.filter_support, c->resource_group, c->checksum,',
    '      c->qmap, c->aggregation, c->status_enable, c->dma_mode, c->dma_endpoint);',
    '    if (e->toward_ipa) printf("%u, %u, %u, 0, 0, 0, 0, 0, 0 },\\n", c->tx.seq_type, c->tx.seq_rep_type, c->tx.status_endpoint);',
    '    else printf("0, 0, 0, %u, %u, %u, %u, %u, %u },\\n", c->rx.buffer_size, c->rx.pad_align, c->rx.aggr_time_limit, c->rx.aggr_hard_limit,',
    '      c->rx.aggr_close_eof, c->rx.holb_drop); }',
    '  printf("};\\n\\n#define CIPA_RSRC_GROUP_SRC_COUNT %u\\n#define CIPA_RSRC_GROUP_DST_COUNT %u\\n", d->resource_data->rsrc_group_src_count, d->resource_data->rsrc_group_dst_count);',
    '  printf("/* per resource type: { min, max } for each group */\\nstatic const uint32_t cipa_resource_src[][%d][2] = {\\n", IPA_RESOURCE_GROUP_MAX);',
    '  for (unsigned i = 0; i < d->resource_data->resource_src_count; i++) { printf("    {"); for (unsigned g = 0; g < IPA_RESOURCE_GROUP_MAX; g++)',
    '    printf(" { %u, %u },", d->resource_data->resource_src[i].limits[g].min, d->resource_data->resource_src[i].limits[g].max); printf(" },\\n"); }',
    '  printf("};\\nstatic const uint32_t cipa_resource_dst[][%d][2] = {\\n", IPA_RESOURCE_GROUP_MAX);',
    '  for (unsigned i = 0; i < d->resource_data->resource_dst_count; i++) { printf("    {"); for (unsigned g = 0; g < IPA_RESOURCE_GROUP_MAX; g++)',
    '    printf(" { %u, %u },", d->resource_data->resource_dst[i].limits[g].min, d->resource_data->resource_dst[i].limits[g].max); printf(" },\\n"); }',
    '  printf("};\\n");',
    '  printf("\\n/* enum ipa_endpoint_name */\\n#define CIPA_EP_AP_COMMAND_TX %u\\n#define CIPA_EP_AP_LAN_RX %u\\n#define CIPA_EP_AP_MODEM_TX %u\\n#define CIPA_EP_AP_MODEM_RX %u\\n#define CIPA_EP_COUNT %u\\n",',
    '    IPA_ENDPOINT_AP_COMMAND_TX, IPA_ENDPOINT_AP_LAN_RX, IPA_ENDPOINT_AP_MODEM_TX, IPA_ENDPOINT_AP_MODEM_RX, IPA_ENDPOINT_COUNT);',
    '  return 0; }']
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / 'dump.c').write_text('\n'.join(prog) + '\n')
    sources = [d / 'dump.c', ipa / 'reg/ipa_reg-v4.2.c', ipa / 'reg/gsi_reg-v4.0.c', ipa / 'data/ipa_data-v4.2.c']
    subprocess.run(['cc', '-std=gnu11', '-w', '-I', str(stubs), '-I', str(ipa), '-I', str(ipa / 'reg'), *map(str, sources), '-o', str(d / 'dump')], check=True)
    body = subprocess.run([str(d / 'dump')], check=True, capture_output=True, text=True).stdout
out.write_text('/* SPDX-License-Identifier: BSD-2-Clause */\n'
               '/* Generated by gen_ipa_data.py from Linux %s drivers/net/ipa/reg/ipa_reg-v4.2.c, reg/gsi_reg-v4.0.c\n'
               '   and data/ipa_data-v4.2.c (the sc7180 IPA). Do not edit; regenerate. Offsets are from the "ipa-reg" and "gsi"\n'
               '   bases; *_STRIDE is the per-instance step of parameterized registers; field values are masks. */\n'
               '#ifndef COACHZ_IPA_V42_H\n#define COACHZ_IPA_V42_H\n#include <stdint.h>\n\n'
               'struct cipa_qsb { uint8_t max_writes, max_reads, max_reads_beats; };\n'
               'struct cipa_endpoint { uint8_t ee, channel, endpoint, toward_ipa; uint16_t tre_count, event_count; uint8_t tlv_count, filter,\n'
               '    resource_group, checksum, qmap, aggregation, status_enable, dma_mode, dma_endpoint, tx_seq_type, tx_seq_rep_type,\n'
               '    tx_status_endpoint; uint32_t rx_buffer_size, rx_pad_align, rx_aggr_time_limit; uint8_t rx_aggr_hard_limit,\n'
               '    rx_aggr_close_eof, rx_holb_drop; };\n\n%s\n#endif\n' % (COMMIT, body))
print(out, body.count('#define'), 'defines')
