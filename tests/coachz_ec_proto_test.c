#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_ec_proto.h"  /* extracted from coachz-prelink-battery.patch by test_battery_speaker.py */
static void put16(uint8_t *p,int v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static uint32_t reply(uint8_t *in,uint16_t result,const uint8_t *data,uint32_t n){
    in[0]=3;in[1]=0;put16(in+2,result);put16(in+4,(int)n);in[6]=in[7]=0;memcpy(in+8,data,n);
    uint8_t sum=0;for(uint32_t i=0;i<8+n;i++)sum=(uint8_t)(sum+in[i]);in[1]=(uint8_t)-sum;return 8+n;}
int main(void){
    /* HELLO exactly as boot/display_ec.c built it on hardware: version 3, command 1, version 0, four bytes. */
    const uint8_t hello[4]={0x78,0x56,0x34,0x12};uint8_t out[64];
    assert(coachz_ec_request(out,EC_CMD_HELLO,0,hello,4)==12);
    uint8_t sum=0;for(int i=0;i<12;i++)sum=(uint8_t)(sum+out[i]);assert(sum==0);
    const uint8_t expect[12]={3,0xe4,1,0,0,0,4,0,0x78,0x56,0x34,0x12};assert(!memcmp(out,expect,12));
    assert(coachz_ec_request(out,EC_CMD_BATTERY_GET_DYNAMIC,0,(const uint8_t *)"\0",1)==9&&out[2]==1&&out[3]==6);
    uint8_t big[40]={0};assert(coachz_ec_request(out,1,0,big,33)==0);
    /* The Debian baseline: 8559 mV, 0 mA, 3580 of 3580 mAh, battery present, AC present. */
    uint8_t dyn[14];put16(dyn,8559);put16(dyn+2,0);put16(dyn+4,3580);put16(dyn+6,3580);put16(dyn+8,3);put16(dyn+10,8700);put16(dyn+12,0);
    uint8_t in[80];uint32_t n=reply(in,0,dyn,14),len=0;
    assert(coachz_ec_response(in,n,&len)==0&&len==14);
    struct coachz_ec_battery b;assert(coachz_ec_battery_decode(in+8,len,&b));
    assert(b.voltage_mv==8559&&b.current_ma==0&&b.remaining_mah==3580&&b.full_mah==3580&&b.flags==3);
    put16(dyn+2,-1234);n=reply(in,0,dyn,14);assert(!coachz_ec_response(in,n,&len));coachz_ec_battery_decode(in+8,len,&b);assert(b.current_ma==-1234);
    /* Rejections: short, corrupted, an EC error result, an oversize length, a wrong version. */
    assert(coachz_ec_response(in,n-1,&len)==EC_ERR_SHORT);
    in[9]^=1;assert(coachz_ec_response(in,n,&len)==EC_ERR_CHECKSUM);in[9]^=1;
    n=reply(in,3,dyn,0);assert(coachz_ec_response(in,n,&len)==(EC_ERR_RESULT|3<<8));
    n=reply(in,0,dyn,14);in[4]=200;assert(coachz_ec_response(in,n,&len)==EC_ERR_LENGTH);
    n=reply(in,0,dyn,14);in[0]=2;assert(coachz_ec_response(in,n,&len)==EC_ERR_VERSION);
    assert(!coachz_ec_battery_decode(dyn,13,&b));
    uint8_t cs[20]={1,0,0,0, 0x7c,0x21,0,0, 0x10,0,0,0, 0xdc,5,0,0, 100,0,0,0};
    struct coachz_ec_charge c;assert(coachz_ec_charge_decode(cs,20,&c)&&c.ac==1&&c.charge_mv==8572&&c.charge_ma==16&&c.input_ma==1500&&c.percent==100);
    assert(!coachz_ec_charge_decode(cs,19,&c));
    puts("PASS: EC protocol 3 framing matches the hardware-proven HELLO, replies are checked whole, battery and charge decode");
    return 0;}
