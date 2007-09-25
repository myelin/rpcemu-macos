/*RPCemu v0.6 by Tom Walker
  Main loop
  Not just for Linux - works as a Win32 console app as well*/

#include <allegro.h>
#include "rpcemu.h"
#include "mem.h"
#include "sound.h"
#include "vidc20.h"

#define MB_OK 1
static void MessageBox(void *param, const char *message, 
		       const char *title, int type) {
  printf("MessageBox: %s %s\n", title, message);
}



int mousecapture=0;
float mips;
int updatemips=0;

static uint32_t mipscount;
float mipstotal;

void wakeupsoundthread()
{
}

static void domips(void)
{
        mips=(float)inscount/1000000.0f;
	mipscount += 1;
	if (mipscount > 10)
	  mipstotal += mips;
        inscount=0;
        updatemips=1;
}

void error(const char *format, ...)
{
   char buf[256];

   va_list ap;
   va_start(ap, format);
   vsprintf(buf, format, ap);
   va_end(ap);
   MessageBox(NULL,buf,"RPCemu error",MB_OK);
}

FILE *arclog;
void rpclog(const char *format, ...)
{
   char buf[256];
//   return;
if (!arclog) arclog=fopen("rpclog.txt","wt");
   va_list ap;
   va_start(ap, format);
   vsprintf(buf, format, ap);
   va_end(ap);
   fputs(buf,arclog);
}

int drawscre=0,flyback;

int infocus;

void vblupdate()
{
        if (infocus) drawscre++;
}
uint32_t _oldx=-1,_oldy=-1;
void updatewindowsize(uint32_t x, uint32_t y)
{
if (x==_oldx && y==_oldy) return;
  printf("updatewindowsize: %i %i\n", x, y);
_oldx=x; _oldy=y;

                        if (set_gfx_mode(GRAPHICS_TYPE, x, y, 0, 0))
                        {
                                error("Failed to set gfx mode : %s\n",allegro_error);
                                endrpcemu();
                                exit(-1);
                        }
}

void releasemousecapture()
{
}

void resetrpc()
{
        memset(ram,0,rammask+1);
        resetcp15();
        resetarm();
        resetkeyboard();
        resetiomd();
        reseti2c();
        resetide();
        reset82c711();
}

int quited=0;
int blitrunning=1;

void *blitthread(void *threadid)
{
	blitrunning=1;
	while (!quited)
	{
		blitterthread();
		sleep(0);
	}
	blitrunning=0;
	pthread_exit(NULL);
}

pthread_t thread;
void endblitthread()
{
	quited=1;
	while (blitrunning) sleep(1);
}


void startblitthread()
{
	int c;
//return;
	c=pthread_create(&thread,NULL,blitthread,NULL);
	atexit(endblitthread);
}

void close_button_handler(void)
{
  quited = TRUE;
}
END_OF_FUNCTION(close_button_handler)

int main (void) 
{
        char s[128];
        const char *p;
        char fn[512];
//printf("Started!\n");
mousehackon=1;
        allegro_init();

	LOCK_FUNCTION(close_button_handler);
	set_close_button_callback(close_button_handler);
//printf("1\n");
//set_gfx_mode(GFX_AUTODETECT_WINDOWED,640,480,0,0);
//printf("2a\n");
        install_keyboard();
//poll_keyboard();
//printf("2\n");

        install_timer();
//printf("3\n");
        install_mouse();
//poll_mouse();
infocus=0;
//printf("Allegro inited...\n");
//        arclog=fopen("arclog.txt","wt");
        if (startrpcemu())
           return -1;
	//startblitthread();
//printf("RPCemu started...\n");
        install_int_ex(domips,MSEC_TO_TIMER(1000));
        install_int_ex(vblupdate,BPS_TO_TIMER(refresh));
        if (soundenabled) initsound();
        infocus=1;
mousehackon=1;
//printf("Ready to go!\n");
        while (!quited)
        {
                if (infocus)
                   execrpcemu();
                        if (updatemips)
                        {                           
			  printf("MIPS: %f (AVG: %f) %i\n", mips, mipstotal / (mipscount - 10),mousehack);
			        //sprintf(s,"RPCemu v0.3 - %f MIPS - %s",mips,(mousecapture)?"Press CTRL-END to release mouse":"Click to capture mouse");
				//                                SetWindowText(ghwnd, s);
                                updatemips=0;
                        }
                if ((key[KEY_LCONTROL] || key[KEY_RCONTROL]) && key[KEY_END]) entergui();
				if (key[KEY_Z] || key[KEY_QUOTE]) mouse_b|=4;
				if (key[KEY_X]) mouse_b|=2;
//				if (key[KEY_A] || key[KEY_S]) entergui();
                if ((key[KEY_LCONTROL] || key[KEY_RCONTROL]) && key[KEY_END] && mousecapture)
                {
		  //                        ClipCursor(&oldclip);
                        mousecapture=0;
                        updatemips=1;
                }
        }
        if (mousecapture)
        {
	  //                ClipCursor(&oldclip);
                mousecapture=0;
        }
        endrpcemu();
        return 0;
}

END_OF_MAIN();
