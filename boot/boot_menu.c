/* SPDX-License-Identifier: BSD-2-Clause */
/* The boot choice, before the kernel load: the volume buttons or the keyboard's arrows move the selection, the power button or Enter
   confirms it. The keyboard is a USB device behind the board hub, so it only answers once the USB stack has enumerated it. Depthcharge has
   already loaded this whole partition, so both entries are in memory: iPadOS continues to the XNU load, Debian takes the wrapper's
   existing return to the Linux kernel of the same FIT. Without a press the first entry starts after five seconds; a failing EC does
   not block the boot. The caches are off, so only what changed is redrawn. Buttons as the EC reports them: power 0, up 1, down 2. */
#include "display.h"
void usb_menu_start(uint64_t);
unsigned usb_menu_poll(void);
void usb_menu_stop(void);
#define ENTRIES 2
#define ROW_WIDTH 800u
#define ROW_HEIGHT 170u
#define ROW_GAP 40u
#define SCALE 14u
#define WAIT_SECONDS 5u
#define POLL_MS 30u
#define BAR_HEIGHT 12u
#define COLOR_BLACK 0xff000000u
#define COLOR_SELECTED 0xffffffffu
#define COLOR_OTHER 0xff303030u
#define COLOR_OTHER_TEXT 0xffc0c0c0u
static const char names[ENTRIES][7]={"iPadOS","Debian"};
/* 5x7: the letters of the names only. */
static const struct { char letter; uint8_t rows[7]; } font[]={
    {'i',{0x04,0x00,0x0c,0x04,0x04,0x04,0x0e}},{'P',{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}},
    {'a',{0x00,0x00,0x0e,0x01,0x0f,0x11,0x0f}},{'d',{0x01,0x01,0x0d,0x13,0x11,0x11,0x0f}},
    {'O',{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}},{'S',{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}},
    {'D',{0x1c,0x12,0x11,0x11,0x11,0x12,0x1c}},{'e',{0x00,0x00,0x0e,0x11,0x1f,0x10,0x0e}},
    {'b',{0x10,0x10,0x16,0x19,0x11,0x11,0x1e}},{'n',{0x00,0x00,0x16,0x19,0x11,0x11,0x11}},
};
static uint64_t now(void) {
    uint64_t ticks;__asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(ticks)::"memory");return ticks;
}
static void wait_ticks(uint64_t ticks) {
    const uint64_t start=now();while(now()-start<ticks)__asm__ volatile("yield");
}
static void fill(const struct display_video *v,uint64_t left,uint64_t top,uint64_t width,uint64_t height,uint32_t color) {
    for(uint64_t y=0;y<height;y++) {
        volatile uint32_t *row=(volatile uint32_t *)(uintptr_t)(v->pa+(top+y)*v->stride+left*4);
        for(uint64_t x=0;x<width;x++)row[x]=color;
    }
}
static const uint8_t *glyph(char letter) {
    for(unsigned i=0;i<sizeof(font)/sizeof(font[0]);i++)if(font[i].letter==letter)return font[i].rows;
    return 0;
}
static uint64_t row_top(const struct display_video *v,unsigned entry) {
    return (v->height-(ENTRIES*ROW_HEIGHT+(ENTRIES-1)*ROW_GAP))/2+entry*(ROW_HEIGHT+ROW_GAP);
}
static void draw_row(const struct display_video *v,unsigned entry,int selected) {
    const uint64_t left=(v->width-ROW_WIDTH)/2,top=row_top(v,entry);
    const uint32_t text=selected?COLOR_BLACK:COLOR_OTHER_TEXT;
    fill(v,left,top,ROW_WIDTH,ROW_HEIGHT,selected?COLOR_SELECTED:COLOR_OTHER);
    const uint64_t advance=6*SCALE,width=6*advance,x0=left+(ROW_WIDTH-width)/2,y0=top+(ROW_HEIGHT-7*SCALE)/2;
    for(unsigned c=0;c<6;c++) {
        const uint8_t *rows=glyph(names[entry][c]);
        if(!rows)continue;
        for(unsigned y=0;y<7;y++)for(unsigned x=0;x<5;x++)
            if(rows[y]>>(4-x)&1)fill(v,x0+c*advance+x*SCALE,y0+y*SCALE,SCALE,SCALE,text);
    }
}
/* What is left of the wait, under the rows; empty once a button was used. */
static void draw_bar(const struct display_video *v,uint64_t filled) {
    const uint64_t left=(v->width-ROW_WIDTH)/2,top=row_top(v,ENTRIES)+ROW_GAP;
    fill(v,left,top,ROW_WIDTH,BAR_HEIGHT,COLOR_BLACK);
    if(filled)fill(v,left,top,filled,BAR_HEIGHT,COLOR_OTHER_TEXT);
}
unsigned boot_menu(const struct display_video *v,uint64_t freq) {
    if(!v->ready || v->width<ROW_WIDTH || v->height<ENTRIES*(ROW_HEIGHT+ROW_GAP)+2*ROW_GAP+BAR_HEIGHT || freq<1000)return 0;
    unsigned selected=0,failures=0;
    int waiting=1;
    uint32_t held=~0u,error=0;   /* a button already down when the menu appears does not count until it is released */
    for(unsigned entry=0;entry<ENTRIES;entry++)draw_row(v,entry,entry==selected);
    draw_bar(v,ROW_WIDTH);
    usb_menu_start(freq);
    const uint64_t start=now(),limit=freq*WAIT_SECONDS;
    uint64_t drawn=ROW_WIDTH;
    for(;;) {
        uint32_t buttons=0;
        const int answered=display_ec_buttons(freq,&buttons,&error);
        buttons|=usb_menu_poll();
        if(!answered && ++failures>=20)break;   /* no EC, no choice: boot the first entry rather than wait forever */
        if(answered)failures=0;
        {
            const uint32_t pressed=buttons&~held;
            held=buttons;
            if(pressed&1)break;
            if(pressed&6) {
                const unsigned next=(selected+(pressed&2?ENTRIES-1:1))%ENTRIES;
                draw_row(v,selected,0);
                draw_row(v,next,1);
                selected=next;
                if(waiting)draw_bar(v,0);
                waiting=0;
            }
        }
        if(waiting) {
            const uint64_t passed=now()-start;
            if(passed>=limit)break;
            const uint64_t filled=ROW_WIDTH-passed*ROW_WIDTH/limit;
            if(drawn-filled>=ROW_WIDTH/WAIT_SECONDS/8){draw_bar(v,filled);drawn=filled;}
        }
        wait_ticks(freq*POLL_MS/1000);
    }
    usb_menu_stop();
    /* The load draws its picture on black. */
    for(unsigned entry=0;entry<ENTRIES;entry++)fill(v,(v->width-ROW_WIDTH)/2,row_top(v,entry),ROW_WIDTH,ROW_HEIGHT,COLOR_BLACK);
    draw_bar(v,0);
    __asm__ volatile("dsb sy":::"memory");
    return selected;
}
