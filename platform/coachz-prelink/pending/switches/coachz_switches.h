/* SPDX-License-Identifier: BSD-2-Clause */
/* Current-state queries only. See SOURCES.md for the EC-side handlers. */
#ifndef COACHZ_SWITCHES_H
#define COACHZ_SWITCHES_H
#include <stdint.h>

#define CS_COMMAND 0x0061u
#define CS_REPLY 64u
#define CS_LID_OPEN (1u << 0)
#define CS_TABLET_MODE (1u << 1)
#define CS_BASE_ATTACHED (1u << 2)
#define CS_FRONT_PROXIMITY (1u << 3)
#define CS_KNOWN 0x0fu
enum cs_status { CS_READY, CS_ERROR, CS_VERSION_UNSUPPORTED, CS_NO_KNOWN_SWITCHES };
enum { CS_SHORT = -1, CS_VERSION = -2 };
enum cs_notice { CS_QUIET, CS_BASELINE, CS_CHANGED, CS_UNAVAILABLE };

struct cs_bus {
    void *context;
    int (*command)(void *, uint16_t, uint8_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
};
struct cs_sample {
    enum cs_status status;
    int error;
    uint32_t versions, supported, raw, known, changed;
    int baseline;
};
struct cs_tracker {
    uint32_t supported, previous;
    uint64_t transitions;
    enum cs_status last_status;
    int have_previous, seen;
};

static inline uint32_t cs_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int
cs_query(const struct cs_bus *bus, uint8_t info_type, uint32_t *value)
{
    const uint8_t args[2] = {info_type, 4}; /* SWITCH event type, never BUTTON or GET_NEXT_EVENT. */
    uint8_t reply[CS_REPLY];
    uint32_t length = 0;
    int error = bus->command(bus->context, CS_COMMAND, 1, args, sizeof(args), reply, sizeof(reply), &length);
    if (error) return error;
    if (length != 4) return CS_SHORT;
    *value = cs_le32(reply);
    return 0;
}

static inline void
cs_collect(const struct cs_bus *bus, struct cs_sample *sample)
{
    sample->status = CS_ERROR; sample->error = 0;
    sample->versions = sample->supported = sample->raw = sample->known = sample->changed = 0;
    sample->baseline = 0;
    uint8_t command = CS_COMMAND, reply[CS_REPLY];
    uint32_t length = 0;
    int error = bus->command(bus->context, 0x0008, 0, &command, 1, reply, sizeof(reply), &length);
    if (!error && length != 4) error = CS_SHORT;
    if (error) { sample->error = error; return; }
    sample->versions = cs_le32(reply);
    if (!(sample->versions & (1u << 1))) {
        sample->status = CS_VERSION_UNSUPPORTED; sample->error = CS_VERSION; return;
    }
    error = cs_query(bus, 1, &sample->supported); /* SUPPORTED */
    if (error) { sample->error = error; return; }
    if (!(sample->supported & CS_KNOWN)) { sample->status = CS_NO_KNOWN_SWITCHES; return; }
    error = cs_query(bus, 2, &sample->raw); /* CURRENT, no event/FIFO dequeue. */
    if (error) { sample->error = error; return; }
    sample->known = sample->raw & sample->supported & CS_KNOWN;
    sample->status = CS_READY;
}

/* Missing data is not a closed lid or detached keyboard. A new capability
 * mask or an error gap starts a fresh baseline instead of inventing an edge.
 */
static inline enum cs_notice
cs_track(struct cs_tracker *tracker, struct cs_sample *sample)
{
    enum cs_notice notice = CS_QUIET;
    sample->changed = 0; sample->baseline = 0;
    if (sample->status != CS_READY) {
        if (!tracker->seen || sample->status != tracker->last_status) notice = CS_UNAVAILABLE;
        tracker->have_previous = 0;
    } else {
        if (!tracker->have_previous || sample->supported != tracker->supported) {
            sample->baseline = 1;
            notice = CS_BASELINE;
        } else {
            sample->changed = (sample->known ^ tracker->previous) & sample->supported & CS_KNOWN;
            if (sample->changed) { ++tracker->transitions; notice = CS_CHANGED; }
        }
        tracker->supported = sample->supported;
        tracker->previous = sample->known;
        tracker->have_previous = 1;
    }
    tracker->last_status = sample->status;
    tracker->seen = 1;
    return notice;
}

static inline const char *cs_status_name(enum cs_status status) {
    switch (status) {
    case CS_READY: return "valid";
    case CS_ERROR: return "query-error";
    case CS_VERSION_UNSUPPORTED: return "unsupported-command-version";
    case CS_NO_KNOWN_SWITCHES: return "no-known-switches";
    }
    return "unknown";
}
#endif
