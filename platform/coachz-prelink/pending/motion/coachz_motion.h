/* SPDX-License-Identifier: BSD-2-Clause */
/* Chrome EC motion host-command client. Register/protocol evidence: SOURCES.md.
 * No FIFO consumption, sensor configuration, calibration or direct I2C access.
 */
#ifndef COACHZ_MOTION_H
#define COACHZ_MOTION_H
#include <stdint.h>

#define CM_COMMAND 0x002bu
#define CM_MAX_SENSORS 8u
#define CM_PARAMS 13u /* Packed ec_params_motion_sense at the pinned revision. */
#define CM_REPLY 64u
enum { CM_DUMP = 0, CM_INFO = 1, CM_ODR = 3, CM_RANGE = 4, CM_DATA = 6 };
enum { CM_ACCEL = 0, CM_GYRO = 1, CM_LID = 1, CM_BMI160 = 2, CM_BMI260 = 24 };
enum { CM_SHORT = -1, CM_VERSION = -2, CM_FRAME = -3, CM_CONFIG = -4 };
enum cm_status { CM_UNAVAILABLE, CM_AVAILABLE, CM_INACTIVE, CM_ERROR, CM_UNSUPPORTED };

struct cm_bus {
    void *context;
    int (*command)(void *, uint16_t, uint8_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
};
struct cm_sensor { uint8_t id, type, location, chip; int info_error; };
struct cm_topology {
    uint32_t versions;
    uint8_t module_flags, reported, scanned;
    struct cm_sensor sensors[CM_MAX_SENSORS];
};
struct cm_sample {
    enum cm_status status;
    int error;
    int32_t range, odr_millihz;
    int16_t raw[3];
    /* Accel: micro-g; gyro: micro-degrees/second, NOT radians/second. */
    int64_t scaled[3];
};

static inline uint16_t cm_le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint32_t cm_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline int32_t cm_i32(const uint8_t *p) {
    uint32_t n = cm_le32(p);
    return n <= INT32_MAX ? (int32_t)n : -1 - (int32_t)(UINT32_MAX - n);
}
static inline void cm_clear(void *object, uint32_t size) {
    uint8_t *bytes = (uint8_t *)object;
    for (uint32_t i = 0; i < size; ++i) bytes[i] = 0;
}

static inline void
cm_request(uint8_t *args, uint8_t command, uint8_t sensor)
{
    for (unsigned i = 0; i < CM_PARAMS; ++i) args[i] = 0;
    args[0] = command;
    args[1] = sensor;
    if (command == CM_RANGE || command == CM_ODR)
        for (unsigned i = 5; i < 9; ++i) args[i] = 0xff; /* data = NO_VALUE (-1), query only. */
}

static inline int
cm_query(const struct cm_bus *bus, uint8_t command, uint8_t sensor, uint8_t *reply, uint32_t *length)
{
    uint8_t args[CM_PARAMS];
    cm_request(args, command, sensor);
    *length = 0;
    return bus->command(bus->context, CM_COMMAND, 1, args, sizeof(args), reply, CM_REPLY, length);
}

static inline int
cm_supported(const struct cm_sensor *sensor)
{
    return !sensor->info_error && sensor->location == CM_LID &&
        (sensor->type == CM_ACCEL || sensor->type == CM_GYRO) &&
        (sensor->chip == CM_BMI160 || sensor->chip == CM_BMI260);
}

/* Use version 1's small INFO response deliberately, not the newest unknown ABI. */
static inline int
cm_discover(const struct cm_bus *bus, struct cm_topology *topology)
{
    cm_clear(topology, sizeof(*topology));
    uint8_t reply[CM_REPLY], command = CM_COMMAND;
    uint32_t length = 0;
    int error = bus->command(bus->context, 0x0008, 0, &command, 1, reply, sizeof(reply), &length);
    if (error) return error;
    if (length != 4) return CM_SHORT;
    topology->versions = cm_le32(reply);
    if (!(topology->versions & (1u << 1))) return CM_VERSION;
    error = cm_query(bus, CM_DUMP, 0, reply, &length); /* max_sensor_count=0: count only. */
    if (error) return error;
    if (length != 2) return CM_SHORT;
    topology->module_flags = reply[0];
    topology->reported = reply[1];
    topology->scanned = topology->reported < CM_MAX_SENSORS ? topology->reported : CM_MAX_SENSORS;
    for (unsigned i = 0; i < topology->scanned; ++i) {
        struct cm_sensor *sensor = &topology->sensors[i];
        sensor->id = (uint8_t)i;
        error = cm_query(bus, CM_INFO, sensor->id, reply, &length);
        sensor->info_error = error ? error : (length != 3 ? CM_SHORT : 0);
        if (sensor->info_error) continue;
        sensor->type = reply[0]; sensor->location = reply[1]; sensor->chip = reply[2];
    }
    return 0;
}

static inline int
cm_parameter(const struct cm_bus *bus, uint8_t command, uint8_t sensor, int32_t *value)
{
    uint8_t reply[CM_REPLY];
    uint32_t length;
    int error = cm_query(bus, command, sensor, reply, &length);
    if (error) return error;
    if (length != 4) return CM_SHORT;
    *value = cm_i32(reply);
    return 0;
}

static inline void
cm_collect(const struct cm_bus *bus, const struct cm_sensor *sensor, struct cm_sample *sample)
{
    cm_clear(sample, sizeof(*sample));
    if (!cm_supported(sensor)) {
        sample->status = sensor->info_error ? CM_ERROR : CM_UNSUPPORTED;
        sample->error = sensor->info_error;
        return;
    }
    sample->status = CM_ERROR;
    int error = cm_parameter(bus, CM_ODR, sensor->id, &sample->odr_millihz);
    if (!error && sample->odr_millihz < 0) error = CM_CONFIG;
    if (error) { sample->error = error; return; }
    if (!sample->odr_millihz) { sample->status = CM_INACTIVE; return; }
    error = cm_parameter(bus, CM_RANGE, sensor->id, &sample->range);
    if (!error && (sample->range <= 0 || sample->range > 65535)) error = CM_CONFIG;
    if (error) { sample->error = error; return; }
    uint8_t reply[CM_REPLY];
    uint32_t length;
    error = cm_query(bus, CM_DATA, sensor->id, reply, &length);
    if (!error && length != 8) error = CM_SHORT;
    /* DATA flags are zero in the pinned EC implementation; DUMP's PRESENT
     * bit is NOT a validity bit here. DATA's sensor_num byte isn't filled by
     * that handler either. Associate the reply with the serialized request.
     */
    if (!error && reply[0]) error = CM_FRAME;
    if (error) { sample->error = error; return; }
    for (unsigned i = 0; i < 3; ++i) {
        uint16_t n = cm_le16(reply + 2 + 2 * i);
        sample->raw[i] = n <= INT16_MAX ? (int16_t)n : (int16_t)(-1 - (int32_t)(UINT16_MAX - n));
        sample->scaled[i] = (int64_t)sample->raw[i] * sample->range * 1000000 / 32768;
    }
    sample->status = CM_AVAILABLE;
}

static inline const char *cm_status_name(enum cm_status status) {
    switch (status) {
    case CM_UNAVAILABLE: return "unavailable";
    case CM_AVAILABLE: return "available-latest-ec-value";
    case CM_INACTIVE: return "inactive-odr-zero";
    case CM_ERROR: return "query-error";
    case CM_UNSUPPORTED: return "unsupported-sensor";
    }
    return "unknown";
}
#endif
