/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_switches.h"

struct fixture { uint32_t versions, supported, current, calls, current_reads; int fail, short_reply; };
static int command(void *context, uint16_t cmd, uint8_t version, const void *arguments,
    uint32_t size, void *response, uint32_t room, uint32_t *length) {
    struct fixture *f = context;
    const uint8_t *args = arguments;
    uint8_t *reply = response;
    uint32_t value;
    ++f->calls;
    assert(room == CS_REPLY);
    if (cmd == 0x0008) {
        assert(version == 0 && size == 1 && args[0] == CS_COMMAND);
        value = f->versions;
    } else {
        assert(cmd == CS_COMMAND && version == 1 && size == 2 && args[1] == 4);
        assert(args[0] == 1 || args[0] == 2); // Nothing mutating or dequeuing is allowed.
        value = args[0] == 1 ? f->supported : f->current;
        if (args[0] == 2) {
            ++f->current_reads;
            if (f->fail) return 7 | (3 << 8);
        }
    }
    for (unsigned i = 0; i < 4; ++i) reply[i] = (uint8_t)(value >> (8 * i));
    *length = f->short_reply ? 3 : 4;
    return 0;
}

int main(void) {
    struct fixture f = {0};
    f.versions = 3; f.supported = CS_KNOWN; f.current = CS_LID_OPEN | CS_BASE_ATTACHED;
    const struct cs_bus bus = {&f, command};
    struct cs_tracker tracker = {0};
    struct cs_sample sample;
    cs_collect(&bus, &sample);
    assert(sample.status == CS_READY && sample.supported == CS_KNOWN);
    assert(sample.known == (CS_LID_OPEN | CS_BASE_ATTACHED) && f.calls == 3);
    assert(cs_track(&tracker, &sample) == CS_BASELINE && sample.baseline && !sample.changed);
    assert(!tracker.transitions);
    puts("PASS supported version and initial current-state baseline without a fake edge");

    cs_collect(&bus, &sample);
    assert(cs_track(&tracker, &sample) == CS_QUIET);
    f.current = CS_LID_OPEN | CS_TABLET_MODE; // Detach keyboard, enter tablet mode.
    cs_collect(&bus, &sample);
    assert(cs_track(&tracker, &sample) == CS_CHANGED);
    assert(sample.changed == (CS_BASE_ATTACHED | CS_TABLET_MODE) && tracker.transitions == 1);
    puts("PASS real state change and unchanged samples have distinct notifications");

    f.fail = 1;
    cs_collect(&bus, &sample);
    assert(sample.status == CS_ERROR && sample.error == (7 | (3 << 8)));
    assert(!sample.raw && !sample.known);
    assert(cs_track(&tracker, &sample) == CS_UNAVAILABLE && !tracker.have_previous);
    f.fail = 0; f.current = CS_LID_OPEN | CS_BASE_ATTACHED;
    cs_collect(&bus, &sample);
    assert(cs_track(&tracker, &sample) == CS_BASELINE && !sample.changed && tracker.transitions == 1);
    puts("PASS a failed query and recovery never fabricate close or detach events");

    f.supported = CS_BASE_ATTACHED;
    f.current = CS_LID_OPEN | CS_BASE_ATTACHED | (1u << 31);
    cs_collect(&bus, &sample);
    assert(sample.raw == f.current && sample.known == CS_BASE_ATTACHED);
    assert(cs_track(&tracker, &sample) == CS_BASELINE && !sample.changed);
    f.current ^= CS_LID_OPEN | (1u << 31);
    cs_collect(&bus, &sample);
    assert(cs_track(&tracker, &sample) == CS_QUIET);
    puts("PASS capability changes reset baseline and unsupported bits never become lid state");

    f.supported = 1u << 31;
    unsigned reads = f.current_reads;
    cs_collect(&bus, &sample);
    assert(sample.status == CS_NO_KNOWN_SWITCHES && f.current_reads == reads && !sample.known);
    assert(cs_track(&tracker, &sample) == CS_UNAVAILABLE);
    f.versions = 1;
    cs_collect(&bus, &sample);
    assert(sample.status == CS_VERSION_UNSUPPORTED && sample.error == CS_VERSION);
    puts("PASS unknown capabilities or unsupported command versions are not boolean false");

    f.versions = 3; f.short_reply = 1;
    cs_collect(&bus, &sample);
    assert(sample.status == CS_ERROR && sample.error == CS_SHORT && !sample.raw);
    assert(cs_track(&tracker, &sample) == CS_UNAVAILABLE && !sample.changed);
    puts("PASS short response has no physical state and all transactions are query-only");
    return 0;
}
