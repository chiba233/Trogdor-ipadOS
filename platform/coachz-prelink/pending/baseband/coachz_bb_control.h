/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The methods of the AppleBaseband user client (AppleBasebandN71's, which libTelephonyBaseband finds by the service name AppleBaseband)
 * as the bridge answers them. Selectors and scalar counts are read from __TelephonyBasebandControllerCallMethod's call sites in
 * libTelephonyBasebandDynamic (21H461). Portable and host-tested; no kernel code.
 *
 * On Apple's boards these drive GPIOs and PMU lines of a separate modem chip. Here the modem lives in the SoC and is started by our
 * MBA/PIL code before iPadOS runs, so power, reset, mux and VBUS requests are accepted and recorded but do not touch the modem; the
 * read-back lines report a modem out of reset and powered. The manager and baseband states (22/23, 26/27) are values ABM writes and
 * reads back itself (BootController), so they are stored and returned unchanged. Selectors nothing here implements answer unsupported.
 */
#ifndef COACHZ_BB_CONTROL_H
#define COACHZ_BB_CONTROL_H

#include <stdint.h>

#define CBC_OK 0
#define CBC_BAD_ARGUMENT ((int)0xe00002c2)     /* kIOReturnBadArgument */
#define CBC_UNSUPPORTED ((int)0xe00002c7)      /* kIOReturnUnsupported */
#define CBC_PROPERTIES 16u

enum {
    CBC_RESET_MODEM = 0, CBC_POWER = 1, CBC_MUX = 2, CBC_GET_RESET = 3, CBC_POWERCYCLE = 6, CBC_VBUS = 8, CBC_GET_PMU_EXTON = 11, CBC_SELF_IDENTIFY = 15, CBC_GET_DIAG_READY = 16,
    CBC_GET_EFS_ERASE_DONE = 17, CBC_GET_RESET_REQUEST = 18, CBC_COREDUMP_GPIO = 19, CBC_TIMESYNC = 20, CBC_SET_MANAGER_STATE = 22,
    CBC_GET_MANAGER_STATE = 23, CBC_CHARGER_DESENSE = 25, CBC_GET_BASEBAND_STATE = 26, CBC_SET_BASEBAND_STATE = 27,
    CBC_GET_PROPERTY = 28, CBC_SET_PROPERTY = 29, CBC_CLEAR_FREQ_LIST = 32, CBC_REMOVE_FREQ_LIST = 34, CBC_RESET_SPMI = 35,
    CBC_SPMI_RAW_WRITE = 36, CBC_SPMI_RAW_READ = 37, CBC_SPMI_GET_EVENTS = 41, CBC_GET_COREDUMP_READY = 42, CBC_SPMI_REG0_WRITE = 45,
};

struct cbc_state {
    uint64_t manager_state, baseband_state;
    uint64_t property_key[CBC_PROPERTIES], property_value[CBC_PROPERTIES];
    uint32_t properties;
    uint8_t powered, mux, vbus, coredump_gpio, desense;
    uint32_t resets, calls, unsupported, last_unsupported;
};

static inline void cbc_init(struct cbc_state *s) { __builtin_memset(s, 0, sizeof *s); s->powered = 1; }

/* One call: `in`/`nin` scalars in, up to `*nout` scalars out (set to the count written). */
static inline int cbc_call(struct cbc_state *s, uint32_t selector, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t *nout)
{
    const uint32_t room = *nout;
    *nout = 0;
    s->calls++;
#define CBC_NEED(i, o) do { if (nin < (i) || room < (o)) return CBC_BAD_ARGUMENT; } while (0)
#define CBC_OUT(v) (out[(*nout)++] = (uint64_t)(v))
    switch (selector) {
    case CBC_RESET_MODEM: CBC_NEED(1, 0); s->resets++; return CBC_OK;
    case CBC_POWER: CBC_NEED(1, 0); s->powered = in[0] != 0; return CBC_OK;
    case CBC_POWERCYCLE: s->resets++; s->powered = 1; return CBC_OK;
    case CBC_SELF_IDENTIFY: return CBC_OK;                                      /* each client names itself in a structure; nothing here keys on it */
    case CBC_MUX: CBC_NEED(1, 0); s->mux = in[0] != 0; return CBC_OK;
    case CBC_VBUS: CBC_NEED(1, 0); s->vbus = in[0] != 0; return CBC_OK;
    case CBC_COREDUMP_GPIO: CBC_NEED(1, 0); s->coredump_gpio = in[0] != 0; return CBC_OK;
    case CBC_CHARGER_DESENSE: CBC_NEED(1, 0); s->desense = in[0] != 0; return CBC_OK;
    case CBC_CLEAR_FREQ_LIST: CBC_NEED(1, 0); return CBC_OK;
    case CBC_GET_RESET: CBC_NEED(0, 1); CBC_OUT(0); return CBC_OK;             /* not held in reset */
    case CBC_GET_PMU_EXTON: CBC_NEED(0, 1); CBC_OUT(s->powered); return CBC_OK;
    case CBC_GET_DIAG_READY: CBC_NEED(0, 1); CBC_OUT(0); return CBC_OK;
    case CBC_GET_EFS_ERASE_DONE: CBC_NEED(0, 1); CBC_OUT(0); return CBC_OK;
    case CBC_GET_RESET_REQUEST: CBC_NEED(0, 1); CBC_OUT(0); return CBC_OK;
    case CBC_GET_COREDUMP_READY: CBC_NEED(0, 1); CBC_OUT(0); return CBC_OK;
    case CBC_SET_MANAGER_STATE: CBC_NEED(1, 0); s->manager_state = in[0]; return CBC_OK;
    case CBC_GET_MANAGER_STATE: CBC_NEED(0, 1); CBC_OUT(s->manager_state); return CBC_OK;
    case CBC_SET_BASEBAND_STATE: CBC_NEED(1, 0); s->baseband_state = in[0]; return CBC_OK;
    case CBC_GET_BASEBAND_STATE: CBC_NEED(0, 1); CBC_OUT(s->baseband_state); return CBC_OK;
    case CBC_SET_PROPERTY: {
        CBC_NEED(2, 0);
        for (uint32_t i = 0; i < s->properties; i++)
            if (s->property_key[i] == in[0]) { s->property_value[i] = in[1]; return CBC_OK; }
        if (s->properties == CBC_PROPERTIES) return CBC_BAD_ARGUMENT;
        s->property_key[s->properties] = in[0];
        s->property_value[s->properties++] = in[1];
        return CBC_OK;
    }
    case CBC_GET_PROPERTY:
        CBC_NEED(1, 1);
        for (uint32_t i = 0; i < s->properties; i++)
            if (s->property_key[i] == in[0]) { CBC_OUT(s->property_value[i]); return CBC_OK; }
        CBC_OUT(0);
        return CBC_OK;
    default:   /* time sync, SPMI, GNSS, frequency lists with data: there is no such line to this modem */
        s->unsupported++;
        s->last_unsupported = selector;
        return CBC_UNSUPPORTED;
    }
#undef CBC_NEED
#undef CBC_OUT
}

#endif /* COACHZ_BB_CONTROL_H */
