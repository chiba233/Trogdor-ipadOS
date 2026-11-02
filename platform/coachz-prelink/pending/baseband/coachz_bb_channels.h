/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The channels and user-client methods of Apple's PCIe baseband interface as iPadOS 21H461 uses them (libIOABP, libPCITransport).
 * ABM finds a channel with IOServiceMatching("AppleBasebandPCIInterface") plus ABPInterfaceProtocol = <name> (an ASCII CFString),
 * opens it with IOServiceOpen type 0 and calls method 0 with one scalar 0. Portable; no kernel code.
 */
#ifndef COACHZ_BB_CHANNELS_H
#define COACHZ_BB_CHANNELS_H

#include <stdint.h>

#define CBH_INTERFACE_CLASS "AppleBasebandPCIInterface"
#define CBH_CONTROL_CLASS "AppleBasebandPCIControl"
#define CBH_BASEBAND_CLASS "AppleBaseband"
#define CBH_PROTOCOL_KEY "ABPInterfaceProtocol"

/* libPCITransport's {id, name} table */
struct cbh_channel { uint32_t id; const char *name; };
static const struct cbh_channel cbh_channels[] = {
    { 0, "pbl" }, { 1, "sahara" }, { 2, "diag" }, { 3, "refs" }, { 4, "qmux1" }, { 5, "qmux2" }, { 6, "rom" }, { 7, "boot" },
    { 8, "ari" }, { 9, "trace" }, { 10, "at" }, { 11, "dpl" }, { 12, "mediactl" }, { 13, "vcom" }, { 14, "gnss" }, { 15, "qdss" },
    { 17, "virtual_timesync" }, { 20, "boot_logger" },
};
/* Every channel of that table is published. pbl takes BBUEUR13Loader's register reads and SendImage, sahara the second stage, qmux1 and qmux2 go to the
 * modem. The rest have nothing behind them yet: they open, count what is written and never have anything to read, so the log shows which of them ABM uses. */
enum {
    CBH_PBL, CBH_SAHARA, CBH_DIAG, CBH_REFS, CBH_QMUX1, CBH_QMUX2, CBH_ROM, CBH_BOOT, CBH_ARI, CBH_TRACE, CBH_AT, CBH_DPL, CBH_MEDIACTL, CBH_VCOM, CBH_GNSS,
    CBH_QDSS, CBH_VIRTUAL_TIMESYNC, CBH_BOOT_LOGGER, CBH_PUBLISHED
};
static const char *const cbh_published[CBH_PUBLISHED] = {
    "pbl", "sahara", "diag", "refs", "qmux1", "qmux2", "rom", "boot", "ari", "trace", "at", "dpl", "mediactl", "vcom", "gnss", "qdss", "virtual_timesync", "boot_logger"
};

/* AppleBasebandPCIInterface user client: selector, scalar inputs, async completion arguments (-1: synchronous only) */
struct cbh_method { uint32_t selector, scalars; int async_args; const char *name; };
static const struct cbh_method cbh_interface_methods[] = {
    { 0, 1, -1, "open" },
    { 1, 0, -1, "close" },
    { 2, 2, 1, "write" },             /* user address and length; the synchronous form passes the data as a structure */
    { 3, 2, 1, "read" },
    { 4, 2, 2, "sendImage" },
    { 5, 1, -1, "readRegister" },
    { 6, 1, 0, "abortChannel" },      /* direction */
    { 7, 1, 0, "startChannel" },
    { 8, 3, 1, "timeSync" },
    { 9, 3, 0, "mapMemory" },         /* region, user address, length */
    { 10, 1, 0, "unmapMemory" },
    { 11, 1, -2, "registerTimeEvent" },   /* completes with a variable argument list */
    { 12, 1, 0, "deregisterTimeEvent" },
};
/* AppleBasebandPCIControl user client */
static const struct cbh_method cbh_control_methods[] = {
    { 0, 1, -1, "open" },
    { 1, 0, -1, "close" },
    { 2, 3, 1, "readLogs" },
    { 3, 4, -1, "loggerTune" },
};

/* IOKit's async reference as libIOABP fills it: three words, [1] the callback, [2] its refcon */
#define CBH_ASYNC_REFERENCE_WORDS 3u

static inline int cbh_channel_id(const char *name)
{
    for (unsigned i = 0; i < sizeof cbh_channels / sizeof cbh_channels[0]; i++) {
        const char *a = cbh_channels[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return (int)cbh_channels[i].id;
    }
    return -1;
}

#endif /* COACHZ_BB_CHANNELS_H */
