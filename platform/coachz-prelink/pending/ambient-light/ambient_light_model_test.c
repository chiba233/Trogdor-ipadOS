/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Test source only: not executed, not registered with the repository tests. */
#include "coachz_als.h"
#include "coachz_auto_brightness.h"
#include <assert.h>
#include <string.h>

struct fixture {
    unsigned calls; uint8_t count,type[4],location[4];
    uint32_t rate; uint16_t lux;
    int short_data,fail_data;
};
static void put32(uint8_t *p,uint32_t x) {
    for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(i*8));
}
static int command(void *context,uint16_t cmd,uint8_t version,const void *request,
    uint32_t length,void *response,uint32_t room,uint32_t *written) {
    struct fixture *f=context;const uint8_t *q=request;uint8_t *r=response;
    assert(room>=64);++f->calls;memset(r,0,room);
    if(cmd==0x0008) {
        assert(version==0 && length==1 && q[0]==0x2b);
        put32(r,2);*written=4;return 0;
    }
    assert(cmd==CA_COMMAND && version==1 && length==CA_PARAMS);
    assert(q[0]==CA_DUMP || q[0]==CA_INFO || q[0]==CA_ODR || q[0]==CA_DATA);
    if(q[0]==CA_DUMP) {assert(q[1]==0);r[1]=f->count;*written=2;return 0;}
    assert(q[1]<f->count);
    if(q[0]==CA_INFO) {r[0]=f->type[q[1]];r[1]=f->location[q[1]];r[2]=8;*written=3;return 0;}
    if(q[0]==CA_ODR) {
        assert(q[2]==0 && q[3]==0 && q[4]==0);
        assert(q[5]==255 && q[6]==255 && q[7]==255 && q[8]==255);
        put32(r,f->rate);*written=4;return 0;
    }
    if(f->fail_data)return 9; /* preserve EC transport error */
    r[0]=2; /* not DUMP PRESENT, not a reason to reject DATA */
    r[2]=(uint8_t)f->lux;r[3]=(uint8_t)(f->lux>>8);
    *written=f->short_data?3:8;return 0;
}
static void test_protocol(void) {
    struct fixture f={0};f.count=2;f.type[0]=0;f.type[1]=1;
    f.location[0]=f.location[1]=CA_LID;
    struct ca_bus bus={&f,command};struct ca_topology t;struct ca_sensor s;
    uint8_t reply[64];uint32_t size=0;
    assert(ca_query(&bus,4,0,reply,&size)==CA_ARGUMENT && f.calls==0); /* never write SENSOR_RANGE */
    assert(ca_discover(&bus,&t)==CA_ABSENT); /* published Coachz topology */
    f.count=3;f.type[2]=CA_LIGHT;f.location[2]=CA_LID;f.rate=1000;f.lux=50000;
    assert(ca_discover(&bus,&t)==0 && ca_select(&t,&s)==0 && s.id==2);
    struct ca_sample sample;
    ca_collect(&bus,&s,&sample);
    assert(sample.status==CA_READY && sample.lux==50000 && sample.odr_millihz==1000);
    f.lux=0;ca_collect(&bus,&s,&sample);assert(sample.status==CA_READY && sample.lux==0);
    f.rate=0;ca_collect(&bus,&s,&sample);assert(sample.status==CA_INACTIVE);
    f.rate=1000;f.short_data=1;ca_collect(&bus,&s,&sample);
    assert(sample.status==CA_ERROR && sample.error==CA_SHORT && sample.lux==0);
    f.short_data=0;f.fail_data=1;ca_collect(&bus,&s,&sample);assert(sample.error==9);
    f.fail_data=0;f.count=4;f.type[3]=CA_LIGHT;f.location[3]=CA_LID;
    assert(ca_discover(&bus,&t)==0 && ca_select(&t,&s)==CA_AMBIGUOUS);
    f.type[2]=f.type[3]=3;assert(ca_discover(&bus,&t)==CA_ABSENT); /* proximity != ALS */
    f.count=33;assert(ca_discover(&bus,&t)==CA_LIMIT);
}
static void test_policy(void) {
    assert(cab_target(0)==CAB_MIN && cab_target(65535000)==CAB_MAX);
    assert(cab_target(100000)==110000);
    struct cab_policy p={0};struct cab_action a;
    a=cab_update(&p,0,1,1000,100000);assert(!a.apply); /* disabled by default */
    cab_enable(&p,1,1);
    a=cab_update(&p,0,1,1000,100000);assert(!a.apply);
    a=cab_update(&p,500,1,1000,100000);assert(!a.apply);
    a=cab_update(&p,1000,1,1000,100000);assert(!a.apply);
    a=cab_update(&p,1500,1,1000,100000);assert(a.apply && a.millinits==260000);
    cab_commit(&p,&a);
    a=cab_update(&p,2000,1,1000,200000);assert(!a.apply && p.hold_until_ms==32000);
    a=cab_update(&p,3000,1,1000,200000);assert(!a.apply); /* manual hold */
    a=cab_update(&p,33000,1,1000,200000);assert(!a.apply); /* fresh baseline */
    cab_enable(&p,1,0);a=cab_update(&p,34000,1,65535,200000);assert(!a.apply);
    cab_enable(&p,1,1);a=cab_update(&p,35000,0,65535,200000);assert(!a.apply && !p.have_sample);
    /* Darkness is valid; after the longer downward dwell it reaches min. */
    uint32_t applied=400000;
    for(uint64_t ms=36000;ms<=43000;ms+=500) {
        a=cab_update(&p,ms,1,0,applied);
        if(a.apply){assert(a.millinits==CAB_MIN);cab_commit(&p,&a);applied=a.millinits;}
    }
    assert(applied==CAB_MIN);
    a=cab_update(&p,1,1,1000,applied);assert(!a.apply); /* clock goes backward */
}
int main(void) {test_protocol();test_policy();return 0;}
