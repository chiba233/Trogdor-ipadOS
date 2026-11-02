/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The device end of SAHARA on the bridge's sahara channel. BBUpdater (BBUEUR10Programmer for Mav16) is the host and is passive: it
 * waits for the device's HELLO, answers it, then sends whatever the device asks for with READ_DATA, and after END_IMAGE_TX sends DONE.
 * The real modem here already runs (started by MBA/PIL), so this end asks for each listed image the way a Qualcomm boot loader does
 * (ELF header, program headers, then every segment with file data), throws the bytes away, and reports the image transferred. The
 * DONE answer says "complete" after the last listed image. Portable and host-tested; no kernel code.
 *
 * Packets (libETLSAH, little-endian 32-bit words): HELLO 1, HELLO_RESP 2, READ_DATA 3, END_IMAGE_TX 4, DONE 5, DONE_RESP 6, RESET 7,
 * RESET_RESP 8, COMMAND_READY 0xb, SWITCH_MODE 0xc, EXECUTE 0xd, EXECUTE_RESP 0xe, EXECUTE_DATA 0xf. Modes: 0 image pending, 1 complete, 2 memory
 * debug, 3 command, 0x100 Apple's Maverick (manifest and nonce). BBUEUR10Loader first answers HELLO with mode 3 and queries the chip in command mode
 * (execute 2 the MSM hardware id, whose word at offset 4 is the chip id, execute 1 the serial number), then switches to mode 0, which brings a new
 * HELLO and image transfer. Which image ids a normal Mav16 boot must ask for is not known yet: BBUpdater logs every request ("request image ID")
 * and refuses ids it has no item for ("item not found in item list").
 */
#ifndef COACHZ_BB_SAHARA_H
#define COACHZ_BB_SAHARA_H

#include <stdint.h>

#define CBS_HELLO 1u
#define CBS_HELLO_RESP 2u
#define CBS_READ_DATA 3u
#define CBS_END_IMAGE_TX 4u
#define CBS_DONE 5u
#define CBS_DONE_RESP 6u
#define CBS_RESET 7u
#define CBS_RESET_RESP 8u
#define CBS_COMMAND_READY 0xbu
#define CBS_SWITCH_MODE 0xcu
#define CBS_EXECUTE 0xdu
#define CBS_EXECUTE_RESP 0xeu
#define CBS_EXECUTE_DATA 0xfu
#define CBS_MODE_IMAGE_PENDING 0u
#define CBS_MODE_COMPLETE 1u
#define CBS_MODE_COMMAND 3u
#define CBS_EXEC_SERIAL 1u
#define CBS_EXEC_HW_ID 2u
#define CBS_CHIP_ID 0x009540e1u          /* com.apple.EmbeddedSoftwareRestore.Baseband.ChipId of the Mav16 Info.plist */
#define CBS_SERIAL 0x0c0ac42bu           /* no real serial number leaves this machine */
#define CBS_VERSION 2u
#define CBS_VERSION_COMPATIBLE 1u
#define CBS_MAX_COMMAND 0x400u
#define CBS_ELF_PROBE 64u               /* covers the ELF32 (52) and ELF64 (64) headers */
#define CBS_MAX_PHDRS 64u
#define CBS_MAX_IMAGES 32u
#define CBS_PACKET_MAX 48u

enum cbs_state {
    CBS_IDLE, CBS_WAIT_HELLO_RESP, CBS_WAIT_ELF, CBS_WAIT_PHDRS, CBS_WAIT_SEGMENT, CBS_WAIT_DONE, CBS_FINISHED, CBS_FAILED, CBS_COMMAND,
};

struct cbs_out { uint32_t count, len[2]; uint8_t data[2][CBS_PACKET_MAX]; };

struct cbs_device {
    enum cbs_state state;
    uint32_t images[CBS_MAX_IMAGES], image_count, image_index;
    uint32_t chunk;                           /* largest READ_DATA length */
    uint32_t host_mode, host_status, last_command, last_execute, executes;
    /* the image being read */
    uint32_t elf64, phoff, phnum, phentsize;
    uint32_t seg_offset[CBS_MAX_PHDRS], seg_size[CBS_MAX_PHDRS], segments, segment;
    uint32_t read_offset, read_remaining;     /* the current segment's next request */
    uint32_t expect, have;                    /* raw bytes the current READ_DATA still owes */
    uint8_t keep[CBS_MAX_PHDRS * 56u];        /* ELF header, then the program headers; segment data is dropped */
    uint64_t bytes;                           /* everything received as image data, for the log */
    uint32_t requests;
};

static inline void cbs_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint32_t cbs_get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t cbs_get64(const uint8_t *p) { return cbs_get32(p) | (uint64_t)cbs_get32(p + 4) << 32; }

static inline uint8_t *cbs_packet(struct cbs_out *o, uint32_t command, uint32_t length)
{
    uint8_t *p = o->data[o->count];
    __builtin_memset(p, 0, CBS_PACKET_MAX);
    cbs_put32(p, command);
    cbs_put32(p + 4, length);
    o->len[o->count++] = length;
    return p;
}

static inline void cbs_hello(struct cbs_out *o, uint32_t mode)
{
    uint8_t *p = cbs_packet(o, CBS_HELLO, 0x30);
    cbs_put32(p + 8, CBS_VERSION);
    cbs_put32(p + 12, CBS_VERSION_COMPATIBLE);
    cbs_put32(p + 16, CBS_MAX_COMMAND);
    cbs_put32(p + 20, mode);
}

/* What an execute answers with; unknown commands answer empty. */
static inline uint32_t cbs_execute_length(uint32_t command)
{
    return command == CBS_EXEC_HW_ID ? 8u : command == CBS_EXEC_SERIAL ? 4u : 0u;
}

static inline void cbs_read(struct cbs_device *d, struct cbs_out *o, uint32_t offset, uint32_t length)
{
    uint8_t *p = cbs_packet(o, CBS_READ_DATA, 0x14);
    cbs_put32(p + 8, d->images[d->image_index]);
    cbs_put32(p + 12, offset);
    cbs_put32(p + 16, length);
    d->expect = length;
    d->have = 0;
    d->requests++;
}

static inline void cbs_init(struct cbs_device *d, const uint32_t *images, uint32_t count, uint32_t chunk)
{
    __builtin_memset(d, 0, sizeof *d);
    if (count > CBS_MAX_IMAGES) count = CBS_MAX_IMAGES;
    for (uint32_t i = 0; i < count; i++) d->images[i] = images[i];
    d->image_count = count;
    d->chunk = chunk ? chunk : 0x10000u;
}

/* The channel was opened: the device speaks first. */
static inline void cbs_start(struct cbs_device *d, struct cbs_out *o)
{
    o->count = 0;
    d->image_index = 0;
    d->state = d->image_count ? CBS_WAIT_HELLO_RESP : CBS_FAILED;
    if (d->image_count) cbs_hello(o, CBS_MODE_IMAGE_PENDING);
}

/* Ask for the next piece of the current segment, or move to the next segment, or end the image. */
static inline void cbs_next_request(struct cbs_device *d, struct cbs_out *o)
{
    while (!d->read_remaining && d->segment < d->segments) {
        d->read_offset = d->seg_offset[d->segment];
        d->read_remaining = d->seg_size[d->segment];
        d->segment++;
    }
    if (d->read_remaining) {
        const uint32_t n = d->read_remaining < d->chunk ? d->read_remaining : d->chunk;
        cbs_read(d, o, d->read_offset, n);
        d->read_offset += n;
        d->read_remaining -= n;
        d->state = CBS_WAIT_SEGMENT;
        return;
    }
    uint8_t *p = cbs_packet(o, CBS_END_IMAGE_TX, 0x10);
    cbs_put32(p + 8, d->images[d->image_index]);
    cbs_put32(p + 12, 0);
    d->state = CBS_WAIT_DONE;
}

/* ELF header: where the program headers are. A non-ELF image ends after the probe. */
static inline void cbs_parse_elf(struct cbs_device *d, struct cbs_out *o)
{
    const uint8_t *h = d->keep;
    d->segments = d->segment = d->read_remaining = 0;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || (h[4] != 1 && h[4] != 2)) { cbs_next_request(d, o); return; }
    d->elf64 = h[4] == 2;
    d->phoff = d->elf64 ? (uint32_t)cbs_get64(h + 0x20) : cbs_get32(h + 0x1c);
    d->phentsize = d->elf64 ? (uint32_t)(h[0x36] | h[0x37] << 8) : (uint32_t)(h[0x2a] | h[0x2b] << 8);
    d->phnum = d->elf64 ? (uint32_t)(h[0x38] | h[0x39] << 8) : (uint32_t)(h[0x2c] | h[0x2d] << 8);
    if (!d->phnum || d->phnum > CBS_MAX_PHDRS || d->phentsize != (d->elf64 ? 56u : 32u)) { cbs_next_request(d, o); return; }
    cbs_read(d, o, d->phoff, d->phnum * d->phentsize);
    d->state = CBS_WAIT_PHDRS;
}

/* Program headers: every one with file data is read, in order (a boot loader also reads the hash segment, a PT_NULL with data). */
static inline void cbs_parse_phdrs(struct cbs_device *d, struct cbs_out *o)
{
    for (uint32_t i = 0; i < d->phnum; i++) {
        const uint8_t *e = d->keep + i * d->phentsize;
        const uint32_t offset = d->elf64 ? (uint32_t)cbs_get64(e + 8) : cbs_get32(e + 4);
        const uint32_t size = d->elf64 ? (uint32_t)cbs_get64(e + 32) : cbs_get32(e + 16);
        if (!size) continue;
        d->seg_offset[d->segments] = offset;
        d->seg_size[d->segments++] = size;
    }
    cbs_next_request(d, o);
}

/* Bytes from the host. Returns how many were used; packets to send go to `o` (at most two). */
static inline uint32_t cbs_receive(struct cbs_device *d, const uint8_t *in, uint32_t n, struct cbs_out *o)
{
    o->count = 0;
    if (d->state == CBS_WAIT_ELF || d->state == CBS_WAIT_PHDRS || d->state == CBS_WAIT_SEGMENT) {
        const uint32_t take = n < d->expect - d->have ? n : d->expect - d->have;
        if (d->state != CBS_WAIT_SEGMENT && d->have + take <= sizeof d->keep) __builtin_memcpy(d->keep + d->have, in, take);
        d->have += take;
        d->bytes += take;
        if (d->have == d->expect) {
            if (d->state == CBS_WAIT_ELF) cbs_parse_elf(d, o);
            else if (d->state == CBS_WAIT_PHDRS) cbs_parse_phdrs(d, o);
            else cbs_next_request(d, o);
        }
        return take;
    }
    if (n < 8) { d->state = CBS_FAILED; return n; }
    const uint32_t command = cbs_get32(in), length = cbs_get32(in + 4);
    d->last_command = command;
    if (length < 8 || length > n) { d->state = CBS_FAILED; return n; }
    if (command == CBS_RESET) {
        cbs_packet(o, CBS_RESET_RESP, 8);
        d->state = CBS_IDLE;
        return length;
    }
    if (d->state == CBS_WAIT_HELLO_RESP && command == CBS_HELLO_RESP && length >= 0x18) {
        d->host_status = cbs_get32(in + 16);
        d->host_mode = cbs_get32(in + 20);
        if (d->host_status) { d->state = CBS_FAILED; return length; }
        if (d->host_mode == CBS_MODE_COMMAND) {
            cbs_packet(o, CBS_COMMAND_READY, 8);
            d->state = CBS_COMMAND;
            return length;
        }
        if (d->host_mode != CBS_MODE_IMAGE_PENDING && d->host_mode != CBS_MODE_COMPLETE) { d->state = CBS_FAILED; return length; }
        cbs_read(d, o, 0, CBS_ELF_PROBE);
        d->state = CBS_WAIT_ELF;
        return length;
    }
    if (d->state == CBS_COMMAND && length >= 0xc) {
        const uint32_t argument = cbs_get32(in + 8);
        if (command == CBS_EXECUTE) {
            uint8_t *p = cbs_packet(o, CBS_EXECUTE_RESP, 0x10);
            cbs_put32(p + 8, argument);
            cbs_put32(p + 12, cbs_execute_length(argument));
            d->last_execute = argument;
            d->executes++;
            return length;
        }
        if (command == CBS_EXECUTE_DATA) {
            /* Raw data, no packet header: the host asked for exactly this many bytes. */
            const uint32_t data = cbs_execute_length(argument);
            if (data) {
                uint8_t *p = o->data[o->count];
                __builtin_memset(p, 0, CBS_PACKET_MAX);
                if (argument == CBS_EXEC_HW_ID) cbs_put32(p + 4, CBS_CHIP_ID);
                else cbs_put32(p, CBS_SERIAL);
                o->len[o->count++] = data;
            }
            return length;
        }
        if (command == CBS_SWITCH_MODE) {
            cbs_hello(o, argument);
            d->state = CBS_WAIT_HELLO_RESP;
            return length;
        }
    }
    if (d->state == CBS_WAIT_DONE && command == CBS_DONE) {
        const int last = d->image_index + 1u >= d->image_count;
        uint8_t *p = cbs_packet(o, CBS_DONE_RESP, 0x0c);
        cbs_put32(p + 8, last ? 1u : 0u);
        if (last) {
            d->state = CBS_FINISHED;
        } else {
            d->image_index++;
            cbs_hello(o, CBS_MODE_IMAGE_PENDING);
            d->state = CBS_WAIT_HELLO_RESP;
        }
        return length;
    }
    d->state = CBS_FAILED;
    return length;
}

#endif /* COACHZ_BB_SAHARA_H */
