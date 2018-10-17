/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2005-2010 Sarah Walker

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "rpcemu.h"

int blockend;
#define STRONGARM

/*Preliminary FPA emulation. This works to an extent - !Draw works with it, !SICK
  seems to (FPA Whetstone scores are around 100x without), but !AMPlayer doesn't
  work, and GCC stuff tends to crash.*/
//#define FPA

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined __linux__ || defined __MACH__
#	include <unistd.h>
#	include <sys/mman.h>
#elif defined WIN32 || defined _WIN32
#	include <Windows.h>
#endif

#include "rpcemu.h"
#include "hostfs.h"
#include "keyboard.h"
#include "mem.h"
#include "iomd.h"
#include "ide.h"
#include "arm.h"
#include "cp15.h"
#include "fdc.h"

#if defined __amd64__
#	include "codegen_amd64.h"
#elif defined i386 || defined __i386 || defined __i386__ || defined _X86_
#	include "codegen_x86.h"
#else
#	error "Fatal error : no recompiler available for this architecture"
#endif

extern void removeblock(void); /* in codegen_*.c */
	
ARMState arm;

static int fdci=0;
static int cycles;
int prefabort;
uint32_t inscount;
uint32_t armirq = 0;
int cpsr;
uint32_t *pcpsr;

uint8_t flaglookup[16][16];

uint32_t *usrregs[16];
int databort;
int prog32;

#define NFSET	((arm.reg[cpsr] & NFLAG) ? 1u : 0)
#define ZFSET	((arm.reg[cpsr] & ZFLAG) ? 1u : 0)
#define CFSET	((arm.reg[cpsr] & CFLAG) ? 1u : 0)
#define VFSET	((arm.reg[cpsr] & VFLAG) ? 1u : 0)

#define GETADDR(r) ((r == 15) ? (arm.reg[15] & arm.r15_mask) : arm.reg[r])
#define LOADREG(r,v) if (r == 15) { arm.reg[15] = (arm.reg[15] & ~arm.r15_mask) | (((v) + 4) & arm.r15_mask); refillpipeline(); } else arm.reg[r] = (v);
#define GETREG(r) ((r == 15) ? (arm.reg[15] + arm.r15_diff) : arm.reg[r])

#define refillpipeline() blockend=1;

#include "arm_common.h"

uint32_t pccache;
static const uint32_t *pccache2;

/**
 * Return true if this ARM core is the dynarec version
 *
 * @return 1 yes this is dynarec
 */
int
arm_is_dynarec(void)
{
	return 1;
}

void updatemode(uint32_t m)
{
        uint32_t c, om = arm.mode;

        usrregs[15] = &arm.reg[15];
        switch (arm.mode & 0xf) { /* Store back registers */
            case USER:
            case SYSTEM: /* System (ARMv4) shares same bank as User mode */
                for (c=8;c<15;c++) arm.user_reg[c] = arm.reg[c];
                break;

            case IRQ:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.irq_reg[0] = arm.reg[13];
                arm.irq_reg[1] = arm.reg[14];
                break;

            case FIQ:
                for (c=8;c<15;c++) arm.fiq_reg[c] = arm.reg[c];
                break;

            case SUPERVISOR:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.super_reg[0] = arm.reg[13];
                arm.super_reg[1] = arm.reg[14];
                break;

            case ABORT:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.abort_reg[0] = arm.reg[13];
                arm.abort_reg[1] = arm.reg[14];
                break;

            case UNDEFINED:
                for (c=8;c<13;c++) arm.user_reg[c] = arm.reg[c];
                arm.undef_reg[0] = arm.reg[13];
                arm.undef_reg[1] = arm.reg[14];
                break;
        }
        arm.mode = m;

        switch (m&15)
        {
            case USER:
            case SYSTEM:
                for (c=8;c<15;c++) arm.reg[c] = arm.user_reg[c];
                for (c=0;c<15;c++) usrregs[c] = &arm.reg[c];
                break;

            case IRQ:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.irq_reg[0];
                arm.reg[14] = arm.irq_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;
            
            case FIQ:
                for (c=8;c<15;c++) arm.reg[c] = arm.fiq_reg[c];
                for (c=0;c<8;c++)  usrregs[c] = &arm.reg[c];
                for (c=8;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;

            case SUPERVISOR:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.super_reg[0];
                arm.reg[14] = arm.super_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;
            
            case ABORT:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.abort_reg[0];
                arm.reg[14] = arm.abort_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;

            case UNDEFINED:
                for (c=8;c<13;c++) arm.reg[c] = arm.user_reg[c];
                arm.reg[13] = arm.undef_reg[0];
                arm.reg[14] = arm.undef_reg[1];
                for (c=0;c<13;c++) usrregs[c] = &arm.reg[c];
                for (c=13;c<15;c++) usrregs[c] = &arm.user_reg[c];
                break;

            default:
                fatal("Bad mode %i\n", arm.mode);
        }

        if (ARM_MODE_32(arm.mode)) {
                arm.mmask = 0x1f;
                cpsr=16;
                pcpsr = &arm.reg[16];
                arm.r15_mask = 0xfffffffc;
                if (!ARM_MODE_32(om)) {
			/* Change from 26-bit to 32-bit mode */
                        arm.reg[16] = (arm.reg[15] & 0xf0000000) | arm.mode;
                        arm.reg[16] |= ((arm.reg[15] & 0xc000000) >> 20);
                        arm.reg[15] &= 0x3fffffc;
                }
        }
        else
        {
                arm.mmask = 3;
                cpsr=15;
                pcpsr = &arm.reg[15];
                arm.r15_mask = 0x3fffffc;
                arm.reg[16] = (arm.reg[16] & 0xffffffe0) | arm.mode;
                if (ARM_MODE_32(om)) {
                        arm.reg[15] &= arm.r15_mask;
                        arm.reg[15] |= (arm.mode & 3);
                        arm.reg[15] |= (arm.reg[16] & 0xf0000000);
                        arm.reg[15] |= ((arm.reg[16] & 0xc0) << 20);
                }
        }

	/* Update memory access mode based on privilege level of ARM mode */
	memmode = ARM_MODE_PRIV(arm.mode) ? 1 : 0;
}

static int stmlookup[256];

int countbitstable[65536];

void
arm_init(void)
{
	unsigned c, d, exec;

	for (c = 0; c < 256; c++) {
		stmlookup[c] = 0;
		for (d = 0; d < 8; d++) {
			if (c & (1u << d)) {
				stmlookup[c] += 4;
			}
		}
	}
	for (c = 0; c < 65536; c++) {
		countbitstable[c] = 0;
		for (d = 0; d < 16; d++) {
			if (c & (1u << d)) {
				countbitstable[c] += 4;
			}
		}
	}

	cpsr = 15;
	for (c = 0; c < 16; c++) {
		for (d = 0; d < 16; d++) {
			arm.reg[15] = d << 28;
			switch (c) {
			case 0:  /* EQ */ exec = ZFSET; break;
			case 1:  /* NE */ exec = !ZFSET; break;
			case 2:  /* CS */ exec = CFSET; break;
			case 3:  /* CC */ exec = !CFSET; break;
			case 4:  /* MI */ exec = NFSET; break;
			case 5:  /* PL */ exec = !NFSET; break;
			case 6:  /* VS */ exec = VFSET; break;
			case 7:  /* VC */ exec = !VFSET; break;
			case 8:  /* HI */ exec = (CFSET && !ZFSET); break;
			case 9:  /* LS */ exec = (!CFSET || ZFSET); break;
			case 10: /* GE */ exec = (NFSET == VFSET); break;
			case 11: /* LT */ exec = (NFSET != VFSET); break;
			case 12: /* GT */ exec = (!ZFSET && (NFSET == VFSET)); break;
			case 13: /* LE */ exec = (ZFSET || (NFSET != VFSET)); break;
			case 14: /* AL */ exec = 1; break;
			case 15: /* NV */ exec = 0; break;
			}
			flaglookup[c][d] = (uint8_t) exec;
		}
	}
}

void
resetarm(CPUModel cpu_model)
{
//        atexit(dumpregs);

	memset(&arm, 0, sizeof(arm));

        arm.r15_mask = 0x3fffffc;
        pccache=0xFFFFFFFF;
        updatemode(SUPERVISOR);
        cpsr=15;
//        prog32=1;

        arm.reg[15] = 0x0c000008 | 3;
        arm.reg[16] = SUPERVISOR | 0xd0;
        arm.mode = SUPERVISOR;
        pccache=0xFFFFFFFF;
	if (cpu_model == CPUModel_SA110 || cpu_model == CPUModel_ARM810) {
		arm.r15_diff = 0;
		arm.abort_base_restored = 1;
		arm.stm_writeback_at_end = 1;
	} else {
		arm.r15_diff = 4;
		arm.abort_base_restored = 0;
		arm.stm_writeback_at_end = 0;
	}

	cycles = 0;
}

void dumpregs(void)
{
        char s[1024];

        sprintf(s, "R 0=%08X R 4=%08X R 8=%08X R12=%08X\n"
                   "R 1=%08X R 5=%08X R 9=%08X R13=%08X\n"
                   "R 2=%08X R 6=%08X R10=%08X R14=%08X\n"
                   "R 3=%08X R 7=%08X R11=%08X R15=%08X\n"
                   "%s\n",
                   arm.reg[0], arm.reg[4], arm.reg[8], arm.reg[12],
                   arm.reg[1], arm.reg[5], arm.reg[9], arm.reg[13],
                   arm.reg[2], arm.reg[6], arm.reg[10], arm.reg[14],
                   arm.reg[3], arm.reg[7], arm.reg[11], arm.reg[15],
                   mmu ? "MMU enabled" : "MMU disabled");
        rpclog("%s",s);
        printf("%s",s);

        memmode=1;
}

#define dumpregs()

static uint32_t
shift3(uint32_t opcode)
{
	uint32_t shiftmode = opcode & 0x60;
	uint32_t shiftamount;
	uint32_t temp;
	uint32_t cflag = CFSET;

	if (opcode & 0x10) {
		shiftamount = arm.reg[(opcode >> 8) & 0xf] & 0xff;
	} else {
		shiftamount = (opcode >> 7) & 0x1f;
	}
	temp = arm.reg[RM];
	if (shiftamount != 0) {
		arm.reg[cpsr] &= ~CFLAG;
	}
	switch (shiftmode) {
	case 0: /* LSL */
		if (shiftamount == 0) {
			return temp;
		}
		if (shiftamount == 32) {
			if (temp & 1) {
				arm.reg[cpsr] |= CFLAG;
			}
			return 0;
		}
		if (shiftamount > 32) {
			return 0;
		}
		if ((temp << (shiftamount - 1)) & 0x80000000) {
			arm.reg[cpsr] |= CFLAG;
		}
		return temp << shiftamount;

	case 0x20: /* LSR */
		if (shiftamount == 0 && !(opcode & 0x10)) {
			shiftamount = 32;
		}
		if (shiftamount == 0) {
			return temp;
		}
		if (shiftamount == 32) {
			if (temp & 0x80000000) {
				arm.reg[cpsr] |= CFLAG;
			} else {
				arm.reg[cpsr] &= ~CFLAG;
			}
			return 0;
		}
		if (shiftamount > 32) {
			return 0;
		}
		if ((temp >> (shiftamount - 1)) & 1) {
			arm.reg[cpsr] |= CFLAG;
		}
		return temp >> shiftamount;

	case 0x40: /* ASR */
		if (shiftamount == 0) {
			if (opcode & 0x10) {
				return temp;
			}
		}
		if (shiftamount >= 32 || shiftamount == 0) {
			if (temp & 0x80000000) {
				arm.reg[cpsr] |= CFLAG;
			} else {
				arm.reg[cpsr] &= ~CFLAG;
			}
			if (temp & 0x80000000) {
				return 0xffffffff;
			}
			return 0;
		}
		if (((int32_t) temp >> (shiftamount - 1)) & 1) {
			arm.reg[cpsr] |= CFLAG;
		}
		return (uint32_t) ((int32_t) temp >> shiftamount);

	default: /* ROR */
		arm.reg[cpsr] &= ~CFLAG;
		if (shiftamount == 0 && !(opcode & 0x10)) {
			/* RRX */
			if (temp & 1) {
				arm.reg[cpsr] |= CFLAG;
			}
			return (cflag << 31) | (temp >> 1);
		}
		if (shiftamount == 0) {
			arm.reg[cpsr] |= (cflag << 29);
			return temp;
		}
		if ((shiftamount & 0x1f) == 0) {
			if (temp & 0x80000000) {
				arm.reg[cpsr] |= CFLAG;
			}
			return temp;
		}
		shiftamount &= 0x1f;
		if (((temp >> shiftamount) | (temp << (32 - shiftamount))) & 0x80000000) {
			arm.reg[cpsr] |= CFLAG;
		}
		return (temp >> shiftamount) | (temp << (32 - shiftamount));
	}
}

#define shift(o)  ((o & 0xff0) ? shift3(o) : arm.reg[RM])
#define shift2(o) ((o & 0xff0) ? shift4(o) : arm.reg[RM])
#define shift_ldrstr(o) shift2(o)

static uint32_t
shift5(uint32_t opcode, uint32_t shiftmode, uint32_t shiftamount, uint32_t rm)
{
	switch (shiftmode) {
	case 0: /* LSL */
		if (shiftamount == 0) {
			return rm;
		}
		return 0; /* shiftamount >= 32 */

	case 0x20: /* LSR */
		if (shiftamount == 0 && (opcode & 0x10)) {
			return rm;
		}
		return 0; /* shiftamount >= 32 */

	case 0x40: /* ASR */
		if (shiftamount == 0 && !(opcode & 0x10)) {
			shiftamount = 32;
		}
		if (shiftamount >= 32) {
			if (rm & 0x80000000) {
				return 0xffffffff;
			}
			return 0;
		}
		return (uint32_t) ((int32_t) rm >> shiftamount);

	default: /* ROR */
		if (!(opcode & 0x10)) {
			/* RRX */
			return (CFSET << 31) | (rm >> 1);
		}
		shiftamount &= 0x1f;
		return (rm >> shiftamount) | (rm << (32 - shiftamount));
	}
}

static inline uint32_t
shift4(uint32_t opcode)
{
	uint32_t shiftmode = opcode & 0x60;
	uint32_t shiftamount;
	uint32_t rm = arm.reg[RM];

	if (opcode & 0x10) {
		shiftamount = arm.reg[(opcode >> 8) & 0xf] & 0xff;
	} else {
		shiftamount = (opcode >> 7) & 0x1f;
	}

	if ((shiftamount - 1) >= 31) {
		return shift5(opcode, shiftmode, shiftamount, rm);
	}

	switch (shiftmode) {
	case 0: /* LSL */
		return rm << shiftamount;
	case 0x20: /* LSR */
		return rm >> shiftamount;
	case 0x40: /* ASR */
		return (uint32_t) ((int32_t) rm >> shiftamount);
	default: /* ROR */
		return (rm >> shiftamount) | (rm << (32 - shiftamount));
	}
}

#define undefined() exception(UNDEFINED,8,4)

static void bad_opcode(uint32_t opcode) 
{
     error("Bad opcode %02X %08X at %07X\n",(opcode >> 20) & 0xFF, opcode, PC);
     dumpregs();
     exit(EXIT_FAILURE);
}

void
exception(uint32_t mmode, uint32_t address, uint32_t diff)
{
	uint32_t link;
	uint32_t irq_disable;

	/* If FIQ exception, disable FIQ and IRQ, otherwise disable just IRQ */
	if (mmode == FIQ) {
		irq_disable = (0x80 | 0x40);
	} else {
		irq_disable = 0x80;
	}

	link = arm.reg[15] - diff;

	if (ARM_MODE_32(arm.mode)) {
		arm.spsr[mmode] = arm.reg[16];
		updatemode(0x10 | mmode);
		arm.reg[14] = link;
		arm.reg[16] &= ~0x1fu;
		arm.reg[16] |= 0x10 | mmode | irq_disable;
		arm.reg[15] = address;
	} else if (prog32) {
		updatemode(0x10 | mmode);
		arm.reg[14] = link & 0x3fffffc;
		arm.spsr[mmode] = (arm.reg[16] & ~0x1fu) | (link & 3);
		arm.spsr[mmode] &= ~0x10u;
		arm.reg[16] |= irq_disable;
		arm.reg[15] = address;
	} else {
		arm.reg[15] |= 3;
		/* When in 26-bit config, Abort and Undefined exceptions enter
		   mode SVC_26 */
		updatemode(mmode >= SUPERVISOR ? SUPERVISOR : mmode);
		arm.reg[14] = link;
		arm.reg[15] &= 0xfc000003;
		arm.reg[15] |= ((irq_disable << 20) | address);
	}
	refillpipeline();
}

#if defined __linux__ || defined __MACH__
/**
 * Grant executable privilege to a region of memory (Unix)
 *
 * @param ptr Pointer to region of memory
 * @param len Length of region of memory
 */
void
set_memory_executable(void *ptr, size_t len)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	const long page_mask = ~(page_size - 1);
	void *start;
	long end;

	start = (void *) ((long) ptr & page_mask);
	end = ((long) ptr + len + page_size - 1) & page_mask;
	len = (size_t) (end - (long) start);

	if (mprotect(start, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		perror("mprotect");
		exit(1);
	}
}

#elif defined WIN32 || defined _WIN32
/**
 * Grant executable privilege to a region of memory (Windows)
 *
 * @param ptr Pointer to region of memory
 * @param len Length of region of memory
 */
void
set_memory_executable(void *ptr, size_t len)
{
	DWORD old_protect;

	if (!VirtualProtect(ptr, len, PAGE_EXECUTE_READWRITE, &old_protect)) {
		fprintf(stderr, "VirtualProtect() failed: error code 0x%lx\n", GetLastError());
		exit(1);
	}
}

#else
/**
 * Stub implementation for when another implementation does not apply.
 */
void
set_memory_executable(void *ptr, size_t len)
{
	NOT_USED(ptr);
	NOT_USED(len);
}
#endif

#include "ArmDynarecOps.h"

static void badopcode(uint32_t opcode)
{
        bad_opcode(opcode);
        exit(-1);
}

static const unsigned char validforskip[64]=
{
        1,        1,        1,        1,        1,       1,        1,       1,
        1,        1,        1,        1,        1,       1,        1,       1,
        0,        0,        0,        0,        0,       0,        0,       0,
        1,        1,        0,        0,        1,       1,        1,       1,
        
        0,        0,        1,        0,        1,       0,        1,       0,
        1,        0,        1,        0,        1,       0,        1,       0,
        0,        0,        0,        0,        0,       0,        0,       0,
        1,        0,        1,        0,        1,       0,        1,       0
};

static const OpFn opcodes[256]=
{
	opANDreg, opANDregS,opEORreg, opEORregS,opSUBreg,opSUBregS,opRSBreg,opRSBregS,   //00
	opADDreg, opADDregS,opADCreg, opADCregS,opSBCreg,opSBCregS,opRSCreg,opRSCregS,   //08
	(OpFn)opSWPword,opTSTreg, opMSRcreg,opTEQreg, (OpFn)opSWPbyte,opCMPreg,opMSRsreg,opCMNreg,   //10
	opORRreg, opORRregS,opMOVreg, opMOVregS,opBICreg,opBICregS,opMVNreg,opMVNregS,   //18

	opANDimm, opANDimmS,opEORimm, opEORimmS,opSUBimm, opSUBimmS,opRSBimm, opRSBimmS, //20
	opADDimm, opADDimmS,opADCimm, opADCimmS,opSBCimm, opSBCimmS,opRSCimm, opRSCimmS, //28
	badopcode,opTSTimm, opMSRcimm,opTEQimm, badopcode,opCMPimm, badopcode,opCMNimm,  //30
	opORRimm, opORRimmS,opMOVimm, opMOVimmS,opBICimm, opBICimmS,opMVNimm, opMVNimmS, //38

	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRT,   (OpFn)opLDRT,   (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRBT,  (OpFn)opLDRBT,   //40
	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRT,   (OpFn)opLDRT,   (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRBT,  (OpFn)opLDRBT,   //48
	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRB,   (OpFn)opLDRB,    //50
	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRB,   (OpFn)opLDRB,    //58

	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRT,   (OpFn)opLDRT,   (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRBT,  (OpFn)opLDRBT,   //60
	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRT,   (OpFn)opLDRT,   (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRBT,  (OpFn)opLDRBT,   //68
	(OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRB,   (OpFn)opLDRB,    //70
        (OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTR,    (OpFn)opLDR,    (OpFn)opSTRB,   (OpFn)opLDRB,   (OpFn)opSTRB,   (OpFn)opLDRB,    //78

	(OpFn)opSTMD,   (OpFn)opLDMD,   (OpFn)opSTMD,   (OpFn)opLDMD,   (OpFn)opSTMDS,  (OpFn)opLDMDS,  (OpFn)opSTMDS,  (OpFn)opLDMDS,   //80
	(OpFn)opSTMI,   (OpFn)opLDMI,   (OpFn)opSTMI,   (OpFn)opLDMI,   (OpFn)opSTMIS,  (OpFn)opLDMIS,  (OpFn)opSTMIS,  (OpFn)opLDMIS,   //88
	(OpFn)opSTMD,   (OpFn)opLDMD,   (OpFn)opSTMD,   (OpFn)opLDMD,   (OpFn)opSTMDS,  (OpFn)opLDMDS,  (OpFn)opSTMDS,  (OpFn)opLDMDS,   //90
	(OpFn)opSTMI,   (OpFn)opLDMI,   (OpFn)opSTMI,   (OpFn)opLDMI,   (OpFn)opSTMIS,  (OpFn)opLDMIS,  (OpFn)opSTMIS,  (OpFn)opLDMIS,   //98

	opB,	  opB,	    opB,      opB,      opB,      opB,      opB,      opB,       //A0
	opB,	  opB,	    opB,      opB,      opB,      opB,      opB,      opB,       //A8
	opBL,	  opBL,	    opBL,     opBL,     opBL,     opBL,     opBL,     opBL,      //B0
	opBL,	  opBL,	    opBL,     opBL,     opBL,     opBL,     opBL,     opBL,      //B8

	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   //C0
	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   //C8
	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   //D0
	opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,  opcopro,   //D8

	opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,     //E0
	opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,    opMCR,    opMRC,     //E8
	opSWI,	  opSWI,    opSWI,    opSWI,	opSWI,	  opSWI,    opSWI,    opSWI,     //F0
	opSWI,	  opSWI,    opSWI,    opSWI,	opSWI,	  opSWI,    opSWI,    opSWI      //F8
};

int linecyc=0;

static inline int
arm_opcode_may_abort(uint32_t opcode)
{
	/* Is this a single or multiple data transfer? */
	if (((opcode + 0x6000000) & 0xf000000) >= 0xa000000) {
		return 1;
	}
	/* Is this a swap? */
	if ((opcode & 0x0fb000f0) == 0x01000090) {
		return 1;
	}
	return 0;
}

void
execarm(int cycs)
{
	int hash;
	void (*gen_func)(void);
	uint32_t opcode;
	uint32_t templ;

	cycles += cycs;
	linecyc = 256;
	while (cycles > 0) {
		// cyccount+=200;
		while (linecyc-- >= 0) {
			armirq &= ~0xc0u;
			if (!isblockvalid(PC)) {
				/* Interpret block */
				blockend = 0;
				if ((PC >> 12) != pccache) {
					pccache2 = getpccache(PC);
					if (pccache2 == NULL) {
						pccache = 0xffffffff;
						armirq |= 0x80;
					} else {
						opcode = pccache2[PC >> 2];
						pccache = PC >> 12;
					}
				}
				while (!blockend && !(armirq & 0xc0)) {
					opcode = pccache2[PC >> 2];
					if ((opcode & 0x0e000000) == 0x0a000000) { blockend = 1; } /* Always end block on branches */
					if ((opcode & 0x0c000000) == 0x0c000000) { blockend = 1; } /* And SWIs and copro stuff */
					if (!(opcode & 0x0c000000) && (RD == 15)) { blockend = 1; } /* End if R15 can be modified */
					if ((opcode & 0x0e108000) == 0x08108000) { blockend = 1; } /* End if R15 reloaded from LDM */
					if ((opcode & 0x0c100000) == 0x04100000 && (RD == 15)) { blockend = 1; } /* End if R15 reloaded from LDR */
					if (flaglookup[opcode >> 28][(*pcpsr) >> 28]) {// && !(armirq&0x80))
						opcodes[(opcode >> 20) & 0xff](opcode);
					}
					// if ((opcode & 0x0e000000) == 0x0a000000) blockend = 1; /* Always end block on branches */
					// if ((opcode & 0x0c000000) == 0x0c000000) blockend = 1; /* And SWIs and copro stuff */
					arm.reg[15] += 4;
					if ((PC & 0xffc) == 0) {
						blockend = 1;
					}
					// if (armirq) blockend=1;
					inscount++;
				}
			} else {
				hash = HASH(PC);
				/* if (pagedirty[PC>>9])
				{
					pagedirty[PC>>9]=0;
					cacheclearpage(PC>>9);
				}
				else */ if (codeblockpc[hash] == PC) {
					templ = codeblocknum[hash];
					gen_func = (void *) (&rcodeblock[templ][BLOCKSTART]);
					// gen_func=(void *)(&codeblock[blocks[templ]>>24][blocks[templ]&0xFFF][4]);
					gen_func();
					if (armirq & 0x40) {
						arm.reg[15] += 4;
					}
					if ((arm.reg[cpsr] & arm.mmask) != arm.mode) {
						updatemode(arm.reg[cpsr] & arm.mmask);
					}
				} else {
					blockend = 0;
					/* Initialise 'opcode' to invalid value */
					opcode = 0xffffffff;
					if ((PC >> 12) != pccache) {
						pccache2 = getpccache(PC);
						if (pccache2 == NULL) {
							pccache = 0xffffffff;
							armirq |= 0x80;
						} else {
							opcode = pccache2[PC >> 2];
							pccache = PC >> 12;
						}
					}
					if (!(armirq & 0x80)) {
						initcodeblock(PC);
						//printf("New block %08X %04X %08X\n",PC,hash,codeblockpc[hash]);
						//codeblockpc[hash]=PC;
					}
					while (!blockend && !(armirq & 0xc0)) {
						opcode = pccache2[PC >> 2];
						if ((opcode >> 28) == 0xf) {
							/* NV */
							generatepcinc();
						} else {
#ifdef ABORTCHECKING
							generateupdatepc();
#else
							if ((opcode & 0x0e000000) == 0x00000000/* && (RN==15 || RD==15 || RM==15 || !validforskip[(opcode>>20)&63])*/) generateupdatepc();
							if ((opcode & 0x0e000000) == 0x02000000/* && (RN==15 || RD==15 ||           !validforskip[(opcode>>20)&63])*/) generateupdatepc();
							if ((opcode & 0x0c000000) == 0x04000000 && (RN == 15 || RD == 15 || RM == 15)) generateupdatepc();
							if ((opcode & 0x0e000000) == 0x08000000 && ((opcode & 0x8000) || (RN == 15))) generateupdatepc();
							if ((opcode & 0x0f000000) >= 0x0a000000) generateupdatepc();
#endif
							// if (((opcode + 0x06000000) & 0x0f000000) >= 0x0a000000) generateupdatepc();
							// generateupdatepc();
							generatepcinc();
							if ((opcode & 0x0e000000) == 0x0a000000) {
								generateupdateinscount();
							}
							if ((opcode >> 28) != 0xe) {
								generateflagtestandbranch(opcode, pcpsr);//,flaglookup);
							} else {
								lastflagchange = 0;
							}
							generatecall(opcodes[(opcode >> 20) & 0xff], opcode, pcpsr);
#ifdef ABORTCHECKING
							if (arm_opcode_may_abort(opcode)) {
								generateirqtest();
							}
#endif
							// if ((opcode & 0x0e000000) == 0x0a000000) blockend = 1; /* Always end block on branches */
							if ((opcode & 0x0c000000) == 0x0c000000) blockend = 1; /* And SWIs and copro stuff */
							if (!(opcode & 0x0c000000) && (RD == 15)) blockend = 1; /* End if R15 can be modified */
							if ((opcode & 0x0e108000) == 0x08108000) blockend = 1; /* End if R15 reloaded from LDM */
							if ((opcode & 0x0c100000) == 0x04100000 && (RD == 15)) blockend=1; /* End if R15 reloaded from LDR */
							if (flaglookup[opcode >> 28][(*pcpsr) >> 28]) { // && !(armirq&0x80))
								opcodes[(opcode >> 20) & 0xff](opcode);
							}
						}
						arm.reg[15] += 4;
						if ((PC & 0xffc) == 0) {
							blockend = 1;
						}
						// blockend = 1;
						// inscount++;
					}
					if (!(armirq & 0x80)) {
						endblock(opcode);
					} else {
						removeblock();
					}
				}
			}

			if (/*databort|*/armirq & 0xc3)//|prefabort)
			{
				if (!ARM_MODE_32(arm.mode)) {
					arm.reg[16] &= ~0xc0;
					arm.reg[16] |= ((arm.reg[15] & 0xc000000) >> 20);
				}

				if (armirq & 0xc0) {
					if (armirq & 0x80) { //prefabort)
						/* Prefetch Abort */
						arm.reg[15] -= 4;
						exception(ABORT, 0x10, 4);
						arm.reg[15] += 4;
						armirq &= ~0xc0u;
					} else if (armirq & 0x40) { //databort==1)
						/* Data Abort */
						arm.reg[15] -= 4;
						exception(ABORT, 0x14, 0);
						arm.reg[15] += 4;
						armirq &= ~0xc0u;
					}
				} else if ((armirq & 2) && !(arm.reg[16] & 0x40)) {
					/* FIQ */
					arm.reg[15] -= 4;
					exception(FIQ, 0x20, 0);
					arm.reg[15] += 4;
				} else if ((armirq & 1) && !(arm.reg[16] & 0x80)) {
					/* IRQ */
					arm.reg[15] -= 4;
					exception(IRQ, 0x1c, 0);
					arm.reg[15] += 4;
				}
			}
			// armirq=(armirq&0xCC)|((armirq>>2)&3);
		}
		linecyc += 256;

		if (kcallback) {
			kcallback--;
			if (kcallback <= 0) {
				kcallback = 0;
				keyboard_callback_rpcemu();
			}
		}
		if (mcallback) {
			mcallback -= 10;
			if (mcallback <= 0) {
				mcallback = 0;
				mouse_ps2_callback();
			}
		}
		if (fdccallback) {
			fdccallback -= 100;
			if (fdccallback <= 0) {
				fdccallback = 0;
				fdc_callback();
			}
		}
		if (idecallback) {
			idecallback -= 10;
			if (idecallback <= 0) {
				idecallback = 0;
				callbackide();
			}
		}
		if (motoron) {
			fdci--;
			if (fdci <= 0) {
				fdci = 20000;
				iomd.irqa.status |= IOMD_IRQA_FLOPPY_INDEX;
				updateirqs();
			}
		}
		cycles -= 1000;
	}
}
