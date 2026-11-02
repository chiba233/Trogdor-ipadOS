/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Chrome EC LIGHT protocol client; independently expressed from Linux facts.
 * No direct I2C, MEMMAP fallback, FIFO consumption or calibration writes.
 * See SOURCES.md: Coachz's published EC tables do NOT register a LIGHT sensor.
 */
#ifndef COACHZ_ALS_H
#define COACHZ_ALS_H
#include <stdint.h>

#define CA_COMMAND 0x002bu
#define CA_PARAMS 13u
#define CA_MAX_SENSORS 32u
enum {CA_DUMP=0, CA_INFO=1, CA_ODR=3, CA_DATA=6, CA_LIGHT=4, CA_LID=1};
enum ca_error {CA_SHORT=-1, CA_VERSION=-2, CA_LIMIT=-3, CA_ABSENT=-4,
    CA_AMBIGUOUS=-5, CA_NOT_LIGHT=-6, CA_RATE=-7, CA_ARGUMENT=-8};
enum ca_status {CA_UNAVAILABLE, CA_READY, CA_INACTIVE, CA_ERROR, CA_PAUSED};
struct ca_bus {
    void *context;
    int (*command)(void *, uint16_t, uint8_t, const void *, uint32_t,
        void *, uint32_t, uint32_t *);
};
struct ca_sensor {uint8_t id, type, location, chip;};
struct ca_topology {
    uint32_t versions;
    uint8_t reported, scanned, light_count, lid_light_count;
    struct ca_sensor light[CA_MAX_SENSORS];
};
struct ca_sample {
    enum ca_status status;
    int error;
    uint16_t lux; /* EC processed unsigned lux, including valid darkness=0 */
    uint32_t odr_millihz;
    uint8_t flags; /* retained, not interpreted as DUMP PRESENT validity */
};
static inline uint16_t ca_u16(const uint8_t *p) {return p[0] | (uint16_t)p[1]<<8;}
static inline uint32_t ca_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static inline int ca_query(const struct ca_bus *bus, uint8_t cmd,
    uint8_t sensor, uint8_t *reply, uint32_t *length) {
    uint8_t request[CA_PARAMS] = {0};
    if (!bus || !bus->command || !reply || !length) return CA_ARGUMENT;
    if (cmd!=CA_DUMP && cmd!=CA_INFO && cmd!=CA_ODR && cmd!=CA_DATA) return CA_ARGUMENT;
    request[0]=cmd; request[1]=sensor;
    /* Packed union: command byte, then sensor/roundup/reserved[2]/data.
     * NO_VALUE=-1 starts at byte5, not byte4. This never changes ODR. */
    if (cmd==CA_ODR) for (unsigned i=5;i<9;++i) request[i]=0xff;
    *length=0;
    return bus->command(bus->context,CA_COMMAND,1,request,sizeof(request),reply,64,length);
}
static inline int ca_discover(const struct ca_bus *bus, struct ca_topology *t) {
    uint8_t reply[64], command=(uint8_t)CA_COMMAND;
    uint32_t size=0;
    if (!bus || !bus->command || !t) return CA_ARGUMENT;
    *t=(struct ca_topology){0};
    int error=bus->command(bus->context,0x0008,0,&command,1,reply,sizeof(reply),&size);
    if (error) return error;
    if (size!=4) return CA_SHORT;
    t->versions=ca_u32(reply);
    if (!(t->versions & (1u<<1))) return CA_VERSION;
    /* DUMP max_sensor_count=0 asks for count only. No assumed ALS ID. */
    error=ca_query(bus,CA_DUMP,0,reply,&size);
    if (error) return error;
    if (size!=2) return CA_SHORT;
    t->reported=reply[1];
    if (t->reported>CA_MAX_SENSORS) return CA_LIMIT;
    for (unsigned i=0;i<t->reported;++i) {
        error=ca_query(bus,CA_INFO,(uint8_t)i,reply,&size);
        if (error) return error;
        if (size!=3) return CA_SHORT;
        ++t->scanned;
        if (reply[0]!=CA_LIGHT) continue; /* PROX=3 is NOT an illuminance sensor */
        struct ca_sensor *s=&t->light[t->light_count++];
        s->id=(uint8_t)i; s->type=reply[0]; s->location=reply[1]; s->chip=reply[2];
        if (s->location==CA_LID) ++t->lid_light_count;
    }
    return t->light_count ? 0 : CA_ABSENT;
}
/* Choose a unique lid LIGHT; never silently choose the first of several.
 * A board-specific override can pass a discovered sensor explicitly instead. */
static inline int ca_select(const struct ca_topology *t, struct ca_sensor *sensor) {
    if (!t || !sensor) return CA_ARGUMENT;
    if (!t->lid_light_count) return CA_ABSENT;
    if (t->lid_light_count!=1) return CA_AMBIGUOUS;
    for (unsigned i=0;i<t->light_count;++i) if (t->light[i].location==CA_LID) {
        *sensor=t->light[i]; return 0;
    }
    return CA_ABSENT;
}
static inline void ca_collect(const struct ca_bus *bus, const struct ca_sensor *sensor,
    struct ca_sample *sample) {
    if (!sample) return;
    *sample=(struct ca_sample){0};
    sample->status=CA_ERROR;
    if (!sensor || sensor->type!=CA_LIGHT) {sample->error=CA_NOT_LIGHT;return;}
    uint8_t reply[64]; uint32_t size=0;
    int error=ca_query(bus,CA_ODR,sensor->id,reply,&size);
    if (!error && size!=4) error=CA_SHORT;
    if (error) {sample->error=error;return;}
    sample->odr_millihz=ca_u32(reply);
    if (sample->odr_millihz>0x7fffffffu) {sample->error=CA_RATE;return;}
    if (!sample->odr_millihz) {sample->status=CA_INACTIVE;return;}
    error=ca_query(bus,CA_DATA,sensor->id,reply,&size);
    if (!error && size!=8) error=CA_SHORT;
    if (error) {sample->error=error;return;}
    /* cros_ec_sensors_read_cmd takes data[0] at byte2. Linux's light
     * driver interprets it as u16 PROCESSED lux; no signed extension,
     * SENSOR_RANGE multiplication, RGB synthesis or extra calibration. */
    sample->flags=reply[0]; sample->lux=ca_u16(reply+2);
    sample->status=CA_READY;
}
static inline const char *ca_status_name(enum ca_status s) {
    switch(s) {
    case CA_READY:return "available-latest-ec-lux";
    case CA_INACTIVE:return "inactive-odr-zero";
    case CA_ERROR:return "query-error";
    case CA_PAUSED:return "paused-display-asleep";
    default:return "unavailable";
    }
}
#endif
