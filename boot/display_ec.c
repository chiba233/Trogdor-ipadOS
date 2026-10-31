/* SPDX-License-Identifier: BSD-2-Clause */
/* ChromeOS EC host commands (protocol 3) over the GENI SPI engine firmware left
   configured. Every wait is bounded and every path releases chip select. This
   only sends runtime commands; it never touches EC firmware or flash. */
#include "display.h"
#define QUP 0x00a80000u
#define R_LOOPBACK 0x22cu
#define R_DEMUX 0x250u
#define R_BYTE_GRAN 0x254u
#define R_TX_PACK0 0x260u
#define R_TX_PACK1 0x264u
#define R_WORD_LEN 0x268u
#define R_TX_LEN 0x26cu
#define R_RX_LEN 0x270u
#define R_RX_PACK0 0x284u
#define R_RX_PACK1 0x288u
#define R_M_CMD 0x600u
#define R_M_CTRL 0x604u
#define R_M_STATUS 0x610u
#define R_M_CLEAR 0x618u
#define R_TX_FIFO 0x700u
#define R_RX_FIFO 0x780u
#define R_RX_STATUS 0x804u
#define R_TX_WATERMARK 0x80cu
#define IRQ_DONE 1u
#define IRQ_CANCEL (1u<<4)
#define IRQ_ABORT (1u<<5)
#define IRQ_RX_WATERMARK (1u<<26)
#define IRQ_RX_LAST (1u<<27)
#define IRQ_TX_WATERMARK (1u<<30)
#define OP_TX 1u
#define OP_RX 2u
#define OP_CS_ASSERT 8u
#define OP_CS_DEASSERT 9u
#define PARAM_FRAGMENT 4u
#define FIFO_TX_BYTES 60u
#define FRAME_START 0xecu
#define FRAME_PROCESSING 0xfau
#define FRAME_BAD_DATA 0xfbu
#define FRAME_NOT_READY 0xfcu
enum { EC_CS=1,EC_TX,EC_FRAME_BUS,EC_FRAME_BAD,EC_FRAME_BUSY,EC_ACCEPT_TIMEOUT,EC_PROCESS_TIMEOUT,EC_RX,EC_VERSION,EC_RESERVED,EC_LENGTH,EC_CHECKSUM,EC_RESULT,EC_HELLO };
static uint64_t ec_freq,ec_last_stop;
static uint32_t rd(uint32_t offset) { return *(volatile uint32_t *)(uintptr_t)(QUP+offset); }
static void wr(uint32_t offset,uint32_t value) { *(volatile uint32_t *)(uintptr_t)(QUP+offset)=value; }
static uint64_t ticks(void) {
    uint64_t t;__asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(t)::"memory");return t;
}
static uint64_t us(uint64_t count) { return ec_freq*count/1000000; }
static int wait_irq(uint32_t mask,uint64_t timeout) {
    uint64_t start=ticks();
    do {
        uint32_t irq=rd(R_M_STATUS);
        if(irq)wr(R_M_CLEAR,irq);
        if(irq&mask)return 1;
    } while(ticks()-start<timeout);
    return 0;
}
static void cancel(void) {
    wr(R_TX_WATERMARK,0);wr(R_M_CTRL,1u<<2);
    if(!wait_irq(IRQ_CANCEL,us(1000))){wr(R_M_CTRL,1u<<1);wait_irq(IRQ_ABORT,us(1000));}
}
static int chip_select(uint32_t op) {
    wr(R_M_CLEAR,rd(R_M_STATUS));
    wr(R_M_CMD,op<<27);
    if(wait_irq(IRQ_DONE,us(1000)))return 1;
    cancel();return 0;
}
static int transfer(uint8_t *in,const uint8_t *out,uint32_t size) {
    wr(R_LOOPBACK,0);wr(R_DEMUX,0);wr(R_WORD_LEN,4);
    wr(R_TX_PACK0,0x0feu|0x1feu<<10);wr(R_TX_PACK1,0x2feu|0x3ffu<<10);
    wr(R_RX_PACK0,0x0feu|0x1feu<<10);wr(R_RX_PACK1,0x2feu|0x3ffu<<10);
    wr(R_BYTE_GRAN,0);
    if(out){wr(R_TX_LEN,size);wr(R_TX_WATERMARK,1);}
    if(in)wr(R_RX_LEN,size);
    wr(R_M_CMD,(out?OP_TX:OP_RX)<<27|PARAM_FRAGMENT);
    uint32_t tx=out?size:0,rx=in?size:0,irq=0;uint64_t start=ticks();
    do {
        irq=rd(R_M_STATUS);
        if(in && irq&(IRQ_RX_WATERMARK|IRQ_RX_LAST)) {
            for(uint32_t words=rd(R_RX_STATUS)&0x1ffffffu;words && rx;words--) {
                uint32_t word=rd(R_RX_FIFO);
                for(unsigned j=0;j<4 && rx;j++,rx--)*in++=(uint8_t)(word>>(8*j));
            }
        }
        if(out && irq&IRQ_TX_WATERMARK) {
            uint32_t chunk=tx<FIFO_TX_BYTES?tx:FIFO_TX_BYTES,last=tx==chunk;
            for(uint32_t sent=0;sent<chunk;sent+=4) {
                uint32_t word=0;
                for(unsigned j=0;j<4 && sent+j<chunk;j++)word|=(uint32_t)out[sent+j]<<(8*j);
                wr(R_TX_FIFO,word);
            }
            out+=chunk;tx-=chunk;
            if(last)wr(R_TX_WATERMARK,0);
        }
        if(irq)wr(R_M_CLEAR,irq);
        if(irq&IRQ_DONE)break;
    } while(ticks()-start<us(1000000));
    if(!(irq&IRQ_DONE) || tx || rx){cancel();return 0;}
    return 1;
}
static uint32_t wait_for_frame(void) {
    uint64_t start=ticks();int accepted=0;
    for(;;) {
        uint8_t byte=0;
        if(!transfer(&byte,0,1))return EC_FRAME_BUS;
        if(byte==FRAME_START)return 0;
        if(byte==FRAME_PROCESSING)accepted=1;
        if(byte==FRAME_BAD_DATA)return EC_FRAME_BAD;
        if(byte==FRAME_NOT_READY)return EC_FRAME_BUSY;
        uint64_t waited=ticks()-start;
        if(!accepted && waited>us(50000))return EC_ACCEPT_TIMEOUT;
        if(waited>us(1000000))return EC_PROCESS_TIMEOUT;
    }
}
static uint32_t parse_reply(const uint8_t *in,uint32_t want,uint8_t *reply) {
    uint32_t result=in[2]|(uint32_t)in[3]<<8,length=in[4]|(uint32_t)in[5]<<8;
    if(in[0]!=3)return EC_VERSION;
    if(in[6] || in[7])return EC_RESERVED;
    if(length>want)return EC_LENGTH;
    uint8_t sum=0;
    for(uint32_t i=0;i<8+length;i++)sum=(uint8_t)(sum+in[i]);
    if(sum)return EC_CHECKSUM;
    if(result)return EC_RESULT|result<<8;
    for(uint32_t i=0;i<want;i++)reply[i]=i<length?in[8+i]:0;
    return 0;
}
static uint32_t command(uint16_t code,const uint8_t *data,uint32_t size,uint8_t *reply,uint32_t want) {
    uint8_t out[16],in[16];uint32_t status;
    out[0]=3;out[1]=0;out[2]=(uint8_t)code;out[3]=(uint8_t)(code>>8);out[4]=0;out[5]=0;out[6]=(uint8_t)size;out[7]=0;
    uint8_t sum=(uint8_t)(3+out[2]+out[3]+out[6]);
    for(uint32_t i=0;i<size;i++){out[8+i]=data[i];sum=(uint8_t)(sum+data[i]);}
    out[1]=(uint8_t)-sum;
    while(ticks()-ec_last_stop<us(200))__asm__ volatile("yield");
    if(!chip_select(OP_CS_ASSERT))return EC_CS;
    status=transfer(0,out,8+size)?wait_for_frame():EC_TX;
    if(!status)status=transfer(in,0,8+want)?parse_reply(in,want,reply):EC_RX;
    chip_select(OP_CS_DEASSERT);ec_last_stop=ticks();
    return status;
}
int display_ec_hello(uint64_t freq,uint32_t *error) {
    uint8_t data[4]={0x78,0x56,0x34,0x12},reply[4];
    ec_freq=freq;
    uint32_t status=command(1,data,4,reply,4);
    if(!status && (reply[0]|(uint32_t)reply[1]<<8|(uint32_t)reply[2]<<16|(uint32_t)reply[3]<<24)!=0x12345678u+0x01020304u)status=EC_HELLO;
    if(status)*error=status;
    return !status;
}
int display_ec_backlight(uint64_t freq,uint16_t duty,uint32_t *error) {
    uint8_t data[4]={(uint8_t)duty,(uint8_t)(duty>>8),2,0};
    ec_freq=freq;
    uint32_t status=command(0x25,data,4,0,0);
    if(status)*error=status;
    return !status;
}
