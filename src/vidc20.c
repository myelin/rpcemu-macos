/*RPCemu v0.6 by Tom Walker
  VIDC20 emulation*/
#include <stdint.h>
#include <allegro.h>
#include "rpcemu.h"
#include "vidc20.h"
#include "keyboard.h"
#include "sound.h"
#include "mem.h"
#include "iomd.h"

int fullscreen=0;
int readflash = 0;
static BITMAP *b = NULL,*bs = NULL,*bs2 = NULL,*bs3=NULL,*bs4=NULL;
static int currentbuffer=1;
static int oldsx = 0,oldsy = 0;
static int drawcode = 0;


/* This state is written by the main thread. The display thread should not read it. */
struct vidc_state {
        uint32_t vidcpal[0x104];
        int palindex;
        uint32_t hdsr,hcsr,hder;
        uint32_t vdsr,vcsr,vcer,vder;
        uint32_t b0,b1;
        int bit8;
        int palchange;
        int curchange;
} vidc;

/* This state is a cached version of the main state, and is read by the display thread.
   The main thread should only change them when it has the mutex (and so the display
   thread is not running). */
static struct cached_state {
        struct
        {
                uint32_t r,g,b;
        } pal[256];
        uint16_t pal16lookup[65536];
        uint32_t vpal[260];
        uint32_t iomd_vidstart;
        uint32_t iomd_vidend;
        uint32_t iomd_vidinit;
        unsigned char iomd_vidcr;
        int xsize;
        int ysize;
        int cursorx;
        int cursory;
        int cursorheight;
        int lastblock;
        int doublesize;
        int bpp;
        uint8_t *dirtybuffer;
        int needvsync;
        int threadpending;
} thr;

/* Two dirty buffers, so one can be written to by the main thread
   while the display thread is reading the other */
uint8_t dirtybuffer1[512*4];
uint8_t dirtybuffer2[512*4];

/* Dirty buffer currently in use by main thread */
uint8_t *dirtybuffer = dirtybuffer1;

static void blitterthread(int xs, int ys, int yl, int yh, int doublesize)
{
        BITMAP *backbuf;
        if (fullscreen)
        {
                switch (currentbuffer)
                {
                        case 0: backbuf=bs4; break;
                        case 1: backbuf=bs2; break;
                        case 2: backbuf=bs3; break;
                }
        }
        switch (doublesize)
        {
                case 0:
                if (!(fullscreen && stretchmode)) ys=yh-yl;
                if (fullscreen)
                {
                        if (stretchmode) blit(b,backbuf,0,0,(SCREEN_W-xs)>>1,((SCREEN_H-oldsy)>>1),xs,ys);
                        else             blit(b,backbuf,0,yl,(SCREEN_W-xs)>>1,yl+((SCREEN_H-oldsy)>>1),xs,ys);
                }
                else            blit(b,screen,0,yl,0,yl,xs,ys);
                break;
                case 1:
                ys=yh-yl;
                if (fullscreen) stretch_blit(b,backbuf,0,yl,xs,ys, (SCREEN_W-(xs<<1))>>1,yl+((SCREEN_H-oldsy)>>1),xs<<1,ys);
                else            stretch_blit(b,screen, 0,yl,xs,ys, 0,                    yl,                      xs<<1,ys);
                break;
                case 2:
                if (stretchmode)
                {
                        if (fullscreen) stretch_blit(b,backbuf,0,0,xs,ys,0,0,xs,(ys<<1)-1);
                        else            stretch_blit(b,screen, 0,0,xs,ys,0,0,xs,(ys<<1)-1);
                }
                else
                {
                        ys=yh-yl;
                        if (fullscreen) stretch_blit(b,backbuf,0,yl,xs,ys, (SCREEN_W-xs)>>1,(yl<<1)+((SCREEN_H-oldsy)>>1),xs,(ys<<1)-1);
                        else            stretch_blit(b,screen, 0,yl,xs,ys, 0,               yl<<1,                        xs,(ys<<1)-1);
                }
                break;
                case 3:
                if (stretchmode)
                {
                        if (fullscreen) stretch_blit(b,backbuf,0,0,xs,ys,(SCREEN_W-(xs<<1))>>1,((SCREEN_H-oldsy)>>1),xs<<1,(ys<<1)-1);
                        else            stretch_blit(b,screen, 0,0,xs,ys,0,0,xs<<1,(ys<<1)-1);
                }
                else
                {
                        ys=yh-yl;
                        if (fullscreen) stretch_blit(b,backbuf,0,yl,xs,ys, (SCREEN_W-(xs<<1))>>1,(yl<<1)+((SCREEN_H-oldsy)>>1),xs<<1,(ys<<1)-1);
                        else            stretch_blit(b,screen, 0,yl,xs,ys, 0,                    yl<<1,                        xs<<1,(ys<<1)-1);
                }
                break;
        }
        if (fullscreen)
        {
                switch (currentbuffer)
                {
                        case 0: request_video_bitmap(bs4); currentbuffer=1; break;
                        case 1: request_video_bitmap(bs2); currentbuffer=2; break;
                        case 2: request_video_bitmap(bs3); currentbuffer=0; break;
                }
        }
}

#define DEFAULT_W 640
#define DEFAULT_H 480

void initvideo()
{
        int depth;

#ifdef FULLSCREENALWAYS
       depth=16;
       set_color_depth(16);
       if (set_gfx_mode(GFX_AUTODETECT_FULLSCREEN,DEFAULT_W,DEFAULT_H,0,0))
       {
                set_color_depth(15);
                depth=15;
                if (set_gfx_mode(GFX_AUTODETECT_FULLSCREEN,DEFAULT_W,DEFAULT_H,0,0))
                {
                        printf("Failed to set video mode 640x480x16\n");
                        exit(-1);
                }
       }
       drawcode=16;
#else
        depth=desktop_color_depth();
        if (depth==16 || depth==15)
        {
                set_color_depth(15);
                depth=15;
                if (set_gfx_mode(GFX_AUTODETECT_WINDOWED,DEFAULT_W,DEFAULT_H,0,0))
                {
                        set_color_depth(16);
                        depth=16;
                        set_gfx_mode(GFX_AUTODETECT_WINDOWED,DEFAULT_W,DEFAULT_H,0,0);
                }
                drawcode=16;
        }
        else if (depth==32)
        {
                set_color_depth(depth);
                set_gfx_mode(GFX_AUTODETECT_WINDOWED,DEFAULT_W,DEFAULT_H,0,0);
                drawcode=32;
        }
        else
        {
                error("Your desktop must be set to either 16-bit or 32-bit colour to run RPCemu");
                exit(0);
        }
#endif

        oldsx=oldsy=-1;
        memset(&thr, 0, sizeof(thr));
        memset(dirtybuffer1,1,512*4);
        memset(dirtybuffer2,1,512*4);
        vidcstartthread();
}


int getxs()
{
        return vidc.hder-vidc.hdsr;
}
int getys()
{
        return vidc.vder-vidc.vdsr;
}

static void freebitmaps(void)
{
        if (b) destroy_bitmap(b);
        if (bs) destroy_bitmap(bs);
        if (bs2) destroy_bitmap(bs2);
        if (bs3) destroy_bitmap(bs3);
        if (bs4) destroy_bitmap(bs4);
        b = NULL;
        bs = NULL;
        bs2 = NULL;
        bs3 = NULL;
        bs4 = NULL;
}

static const int fullresolutions[][2]=
{
        {320,200},
        {320,240},
        {400,300},
        {512,384},
        {640,400},
        {640,480},
        {800,600},
        {1024,768},
        {1280,800},
        {1280,1024},
        {1600,1200},
        {1920,1200},
        {-1,-1}
};

void resizedisplay(int x, int y)
{
        int c;
//        rpclog("Change mode to %ix%i\n",x,y);
        #ifdef FULLSCREENALWAYS
        fullscreen=1;
        #endif
        if (x<16) x=16;
        if (y<16) y=16;
        oldsx=x;
        oldsy=y;
        freebitmaps();
        if (fullscreen)
        {
                c=0;
                tryagain:
                while (fullresolutions[c][0]!=-1)
                {
                        if (fullresolutions[c][0]>=x && fullresolutions[c][1]>=y)
                           break;
                        c++;
                }
                if (fullresolutions[c][0]==-1) c--;
//                rpclog("Trying %ix%i\n",fullresolutions[c][0],fullresolutions[c][1]);
                if (set_gfx_mode(GFX_AUTODETECT_FULLSCREEN,fullresolutions[c][0],fullresolutions[c][1],0,0))
                {
                        if (fullresolutions[c+1][0]==-1) /*Reached the end of resolutions, go for something safe*/
                        {
//                                rpclog("Failed - falling back on 640x480\n");
                                set_gfx_mode(GFX_AUTODETECT_FULLSCREEN,640,480,0,0);
                        }
                        else
                        {
                                c++;
                                goto tryagain;
                        }
                }
#ifdef HARDWAREBLIT                
//                rpclog("Mode set\n");
                bs=create_video_bitmap(fullresolutions[c][0],fullresolutions[c][1]);
                bs2=create_video_bitmap(fullresolutions[c][0],fullresolutions[c][1]);
                bs3=create_video_bitmap(fullresolutions[c][0],fullresolutions[c][1]);
                bs4=create_video_bitmap(fullresolutions[c][0],fullresolutions[c][1]);
//                rpclog("%08X %08X\n",bs,bs2);
                clear(bs);
                clear(bs2);
                clear(bs3);
                clear(bs4);
                show_video_bitmap(bs4);
                currentbuffer=1;
                b=create_video_bitmap(x+16,y+16);
                if (!b) /*Video bitmaps unavailable for some reason*/
#endif
                   b=create_bitmap(x+16,y+16);
        }
        else
        {
                set_gfx_mode(GFX_AUTODETECT_WINDOWED,x,y,0,0);
                updatewindowsize(x,y);
                bs=create_video_bitmap(x,y);
                b=create_video_bitmap(x,y);
                if (!b) /*Video bitmaps unavailable for some reason*/
                   b=create_bitmap(x,y);
        }
        resetbuffer();
}

void closevideo()
{
//        rpclog("Calling closevideo()\n");
        vidcendthread();
        freebitmaps();
}
        
void togglefullscreen(int fs)
{
        fullscreen=fs;
        oldsx=oldsy=-1;
        memset(dirtybuffer,1,512*4);
}

static const int xdiff[8]={8192,4096,2048,1024,512,512,256,256};

static void calccrc(unsigned short *crc, unsigned char byte)
{
        int i;
        for (i = 0; i < 8; i++) {
                if (*crc & 0x8000) {
                        *crc <<= 1;
                        if (!(byte & 0x80)) *crc ^= 0x1021;
                } else {
                        *crc <<= 1;
                        if (byte & 0x80) *crc ^= 0x1021;
                }
                byte <<= 1;
        }
}

/* Called periodically from the main thread. If needredraw is non-zero
   then the refresh timer indicates it is time for a new frame */
void drawscr(int needredraw)
{
        static int lastframeborder=0;
        int x,y;
        int c;
        int firstblock,lastblock;

        /* Must get the mutex before altering the thread's state. */
        if (!vidctrymutex()) return;

        /* If the thread hasn't run since the last request then don't request it again */
        if (thr.threadpending) needredraw = 0;

        if (needredraw)
        {
                        
                thr.xsize=vidc.hder-vidc.hdsr;
                thr.ysize=vidc.vder-vidc.vdsr;
                thr.cursorx=vidc.hcsr-vidc.hdsr;
                thr.cursory=vidc.vcsr-vidc.vdsr;
                thr.cursorheight=vidc.vcer-vidc.vcsr;

                if (vidc.palchange)
                {
                        int i;
                        for (i=0; i < 0x100; i++)
                        {
                                thr.pal[i].r=makecol(vidc.vidcpal[i]&0xFF,0,0);
                                thr.pal[i].g=makecol(0,(vidc.vidcpal[i]>>8)&0xFF,0);
                                thr.pal[i].b=makecol(0,0,(vidc.vidcpal[i]>>16)&0xFF);
                        }
                        for (i=0; i < 0x104; i++)
                        {
                                thr.vpal[i]=makecol(vidc.vidcpal[i]&0xFF,(vidc.vidcpal[i]>>8)&0xFF,(vidc.vidcpal[i]>>16)&0xFF);
                        }
                        if ((vidc.bit8 == 4) && (drawcode == 16))
                        {
                                int i;
                                for (i=0;i<65536;i++)
                                {
                                        thr.pal16lookup[i]=thr.pal[i&0xFF].r|thr.pal[(i>>4)&0xFF].g|thr.pal[i>>8].b;
                                }
                        }
                }


//                rpclog("Draw screen\n");
                thr.iomd_vidstart = iomd.vidstart;
                thr.iomd_vidend = iomd.vidend;
                thr.iomd_vidinit = iomd.vidinit;
                thr.iomd_vidcr = iomd.vidcr;
                thr.bpp = vidc.bit8;
//                rpclog("XS %i YS %i\n",thr.xsize,thr.ysize);
                if (thr.xsize<2) thr.xsize=2;
                if (thr.ysize<1) thr.ysize=480;
                thr.doublesize=0;
#ifdef HARDWAREBLIT
                if (thr.xsize<=448 || (thr.xsize<=480 && thr.ysize<=352))
                {
                        thr.xsize<<=1;
                        thr.doublesize=1;
                }
                if (thr.ysize<=352)
                {
                        thr.ysize<<=1;
                        thr.doublesize|=2;
                }
#endif
                if (thr.ysize!=oldsy || thr.xsize!=oldsx) resizedisplay(thr.xsize,thr.ysize);
                if (!(thr.iomd_vidcr&0x20) || vidc.vdsr>vidc.vder)
                {
                        lastframeborder=1;
                        if (dirtybuffer[0] || vidc.palchange)
                        {
                                dirtybuffer[0]=0;
                                vidc.palchange=0;
                                rectfill(b,0,0,thr.xsize,thr.ysize,thr.vpal[0x100]);
//                                      printf("%i %i\n",thr.xsize,thr.ysize);
                                blit(b,screen,0,0,0,0,thr.xsize,thr.ysize);
                        }
                        needredraw = 0;
                        thr.needvsync = 1;
                }
        }

        if (needredraw)
        {
                if (vidc.palchange)
                {
                        resetbuffer();
                        vidc.palchange=0;
                }

                if (thr.doublesize&1) thr.xsize>>=1;
                if (thr.doublesize&2) thr.ysize>>=1;
                if (lastframeborder)
                {
                        lastframeborder=0;
                        resetbuffer();
                }
        
                if (mousehack)
                {
                        static int oldcursorx=0;
                        static int oldcursory=0;

                        getmousepos(&thr.cursorx,&thr.cursory);
                        if (thr.cursory!=oldcursory || thr.cursorx!=oldcursorx) vidc.curchange=1;
                        oldcursory=thr.cursory;
                        oldcursorx=thr.cursorx;
//                        printf("Mouse at %i,%i %i\n",thr.cursorx,thr.cursory,thr.cursorheight);
                }
        
                x=y=c=0;
                firstblock=lastblock=-1;
                while (y<thr.ysize)
                {
                        if (dirtybuffer[c++])
                        {
                                lastblock=c;
                                if (firstblock==-1) firstblock=c;
                        }
                        x+=(xdiff[vidc.bit8]<<2);
                        while (x>thr.xsize)
                        {
                                x-=thr.xsize;
                                y++;
                        }
                }
                thr.lastblock = lastblock;
                thr.dirtybuffer = dirtybuffer;
                dirtybuffer = (dirtybuffer == dirtybuffer1) ? dirtybuffer2 : dirtybuffer1;

                if (firstblock==-1 && !vidc.curchange) 
                {
                        unsigned short crc=0xFFFF;
                        static uint32_t curcrc=0;
                        unsigned char *ramp;
                        int addr;
                        /*Not looking good for screen redraw - check to see if cursor data has changed*/
                        if (cinit&0x4000000) ramp=(unsigned char *)ram2;
                        else                 ramp=(unsigned char *)ram;
                        addr=(cinit&rammask);//>>2;
                        for (c=0;c<(thr.cursorheight<<3);c++)
                            calccrc(&crc, ramp[addr++]);
                        /*If cursor data matches then no point redrawing screen - return*/
                        if (crc==curcrc && skipblits)
                        {
                                needredraw = 0;
                                thr.needvsync = 1;
                        }
                        curcrc=crc;
                }
                vidc.curchange=0;
//                rpclog("First block %i %08X last block %i %08X finished at %i %08X\n",firstblock,firstblock,lastblock,lastblock,c,c);
        }
        if (needredraw)
        {
                for (c=0;c<1024;c++)
                {
//                        rpclog("%03i %08X %08X %08X\n",c,vraddrphys[c],(vraddrphys[c]&0x1F000000),vraddrls[c]<<12);
                        if ((vwaddrphys[c]&0x1F000000)==0x02000000)
                        {
//                                rpclog("Invalidated %08X\n",vraddrls[c]);
                                vwaddrl[vwaddrls[c]]=0xFFFFFFFF;
                                vwaddrphys[c]=vwaddrls[c]=0xFFFFFFFF;
                        }
                }
                thr.threadpending = 1;
        }

        if (thr.needvsync) 
        {
            iomdvsync();
            thr.needvsync = 0;
        }
        vidcreleasemutex();

        if (needredraw) vidcwakeupthread();
}

/* VIDC display thread. This is called whenever vidcwakeupthread() signals it.
   It will only be called when it has the vidc mutex. */ 
void vidcthread()
{
        uint32_t *vidp=NULL;
        unsigned short *vidp16=NULL;
        int drawit=0;
        int x = 0;
        int y = 0;
        unsigned char *ramp;
        unsigned short *ramw;
        int addr;
        int yl=-1,yh=-1;
        static int oldcursorheight;
        static int oldcursory;

        thr.threadpending = 0;

        if (b == NULL) abort();

        if (thr.iomd_vidinit&0x10000000) ramp=ramb;
        else                         ramp=vramb;
        ramw=(unsigned short *)ramp;

        addr=thr.iomd_vidinit&0x7FFFFF;

        drawit=thr.dirtybuffer[addr>>12];
        if (drawit) yl=0;

        switch (drawcode)
        {
                case 16:
                switch (thr.bpp)
                {
                        case 0: /*1 bpp*/
                        thr.xsize>>=1;
                        for (;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
//                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (;x<thr.xsize;x+=64)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<64;xx+=4)
                                                {
#ifdef _RPCEMU_BIG_ENDIAN
                                                        vidp[x+xx+3]=thr.vpal[ramp[addr]&1]     |(thr.vpal[(ramp[addr]>>1)&1]<<16);
                                                        vidp[x+xx+2]=thr.vpal[(ramp[addr]>>2)&1]|(thr.vpal[(ramp[addr]>>3)&1]<<16);
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr]>>4)&1]|(thr.vpal[(ramp[addr]>>5)&1]<<16);
                                                        vidp[x+xx]=  thr.vpal[(ramp[addr]>>6)&1]|(thr.vpal[(ramp[addr]>>7)&1]<<16);
#else
                                                        vidp[x+xx]=  thr.vpal[ramp[addr]&1]     |(thr.vpal[(ramp[addr]>>1)&1]<<16);
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr]>>2)&1]|(thr.vpal[(ramp[addr]>>3)&1]<<16);
                                                        vidp[x+xx+2]=thr.vpal[(ramp[addr]>>4)&1]|(thr.vpal[(ramp[addr]>>5)&1]<<16);
                                                        vidp[x+xx+3]=thr.vpal[(ramp[addr]>>6)&1]|(thr.vpal[(ramp[addr]>>7)&1]<<16);
#endif
                                                        addr++;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                                x=0;
                        }
                        thr.xsize<<=1;
                        break;
                        case 1: /*2 bpp*/
                        thr.xsize>>=1;
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
//                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=32)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<32;xx+=2)
                                                {
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&3]|(thr.vpal[(ramp[addr]>>2)&3]<<16);
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr]>>4)&3]|(thr.vpal[(ramp[addr]>>6)&3]<<16);
                                                        addr++;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        thr.xsize<<=1;
                        break;
                        case 2: /*4 bpp*/
                        thr.xsize>>=1;
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))                                
                                {
                                        drawit=1;
//                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
//                                rpclog("Line %i drawit %i addr %06X\n",y,drawit,addr);
                                for (x=0;x<thr.xsize;x+=16)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<16;xx+=4)
                                                {
#ifdef _RPCEMU_BIG_ENDIAN
//                                                                                                              if (!x && !y) printf("%02X %04X\n",ramp[addr],vpal[ramp[addr&0xF]]);
                                                        vidp[x+xx+3]=thr.vpal[ramp[addr]>>4]|(thr.vpal[ramp[addr]&0xF]<<16);
                                                        vidp[x+xx+2]=thr.vpal[ramp[addr+1]>>4]|(thr.vpal[ramp[addr+1]&0xF]<<16);
                                                        vidp[x+xx+1]=thr.vpal[ramp[addr+2]>>4]|(thr.vpal[ramp[addr+2]&0xF]<<16);
                                                        vidp[x+xx]=thr.vpal[ramp[addr+3]>>4]|(thr.vpal[ramp[addr+3]&0xF]<<16);
#else
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&0xF]|(thr.vpal[ramp[addr]>>4]<<16);
                                                        vidp[x+xx+1]=thr.vpal[ramp[addr+1]&0xF]|(thr.vpal[ramp[addr+1]>>4]<<16);
                                                        vidp[x+xx+2]=thr.vpal[ramp[addr+2]&0xF]|(thr.vpal[ramp[addr+2]>>4]<<16);
                                                        vidp[x+xx+3]=thr.vpal[ramp[addr+3]&0xF]|(thr.vpal[ramp[addr+3]>>4]<<16);                                                                                                                                                                       
#endif
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend)
                                        {
                                                addr=thr.iomd_vidstart;
                                        }
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
//                                                rpclog("Hit 4k boundary %06X %i,%i %i\n",addr,x,y,drawit);
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        thr.xsize<<=1;
                        break;
                        case 3: /*8 bpp*/
                        thr.xsize>>=1;
//                        rpclog("Start %08X End %08X Init %08X\n",thr.iomd_vidstart,thr.iomd_vidend,addr);
                        for (;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-1)))
                                {
                                        drawit=1;
//                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit)
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (;x<thr.xsize;x+=8)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<8;xx+=2)
                                                {
#ifdef _RPCEMU_BIG_ENDIAN
                                                        vidp[x+xx+1]=thr.vpal[ramp[addr+1]&0xFF]|(thr.vpal[ramp[addr]&0xFF]<<16);
                                                        vidp[x+xx]=thr.vpal[ramp[addr+3]&0xFF]|(thr.vpal[ramp[addr+2]&0xFF]<<16);
#else
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&0xFF]|(thr.vpal[ramp[addr+1]&0xFF]<<16);
                                                        vidp[x+xx+1]=thr.vpal[ramp[addr+2]&0xFF]|(thr.vpal[ramp[addr+3]&0xFF]<<16);
#endif
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend)
                                           addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                int olddrawit=drawit;
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (drawit)
                                                {
                                                        yh=y+1;
                                                        if (yl==-1) yl=y;
                                                }
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-1))) drawit=1;
                                                if (drawit && !olddrawit) vidp=(uint32_t *)bmp_write_line(b,y);
                                        }
                                }
                                x=0;
                        }
                        thr.xsize<<=1;
  //                      rpclog("Yl %i Yh %i\n",yl,yh);
                        break;
                        case 4: /*16 bpp*/
                        thr.xsize>>=1;
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-1)))
                                {
                                        drawit=1;
//                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=4)
                                {
                                        if (drawit)
                                        {
                                                addr>>=1;
                                                vidp[x]=thr.pal16lookup[ramw[addr]]|(thr.pal16lookup[ramw[addr+1]]<<16);
                                                vidp[x+1]=thr.pal16lookup[ramw[addr+2]]|(thr.pal16lookup[ramw[addr+3]]<<16);
                                                vidp[x+2]=thr.pal16lookup[ramw[addr+4]]|(thr.pal16lookup[ramw[addr+5]]<<16);
                                                vidp[x+3]=thr.pal16lookup[ramw[addr+6]]|(thr.pal16lookup[ramw[addr+7]]<<16);
                                                addr<<=1;
                                                addr+=16;
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                int olddrawit=drawit;
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (drawit) 
                                                {
                                                        yh=y+1;
                                                        if (yl==-1) yl=y;
                                                }
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-1))) drawit=1;                                                
                                                if (drawit && !olddrawit) vidp=(uint32_t *)bmp_write_line(b,y);
                                                if ((addr>>12)==thr.lastblock && y>(oldcursorheight+oldcursory))
                                                   y=x=65536;
                                        }
                                }
                        }
                        thr.xsize<<=1;
                        break;
                        case 6: /*32 bpp*/
//                        textprintf(b,font,0,8,makecol(255,255,255),"%i %i %i %i  ",drawit,addr>>10,thr.xsize,thr.ysize);
//                        textprintf(screen,font,0,8,makecol(255,255,255),"%i %i %i %i  ",drawit,addr>>10,thr.xsize,thr.ysize);
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
//                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp16=(unsigned short *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=4)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<4;xx++)
                                                {
                                                        //VIDC20 pixel format is  xxxx xxxx BBBB BBBB GGGG GGGG RRRR RRRR
                                                        //Windows pixel format is                     RRRR RGGG GGGB BBBB
                                                        uint32_t temp=ramp[addr]|(ramp[addr+1]<<8)|(ramp[addr+2]<<16)|(ramp[addr+3]<<24);
                                                        vidp16[x+xx]=thr.pal[temp&0xFF].r|thr.pal[(temp>>8)&0xFF].g|thr.pal[(temp>>16)&0xFF].b;
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp16=(unsigned short *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
//                                                drawit=1;
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
//                        textprintf(b,font,0,16,makecol(255,255,255),"%i %i   ",yl,yh);
//                        textprintf(screen,font,0,16,makecol(255,255,255),"%i %i   ",yl,yh);
                        break;
                        default:
                        error("Bad BPP %i\n",thr.bpp);
                        exit(-1);
                }
                break;
                case 32:
                switch (thr.bpp)
                {
                        case 0: /*1 bpp*/
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=128)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<128;xx+=8)
                                                {
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&1];
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr]>>1)&1];
                                                        vidp[x+xx+2]=thr.vpal[(ramp[addr]>>2)&1];
                                                        vidp[x+xx+3]=thr.vpal[(ramp[addr]>>3)&1];
                                                        vidp[x+xx+4]=thr.vpal[(ramp[addr]>>4)&1];
                                                        vidp[x+xx+5]=thr.vpal[(ramp[addr]>>5)&1];
                                                        vidp[x+xx+6]=thr.vpal[(ramp[addr]>>6)&1];
                                                        vidp[x+xx+7]=thr.vpal[(ramp[addr]>>7)&1];
                                                        addr++;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        break;
                        case 1: /*2 bpp*/
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=64)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<64;xx+=4)
                                                {
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&3];
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr]>>2)&3];
                                                        vidp[x+xx+2]=thr.vpal[(ramp[addr]>>4)&3];
                                                        vidp[x+xx+3]=thr.vpal[(ramp[addr]>>6)&3];
                                                        addr++;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        break;
                        case 2: /*4 bpp*/
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=32)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<32;xx+=8)
                                                {
#ifdef _RPCEMU_BIG_ENDIAN
                                                        vidp[x+xx]=thr.vpal[ramp[addr+3]&0xF];
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr+3]>>4)&0xF];
                                                        vidp[x+xx+2]=thr.vpal[ramp[addr+2]&0xF];
                                                        vidp[x+xx+3]=thr.vpal[(ramp[addr+2]>>4)&0xF];
                                                        vidp[x+xx+4]=thr.vpal[ramp[addr+1]&0xF];
                                                        vidp[x+xx+5]=thr.vpal[(ramp[addr+1]>>4)&0xF];
                                                        vidp[x+xx+6]=thr.vpal[ramp[addr]&0xF];
                                                        vidp[x+xx+7]=thr.vpal[(ramp[addr]>>4)&0xF];
#else
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&0xF];
                                                        vidp[x+xx+1]=thr.vpal[(ramp[addr]>>4)&0xF];
                                                        vidp[x+xx+2]=thr.vpal[ramp[addr+1]&0xF];
                                                        vidp[x+xx+3]=thr.vpal[(ramp[addr+1]>>4)&0xF];
                                                        vidp[x+xx+4]=thr.vpal[ramp[addr+2]&0xF];
                                                        vidp[x+xx+5]=thr.vpal[(ramp[addr+2]>>4)&0xF];
                                                        vidp[x+xx+6]=thr.vpal[ramp[addr+3]&0xF];
                                                        vidp[x+xx+7]=thr.vpal[(ramp[addr+3]>>4)&0xF];
#endif
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        break;
                        case 3: /*8 bpp*/
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=16)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<16;xx+=4)
                                                {
                                                        vidp[x+xx]=thr.vpal[ramp[addr]&0xFF];
                                                        vidp[x+xx+1]=thr.vpal[ramp[addr+1]&0xFF];                                                        
                                                        vidp[x+xx+2]=thr.vpal[ramp[addr+2]&0xFF];                                                        
                                                        vidp[x+xx+3]=thr.vpal[ramp[addr+3]&0xFF];                                                                                                                
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        break;
                        case 4: /*16 bpp*/
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=8)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<8;xx+=2)
                                                {
                                                        unsigned short temp16;
                                                        /*VIDC20 format :                      xBBB BBGG GGGR RRRR
                                                          Windows format : xxxx xxxx RRRR RRRR GGGG GGGG BBBB BBBB*/
                                                        temp16=ramp[addr]|(ramp[addr+1]<<8);
                                                        vidp[x+xx]=thr.pal[temp16&0xFF].r|thr.pal[(temp16>>4)&0xFF].g|thr.pal[(temp16>>8)&0xFF].b;
                                                        temp16=ramp[addr+2]|(ramp[addr+3]<<8);
                                                        vidp[x+xx+1]=thr.pal[temp16&0xFF].r|thr.pal[(temp16>>4)&0xFF].g|thr.pal[(temp16>>8)&0xFF].b;
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        break;
                        case 6: /*32 bpp*/
                        for (y=0;y<thr.ysize;y++)
                        {
                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2)))
                                {
                                        drawit=1;
                                        yh=y+8;
                                        if (yl==-1)
                                           yl=y;
                                }
                                if (drawit) 
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y);
                                        yh=y+1;
                                }
                                for (x=0;x<thr.xsize;x+=4)
                                {
                                        if (drawit)
                                        {
                                                int xx;
                                                for (xx=0;xx<4;xx++)
                                                {
                                                        vidp[x+xx]=thr.pal[ramp[addr]].r|thr.pal[ramp[addr+1]].g|thr.pal[ramp[addr+2]].b;
                                                        addr+=4;
                                                }
                                        }
                                        else
                                           addr+=16;
                                        if (addr==(int)thr.iomd_vidend) addr=thr.iomd_vidstart;
                                        if (!(addr&0xFFF))
                                        {
                                                if (!drawit && thr.dirtybuffer[(addr>>12)]) vidp=(uint32_t *)bmp_write_line(b,y);
                                                drawit=thr.dirtybuffer[(addr>>12)];
                                                if (y<(oldcursorheight+oldcursory) && (y>=(oldcursory-2))) drawit=1;
                                                if (drawit) yh=y+8;
                                                if (yl==-1 && drawit)
                                                   yl=y;
                                        }
                                }
                        }
                        break;
                        default:
                        error("Bad BPP %i\n",thr.bpp);
                        exit(-1);
                }
        }
        if (thr.cursorheight>1)
        {
                if (cinit&0x4000000) ramp=(unsigned char *)ram2;
                else                 ramp=(unsigned char *)ram;
                addr=cinit&rammask;
//                printf("Mouse now at %i,%i\n",thr.cursorx,thr.cursory);
                switch (drawcode)
                {
                        case 16:
                        for (y=0;y<thr.cursorheight;y++)
                        {
                                if ((y+thr.cursory)>=thr.ysize) break;
                                if ((y+thr.cursory)>=0)
                                {
                                        vidp16=(unsigned short *)bmp_write_line(b,y+thr.cursory);
                                        for (x=0;x<32;x+=4)
                                        {
#ifdef _RPCEMU_BIG_ENDIAN
                                                addr^=3;
#endif
                                                if ((x+thr.cursorx)>=0   && ramp[addr]&3)      vidp16[x+thr.cursorx]=thr.vpal[(ramp[addr]&3)|0x100];
                                                if ((x+thr.cursorx+1)>=0 && (ramp[addr]>>2)&3) vidp16[x+thr.cursorx+1]=thr.vpal[((ramp[addr]>>2)&3)|0x100];
                                                if ((x+thr.cursorx+2)>=0 && (ramp[addr]>>4)&3) vidp16[x+thr.cursorx+2]=thr.vpal[((ramp[addr]>>4)&3)|0x100];
                                                if ((x+thr.cursorx+3)>=0 && (ramp[addr]>>6)&3) vidp16[x+thr.cursorx+3]=thr.vpal[((ramp[addr]>>6)&3)|0x100];
#ifdef _RPCEMU_BIG_ENDIAN
                                                addr^=3;
#endif
                                                addr++;
                                        }
                                }
                        }
                        break;
                        case 32:
                        for (y=0;y<thr.cursorheight;y++)
                        {
                                if ((y+thr.cursory)>=thr.ysize) break;
                                if ((y+thr.cursory)>=0)
                                {
                                        vidp=(uint32_t *)bmp_write_line(b,y+thr.cursory);
                                        for (x=0;x<32;x+=4)
                                        {
#ifdef _RPCEMU_BIG_ENDIAN
                                                addr^=3;
#endif
                                                if ((x+thr.cursorx)>=0   && ramp[addr]&3)      vidp[x+thr.cursorx]=thr.vpal[(ramp[addr]&3)|0x100];
                                                if ((x+thr.cursorx+1)>=0 && (ramp[addr]>>2)&3) vidp[x+thr.cursorx+1]=thr.vpal[((ramp[addr]>>2)&3)|0x100];
                                                if ((x+thr.cursorx+2)>=0 && (ramp[addr]>>4)&3) vidp[x+thr.cursorx+2]=thr.vpal[((ramp[addr]>>4)&3)|0x100];
                                                if ((x+thr.cursorx+3)>=0 && (ramp[addr]>>6)&3) vidp[x+thr.cursorx+3]=thr.vpal[((ramp[addr]>>6)&3)|0x100];
#ifdef _RPCEMU_BIG_ENDIAN
                                                addr^=3;
#endif
                                                addr++;
                                        }
                                }
                        }
                        break;
                }
                if (yl>thr.cursory) yl=thr.cursory;
                if (yl==-1) yl=thr.cursory;
                if (thr.cursory<0) yl=0;
                if (yh<(thr.cursorheight+thr.cursory)) yh=thr.cursorheight+thr.cursory;
        }
        oldcursorheight=thr.cursorheight;
        oldcursory=thr.cursory;


        bmp_unwrite_line(b); 
        
        /* Clean the dirtybuffer now we have updated eveything in it */
        memset(thr.dirtybuffer,0,512*4);
        thr.needvsync = 1;
//        rpclog("YL %i YH %i\n",yl,yh);
        if (yh>thr.ysize) yh=thr.ysize;
        if (yl==-1 && yh==-1) return;
        if (yl==-1) yl=0;
//        printf("Cursor %i %i %i\n",thr.cursorx,thr.cursory,thr.cursorheight);
//        rpclog("%i %02X\n",drawcode,bit8);        
//        rpclog("Blitting from 0,%i size %i,%i\n",yl,thr.xsize,thr.ysize);
        blitterthread(thr.xsize, thr.ysize, yl, yh, thr.doublesize);
}



void writevidc20(uint32_t val)
{
        float f;
//        rpclog("Write VIDC %08X %07X\n",val,PC);
        switch (val>>24)
        {
                case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
                case 8: case 9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE:
                        case 0xF: /* Video palette data */
//                printf("Write palette index %i %08X\n",palindex,val);
                if (val!=vidc.vidcpal[vidc.palindex])
                {
                        vidc.vidcpal[vidc.palindex]=val;
                        vidc.palchange=1;
                }
                vidc.palindex++;
                vidc.palindex&=255;
                break;
                case 0x10: /* Video palette address pointer */
                vidc.palindex=val&255;
                break;
                case 0x40: case 0x41: case 0x42: case 0x43:
                case 0x44: case 0x45: case 0x46: case 0x47:
                case 0x48: case 0x49: case 0x4A: case 0x4B:
                case 0x4C: case 0x4D: case 0x4E: case 0x4F:
                /* Border colour register */
                if (val!=vidc.vidcpal[0x100])
                {
                        vidc.palchange=1;
                        vidc.vidcpal[0x100]=val;
//                        rpclog("Change border colour %08X\n",val);
                }
//                rpclog("Border now %06X\n",val&0xFFFFFF);
                break;
                case 0x50: case 0x51: case 0x52: case 0x53:
                case 0x54: case 0x55: case 0x56: case 0x57:
                case 0x58: case 0x59: case 0x5A: case 0x5B:
                case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                /* Cursor palette (colour 1) */
                if (val!=vidc.vidcpal[0x101])
                {
                        vidc.vidcpal[0x101]=val;
                        vidc.palchange=1;
                        vidc.curchange=1;                        
                }
                break;
                case 0x60: case 0x61: case 0x62: case 0x63:
                case 0x64: case 0x65: case 0x66: case 0x67:
                case 0x68: case 0x69: case 0x6A: case 0x6B:
                case 0x6C: case 0x6D: case 0x6E: case 0x6F:
                /* Cursor palette (colour 2) */
                if (val!=vidc.vidcpal[0x102])
                {
                        vidc.vidcpal[0x102]=val;
                        vidc.palchange=1;                        
                        vidc.curchange=1;                        
                }
                break;
                case 0x70: case 0x71: case 0x72: case 0x73:
                case 0x74: case 0x75: case 0x76: case 0x77:
                case 0x78: case 0x79: case 0x7A: case 0x7B:
                case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                /* Cursor palette (colour 3) */
                if (val!=vidc.vidcpal[0x103])
                {
                        vidc.vidcpal[0x103]=val;
                        vidc.palchange=1;
                        vidc.curchange=1;
                }
                break;
                case 0x83: /* Horizontal Display Start Register */
                vidc.hdsr=val&0xFFE;
                break;
                case 0x84: /* Horizontal Display End Register */
                vidc.hder=val&0xFFE;
                break;
                case 0x86: /* Horizontal Cursor Start Register */
//                printf("HCSR write %03X\n",val&0xFFE);
                if (vidc.hcsr != (val&0xFFE)) vidc.curchange=1;
                vidc.hcsr=val&0xFFE;
                break;
                case 0x93: /* Vertical Display Start Register */
                vidc.vdsr=val&0xFFF;
                vidc.palchange=1;
                break;
                case 0x94: /* Vertical Display End Register */
                vidc.vder=val&0xFFF;
                vidc.palchange=1;
                break;
                case 0x96: /* Vertical Cursor Start Register */
//                printf("VCSR write %03X\n",val&0xFFF);
                if (vidc.vcsr != (val&0xFFF)) vidc.curchange=1;
                vidc.vcsr=val&0xFFF;
                break;
                case 0x97: /* Vertical Cursor End Register */
//                printf("VCER write %03X\n",val&0xFFF);
                if (vidc.vcer != (val&0xFFF)) vidc.curchange=1;
                vidc.vcer=val&0xFFF;
                break;
                case 0xB0: /* Sound Frequency Register */
                vidc.b0=val;
//                rpclog("Write B0 %08X %08X\n",val,PC);
                val=(vidc.b0&0xFF)+2;
                if (vidc.b1&1) f=(1000000.0f/(float)val)/4.0f;
                else      f=(705600.0f/(float)val)/4.0f;
                changesamplefreq((int)f);
//                rpclog("Sample rate : %i ns %f hz\n",val,f);
                break;
                case 0xB1: /* Sound Control Register */
//                rpclog("Write B1 %08X %08X\n",val,PC);
                vidc.b1=val;
                val=(vidc.b0&0xFF)+2;
                if (vidc.b1&1) f=(1000000.0f/(float)val)/4.0f;
                else      f=(705600.0f/(float)val)/4.0f;
                changesamplefreq((int)f);
//                rpclog("Sample rate : %i ns %f hz\n",val,f);
                break;
                case 0xE0: /* Control Register */
                if (((val>>5)&7)!=(uint32_t)vidc.bit8)
                {
//                        printf("Change mode - %08X %i\n",val,(val>>5)&7);
                        vidc.bit8=(val>>5)&7;
                        resetbuffer();
                        vidc.palchange=1;
                }
                break;
        }
}

void resetbuffer(void)
{
        memset(dirtybuffer,1,512*4);
//        rpclog("Reset buffer\n");
}
