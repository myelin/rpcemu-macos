/*RPCemu v0.6 by Tom Walker
  Main loop
  Should be platform independent*/
#include <assert.h>
#include <stdint.h>
#include <allegro.h>
#include "rpcemu.h"
#include "mem.h"
#include "vidc20.h"
#include "keyboard.h"
#include "sound.h"
#include "mem.h"
#include "iomd.h"
#include "ide.h"
#include "arm.h"
#include "cmos.h"
#include "superio.h"
#include "romload.h"
#include "cp15.h"
#include "cdrom-iso.h"
#include "podulerom.h"
#include "podules.h"
#include "fdc.h"

unsigned char flaglookup[16][16];

char discname[2][260]={"boot.adf","notboot.adf"};
char exname[512] = {0};

Config config = {
	CPUModel_ARM7500,	/* model */
	0,			/* rammask */
	0,			/* vrammask */
	0,			/* stretchmode */
	NULL,			/* username */
	NULL,			/* ipaddress */
	0,			/* refresh */
	1,			/* soundenabled */
	0,			/* skipblits (blit_optimisation) */
	1,			/* cdromenabled */
	0,			/* cdromtype  -- Only used on Windows build */
	"",			/* isoname */
	1,			/* mousehackon */
};

int infocus = 0;
int rinscount = 0;
int cyccount = 0;
int timetolive = 0;
int drawscre = 0;
int mousecapture = 0;
int quited = 0;

static void loadconfig(void);
static void saveconfig(void);

#ifdef _DEBUG
/**
 * UNIMPLEMENTEDFL
 *
 * Used to report sections of code that have not been implemented yet.
 * Do not use this function directly. Use the macro UNIMPLEMENTED() instead.
 *
 * @param file    File function is called from
 * @param line    Line function is called from
 * @param section Section code is missing from eg. "IOMD register" or
 *                "HostFS filecore message"
 * @param format  Section specific information
 * @param ...     Section specific information variable arguments
 */
void UNIMPLEMENTEDFL(const char *file, unsigned line, const char *section,
                     const char *format, ...)
{
	char buffer[1024];
	va_list arg_list;

	assert(file);
	assert(section);
	assert(format);

	va_start(arg_list, format);
	vsprintf(buffer, format, arg_list);
	va_end(arg_list);

	rpclog("UNIMPLEMENTED: %s: %s(%u): %s\n",
	       section, file, line, buffer);

	fprintf(stderr,
	        "UNIMPLEMENTED: %s: %s(%u): %s\n",
	        section, file, line, buffer);
}
#endif /* _DEBUG */

void resetrpc(void)
{
        mem_reset(config.rammask + 1);
        resetcp15();
        resetarm();
        resetkeyboard();
        resetiomd();
        reseti2c();
        resetide();
        superio_reset();
        resetpodules();
}

int startrpcemu(void)
{
        int c;
        char *p;
        get_executable_name(exname,511);
        p=get_filename(exname);
        *p=0;
        append_filename(HOSTFS_ROOT,exname,"hostfs",511);
        for (c=0;c<511;c++)
        {
                if (HOSTFS_ROOT[c]=='\\')
                   HOSTFS_ROOT[c]='/';
        }
        mem_init();
//printf("Mem inited...\n");
	loadroms();
//printf("ROMs loaded!\n");
        resetarm();
        resetfpa();
        resetiomd();
//printf("IOMD reset!\n");
        resetkeyboard();
//printf("Keyboard reset!\n");
        superio_reset();
//printf("SuperIO reset!\n");
        resetide();
//printf("IDE reset!\n");
        reseti2c();
//printf("i2C reset!\n");
        loadcmos();
        loadadf("boot.adf",0);
        loadadf("notboot.adf",1);
//printf("About to init video...\n");
        initvideo();
//printf("Video inited!\n");
        loadconfig();
        initsound();
        mem_reset(config.rammask + 1);
        initcodeblocks();
        iso_init();
        if (config.cdromtype == 2) /* ISO */
                iso_open(config.isoname);
        initpodules();
        initpodulerom();
        //initics();
//        iso_open("e:/au_cd8.iso");
//        config.cdromtype = CDROM_ISO;
        return 0;
}

void execrpcemu(void)
{
//	static int c;
//	printf("Exec %i\n",c);
//c++;
        execarm(20000);
        drawscr(drawscre);
        if (drawscre>0)
        {
//                rpclog("Drawscre %i\n",drawscre);
                drawscre--;
                if (drawscre>5) drawscre=0;
                
//				poll_keyboard();
//				poll_mouse();
                pollmouse();
//                sleep(0);
                doosmouse();
        }
//                pollmouse();
//                sleep(0);
//                doosmouse();
                pollkeyboard();
}

void endrpcemu(void)
{
        closevideo();
        endiomd();
        saveadf(discname[0], 0);
        saveadf(discname[1], 1);
        free(vram);
        free(ram);
        free(ram2);
        free(rom);
        savecmos();
        saveconfig();
}

static void loadconfig(void)
{
        char fn[512];
        const char *p;

        append_filename(fn,exname,"rpc.cfg",511);
        set_config_file(fn);
        p = get_config_string(NULL,"mem_size",NULL);
        if (!p)                    config.rammask = 0x7FFFFF;
        else if (!strcmp(p,"4"))   config.rammask = 0x1FFFFF;
        else if (!strcmp(p,"8"))   config.rammask = 0x3FFFFF;
        else if (!strcmp(p,"32"))  config.rammask = 0xFFFFFF;
        else if (!strcmp(p,"64"))  config.rammask = 0x1FFFFFF;
        else if (!strcmp(p,"128")) config.rammask = 0x3FFFFFF;
        else                       config.rammask = 0x7FFFFF;
        p = get_config_string(NULL,"vram_size",NULL);
        if (!p) config.vrammask = 0x7FFFFF;
        else if (!strcmp(p,"0"))   config.vrammask = 0;
        else                       config.vrammask = 0x7FFFFF;
        p = get_config_string(NULL,"cpu_type",NULL);
        if (!p) config.model = CPUModel_ARM710;
        else if (!strcmp(p, "ARM610"))  config.model = CPUModel_ARM610;
        else if (!strcmp(p, "ARM7500")) config.model = CPUModel_ARM7500;
        else if (!strcmp(p, "SA110"))   config.model = CPUModel_SA110;
        else                            config.model = CPUModel_ARM710;
        config.soundenabled = get_config_int(NULL, "sound_enabled", 1);
        config.stretchmode  = get_config_int(NULL, "stretch_mode",  0);
        config.refresh      = get_config_int(NULL, "refresh_rate", 60);
        config.skipblits    = get_config_int(NULL, "blit_optimisation", 0);
        config.cdromenabled = get_config_int(NULL, "cdrom_enabled", 0);
        config.cdromtype    = get_config_int(NULL, "cdrom_type", 0);
        p = get_config_string(NULL, "cdrom_iso", NULL);
        if (!p) strcpy(config.isoname, "");
        else    strcpy(config.isoname, p);
        config.mousehackon = get_config_int(NULL, "mouse_following", 1);
        config.username  = get_config_string(NULL, "username",  NULL);
        config.ipaddress = get_config_string(NULL, "ipaddress", NULL);
}

static void saveconfig(void)
{
        char s[256];

        sprintf(s, "%i", ((config.rammask + 1) >> 20) << 1);
        set_config_string(NULL,"mem_size",s);
        switch (config.model)
        {
                case CPUModel_ARM610:  sprintf(s, "ARM610"); break;
                case CPUModel_ARM710:  sprintf(s, "ARM710"); break;
                case CPUModel_SA110:   sprintf(s, "SA110"); break;
                case CPUModel_ARM7500: sprintf(s, "ARM7500"); break;
                default: fprintf(stderr, "saveconfig(): unknown cpu model %d\n", config.model); break;
        }
        set_config_string(NULL,"cpu_type",s);
        if (config.vrammask) set_config_string(NULL, "vram_size", "2");
        else                 set_config_string(NULL, "vram_size", "0");
        set_config_int(NULL, "sound_enabled",     config.soundenabled);
        set_config_int(NULL, "stretch_mode",      config.stretchmode);
        set_config_int(NULL, "refresh_rate",      config.refresh);
        set_config_int(NULL, "blit_optimisation", config.skipblits);
        set_config_int(NULL, "cdrom_enabled",     config.cdromenabled);
        set_config_int(NULL, "cdrom_type",        config.cdromtype);
        set_config_string(NULL, "cdrom_iso",      config.isoname);
        set_config_int(NULL, "mouse_following",   config.mousehackon);
}
