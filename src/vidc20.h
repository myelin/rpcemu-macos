#ifndef __VIDC20__
#define __VIDC20__

extern void initvideo(void);
extern void closevideo(void);
extern int getxs(void);
extern int getys(void);
extern void resetbuffer(void);
extern void writevidc20(uint32_t val);
extern void drawscr(int needredraw);
extern void togglefullscreen(int fs);
extern void vidcthread(void);

/* Platform specific functions */
extern void vidcstartthread(void);
extern void vidcendthread(void);
extern void vidcwakeupthread(void);
extern int vidctrymutex(void);
extern void vidcreleasemutex(void);

extern int fullscreen;

extern uint8_t *dirtybuffer;


#endif //__VIDC20__
