/*RPCemu v0.6 by Tom Walker
  FPA emulation
  Not enabled by default due to bugs*/

#include <math.h>
#include "rpcemu.h"

#define UNDEFINED  11
#define undefined() exception(UNDEFINED,8,4)
//unsigned long oldpc,oldpc2,oldpc3;
double fparegs[8]; /*No C variable type for 80-bit floating point, so use 64*/
uint32_t fpsr,fpcr;

void dumpfpa()
{
        rpclog("\nF0=%f F1=%f F2=%f F3=%f\n",fparegs[0],fparegs[1],fparegs[2],fparegs[3]);
        rpclog("F4=%f F5=%f F6=%f F7=%f\n",fparegs[0],fparegs[1],fparegs[2],fparegs[3]);
        rpclog("FPSR=%08X FPCR=%08X\n",fpsr,fpcr);
}

void resetfpa()
{
        uint32_t temp[3];
        float *tfs;
        double tf;
        tfs=(float *)temp;
        *tfs=0.12f;
        tf=(double)(*tfs);
        rpclog("Double size %i Float size %i %f %f\n",sizeof(double),sizeof(float),*tfs,tf);
//        fpsr=0;
        fpsr=0x81000000; /*FPA system*/
        fpcr=0;
        atexit(dumpfpa);
}

#define FD ((opcode>>12)&7)
#define FN ((opcode>>16)&7)
#define RD ((opcode>>12)&0xF)
#define RN ((opcode>>16)&0xF)
#define RM (opcode&0xF)

#define GETADDR(r) ((r==15)?(armregs[15]&r15mask):armregs[r])

#define NFLAG 0x80000000
#define ZFLAG 0x40000000
#define CFLAG 0x20000000
#define VFLAG 0x10000000

static inline void setsubf(double op1, double op2)
{
        armregs[cpsr]&=0xFFFFFFF;
        if (op1==op2) armregs[cpsr]|=ZFLAG;
        if (op1< op2) armregs[cpsr]|=NFLAG;
        if (op1>=op2) armregs[cpsr]|=CFLAG;
//        if ((op1^op2)&(op1^res)&0x80000000) armregs[cpsr]|=VFLAG;
}
int times8000;
double fconstants[8]={0.0f,1.0f,2.0f,3.0f,4.0f,5.0f,0.5f,10.0f};
/*Instruction types :
  Opcodes Cx/Dx, CP1 - LDF/STF
  Opcodes Cx/Dx, CP2 - LFM/SFM
  Opcodes Ex, bit 4 clear - Data processing
  Opcodes Ex, bit 4 set   - Register transfer
  Opcodex Ex, bit 4 set, RD=15 - Compare*/
void fpaopcode(uint32_t opcode)
{
        uint32_t temp[6];
        double *tf,*tf2;
        float *tfs;
        double tempf;
        int len;
        uint32_t addr;
        tf=(double *)temp;
        tf2=(double *)&temp[4];
        tfs=(float *)temp;
//        rpclog("FPA op %08X %08X %f %i %i\n",opcode,PC,fparegs[1],times8000,ins);
        switch ((opcode>>24)&0xF)
        {
                case 0xC: case 0xD:
                if (opcode&0x100) /*LDF/STF*/
                {
                        addr=GETADDR(RN);
                        if (opcode&0x1000000)
                        {
                                if (opcode&0x800000) addr+=((opcode&0xFF)<<2);
                                else                 addr-=((opcode&0xFF)<<2);
                        }
                        switch (opcode&0x408000)
                        {
                                case 0x000000: /*Single*/
                                *tfs=(float)fparegs[FD];
                                temp[1]=temp[2]=0;
                                len=1;
//                                if (!(opcode&0x100000)) rpclog("Storing %08X %08X %08X %08X %f %f\n",addr,temp[0],temp[1],temp[2],fparegs[FD],*tfs);
                                break;
                                case 0x008000: /*Double*/
                                *tf=fparegs[FD];
                                temp[2]=0;
                                len=2;
                                break;
                                case 0x400000: /*Long*/
                                *tf2=fparegs[FD];
                                temp[0]=temp[5]&0x80000000;
                                temp[0]|=(temp[5]&0x7FFF0000)>>16;
                                temp[1]=(temp[5]&0xFFFFF)<<11;
                                temp[1]|=((temp[4]>>21)&0x7FF);
                                temp[2]=temp[4]<<11;
                                if (temp[0]&0x7FFF) temp[1]|=0x80000000;
                                len=3;
                                break;
                                default:
/*                                armregs[15]+=8;
                                undefined();
                                return;*/
                                error("Bad LDF/STF size %08X %08X\n",opcode&0x408000,opcode);
                                dumpregs();
                                exit(-1);
                        }
//                        rpclog("Address %07X len %i\n",addr,len);
                        if (opcode&0x100000)
                        {
                                switch (len)
                                {
                                        case 1:
                                        temp[0]=readmeml(addr);
                                        break;
                                        case 2:
                                        temp[1]=readmeml(addr);
                                        temp[0]=readmeml(addr+4);
                                        break;
                                        case 3:
                                        temp[2]=readmeml(addr);
                                        temp[1]=readmeml(addr+4);
                                        temp[0]=readmeml(addr+8);
                                        break;
                                }
                                switch (opcode&0x408000)
                                {
                                        case 0x000000: /*Single*/
                                        fparegs[FD]=(double)(*tfs);
//                                        rpclog("Loaded %f %f %i %08X %08X %08X %08X\n",*tfs,fparegs[FD],len,addr,temp[0],temp[1],temp[2]);
                                        break;
                                        case 0x008000: /*Double*/
                                        fparegs[FD]=*tf;
//                                        rpclog("F%i = %f\n",FD,(double)fparegs[FD]);
                                        break;

                                        case 0x400000: /*Long*/
                                        temp[4]=temp[2]>>11;
                                        temp[4]|=(temp[1]<<21);
                                        temp[5]=(temp[1]&~0x80000000)>>11;
                                        temp[5]|=((temp[0]&0x7FFF)<<16);
                                        temp[5]|=(temp[0]&0x80000000);
                                        fparegs[FD]=*tf2;
//                                        fparegs[FD]=*tf;
//                                        rpclog("F%i = %f\n",FD,(double)fparegs[FD]);
                                        break;
                                }
                        }
                        else
                        {
//                                rpclog("Write %f to %08X %08X %i %i\n",fparegs[FD],addr,armregs[RN]);
                                switch (len)
                                {
                                        case 1:
                                        writememl(addr,temp[0]);
                                        break;
                                        case 2:
                                        writememl(addr,temp[1]);
                                        writememl(addr+4,temp[0]);
                                        break;
                                        case 3:
                                        writememl(addr,temp[2]);
                                        writememl(addr+4,temp[1]);
                                        writememl(addr+8,temp[0]);
                                        break;
                                }
                        }
                        if (!(opcode&0x1000000))
                        {
                                if (opcode&0x800000) addr+=((opcode&0xFF)<<2);
                                else                 addr-=((opcode&0xFF)<<2);
                        }
                        if (opcode&0x200000) armregs[RN]=addr;
                        return;
                }
                if (opcode&0x100000) /*LFM*/
                {
                        addr=GETADDR(RN);
                        if (opcode&0x1000000)
                        {
                                if (opcode&0x800000) addr+=((opcode&0xFF)<<2);
                                else                 addr-=((opcode&0xFF)<<2);
                        }
//                        rpclog("LFM from %08X %08X %07X\n",GETADDR(RN),addr,PC);
                        switch (opcode&0x408000)
                        {
                                case 0x000000: /*4 registers*/
                                temp[2]=readmeml(addr);
                                temp[1]=readmeml(addr+4);
                                temp[0]=readmeml(addr+8);
                                fparegs[FD]=*tf;
                                temp[2]=readmeml(addr+12);
                                temp[1]=readmeml(addr+16);
                                temp[0]=readmeml(addr+20);
                                fparegs[(FD+1)&7]=*tf;
                                temp[2]=readmeml(addr+24);
                                temp[1]=readmeml(addr+28);
                                temp[0]=readmeml(addr+32);
                                fparegs[(FD+2)&7]=*tf;
                                temp[2]=readmeml(addr+36);
                                temp[1]=readmeml(addr+40);
                                temp[0]=readmeml(addr+44);
                                fparegs[(FD+3)&7]=*tf;
                                break;
                                case 0x408000: /*3 registers*/
                                temp[2]=readmeml(addr);
                                temp[1]=readmeml(addr+4);
                                temp[0]=readmeml(addr+8);
                                fparegs[FD]=*tf;
                                temp[2]=readmeml(addr+12);
                                temp[1]=readmeml(addr+16);
                                temp[0]=readmeml(addr+20);
                                fparegs[(FD+1)&7]=*tf;
                                temp[2]=readmeml(addr+24);
                                temp[1]=readmeml(addr+28);
                                temp[0]=readmeml(addr+32);
                                fparegs[(FD+2)&7]=*tf;
                                break;
                                case 0x400000: /*2 registers*/
                                temp[2]=readmeml(addr);
                                temp[1]=readmeml(addr+4);
                                temp[0]=readmeml(addr+8);
                                fparegs[FD]=*tf;
                                temp[2]=readmeml(addr+12);
                                temp[1]=readmeml(addr+16);
                                temp[0]=readmeml(addr+20);
                                fparegs[(FD+1)&7]=*tf;
                                break;
                                case 0x008000: /*1 register*/
                                temp[2]=readmeml(addr);
                                temp[1]=readmeml(addr+4);
                                temp[0]=readmeml(addr+8);
                                fparegs[FD]=*tf;
                                break;

                                default:
                                rpclog("Bad number of registers to load %06X\n",opcode&0x408000);
                                dumpregs();
                                exit(-1);
                        }
//                        rpclog("Loaded %08X  %i  %f %f %f %f\n",opcode&0x408000,FD,fparegs[FD],fparegs[(FD+1)&7],fparegs[(FD+2)&7],fparegs[(FD+3)&7]);
                        if (!(opcode&0x1000000))
                        {
                                if (opcode&0x800000) addr+=((opcode&0xFF)<<2);
                                else                 addr-=((opcode&0xFF)<<2);
                        }
                        if (opcode&0x200000) armregs[RN]=addr;
                        return;
                }
                else /*SFM*/
                {
                        addr=GETADDR(RN);
                        if (opcode&0x1000000)
                        {
                                if (opcode&0x800000) addr+=((opcode&0xFF)<<2);
                                else                 addr-=((opcode&0xFF)<<2);
                        }
//                        rpclog("SFM from %08X %08X %07X\n",GETADDR(RN),addr,PC);
                        switch (opcode&0x408000)
                        {
                                case 0x000000: /*4 registers*/
                                temp[2]=0;
                                *tf=fparegs[FD];
                                writememl(addr,temp[2]);
                                writememl(addr+4,temp[1]);
                                writememl(addr+8,temp[0]);
                                *tf=fparegs[(FD+1)&7];
                                writememl(addr+12,temp[2]);
                                writememl(addr+16,temp[1]);
                                writememl(addr+20,temp[0]);
                                *tf=fparegs[(FD+2)&7];
                                writememl(addr+24,temp[2]);
                                writememl(addr+28,temp[1]);
                                writememl(addr+32,temp[0]);
                                *tf=fparegs[(FD+3)&7];
                                writememl(addr+36,temp[2]);
                                writememl(addr+40,temp[1]);
                                writememl(addr+44,temp[0]);
                                break;
                                case 0x408000: /*3 registers*/
                                temp[2]=0;
                                *tf=fparegs[FD];
                                writememl(addr,temp[2]);
                                writememl(addr+4,temp[1]);
                                writememl(addr+8,temp[0]);
                                *tf=fparegs[(FD+1)&7];
                                writememl(addr+12,temp[2]);
                                writememl(addr+16,temp[1]);
                                writememl(addr+20,temp[0]);
                                *tf=fparegs[(FD+2)&7];
                                writememl(addr+24,temp[2]);
                                writememl(addr+28,temp[1]);
                                writememl(addr+32,temp[0]);
                                break;
                                case 0x400000: /*2 registers*/
                                temp[2]=0;
                                *tf=fparegs[FD];
                                writememl(addr,temp[2]);
                                writememl(addr+4,temp[1]);
                                writememl(addr+8,temp[0]);
                                *tf=fparegs[(FD+1)&7];
                                writememl(addr+12,temp[2]);
                                writememl(addr+16,temp[1]);
                                writememl(addr+20,temp[0]);
                                break;
                                case 0x008000: /*1 register*/
                                temp[2]=0;
                                *tf=fparegs[FD];
                                writememl(addr,temp[2]);
                                writememl(addr+4,temp[1]);
                                writememl(addr+8,temp[0]);
                                break;
                                
                                default:
                                rpclog("Bad number of registers to store %06X\n",opcode&0x408000);
                                dumpregs();
                                exit(-1);
                        }
                        if (!(opcode&0x1000000))
                        {
                                if (opcode&0x800000) addr+=((opcode&0xFF)<<2);
                                else                 addr-=((opcode&0xFF)<<2);
                        }
                        if (opcode&0x200000) armregs[RN]=addr;
                        return;
                }
                /*LFM/SFM*/
                error("SFM opcode %08X\n",opcode);
                dumpregs();
                exit(-1);
                return;
                case 0xE:
                if (opcode&0x10)
                {
                        if (RD==15 && opcode&0x100000) /*Compare*/
                        {
                                switch ((opcode>>21)&7)
                                {
                                        case 4: /*CMF*/
                                        case 6: /*CMFE*/
                                        if (opcode&8) tempf=fconstants[opcode&7];
                                        else          tempf=fparegs[opcode&7];
                                        setsubf(fparegs[FN],tempf);
                                        return;
                                }
                                error("Compare opcode %08X %i\n",opcode,(opcode>>21)&7);
                                rpclog("Compare opcode %08X %i\n",opcode,(opcode>>21)&7);
                                dumpregs();
                                exit(-1);
                                return;
                        }
                        /*Register transfer*/
                        switch ((opcode>>20)&0xF)
                        {
                                case 0: /*FLT*/
                                fparegs[FN]=(double)armregs[RD];
//                                rpclog("FLT F%i now %f from R%i %08X %i %07X %07X %07X %07X\n",FN,fparegs[FN],RD,armregs[RD],armregs[RD],PC,oldpc,oldpc2,oldpc3);
                                return;
                                case 1: /*FIX*/
                                armregs[RD]=(int)fparegs[opcode&7];
//                                rpclog("FIX F%i (%f) to R%i (%08X %i)\n",FN,fparegs[FN],RD,armregs[RD],armregs[RD]);
                                return;
                                case 2: /*WFS*/
                                fpsr=(armregs[RD]&0xFFFFFF)|(fpsr&0xFF000000);
                                return;
                                case 3: /*RFS*/
                                armregs[RD]=fpsr;
                                return;
                                case 4: /*WFC*/
                                fpcr=(fpcr&~0xD00)|(armregs[RD]&0xD00);
                                return;
                                case 5: /*RFC*/
                                armregs[RD]=fpcr;
                                return;
                        }
                        error("Register opcode %08X at %07X\n",opcode,PC);
                        dumpregs();
                        exit(-1);
                        return;
                }
                if (opcode&8) tempf=fconstants[opcode&7];
                else          tempf=fparegs[opcode&7];
//                rpclog("Data %08X %06X\n",opcode,opcode&0xF08000);
//                rpclog("F%i F%i F%i\n",FD,FN,opcode&7);
                if ((opcode&0x8000) && ((opcode&0xF08000)>=0x508000) && ((opcode&0xF08000)<0xE08000))
                {
                        armregs[15]+=4;
                        undefined();
                        return;
                }
                switch (opcode&0xF08000)
                {
                        case 0x000000: /*ADF*/
//                        rpclog("ADF %f+%f=",fparegs[FN],tempf);
                        fparegs[FD]=fparegs[FN]+tempf;
//                        rpclog("%f\n",fparegs[RD]);
                        return;
                        case 0x100000: /*MUF*/
                        case 0x900000: /*FML*/
//                        rpclog("MUF %f*%f=",fparegs[FN],tempf);
                        fparegs[FD]=fparegs[FN]*tempf;
//                        rpclog("%f\n",fparegs[RD]);
                        return;
                        case 0x200000: /*SUF*/
//                        rpclog("SUF %f-%f=",fparegs[FN],tempf);
                        fparegs[FD]=fparegs[FN]-tempf;
//                        rpclog("%f\n",fparegs[RD]);
                        return;
                        case 0x300000: /*RSF*/
//                        rpclog("SUF %f-%f=",fparegs[FN],tempf);
                        fparegs[FD]=tempf-fparegs[FN];
//                        rpclog("%f\n",fparegs[RD]);
                        return;
                        case 0x400000: /*DVF*/
                        case 0xA00000: /*FDV*/
//                        rpclog("DVF %f/%f=",fparegs[FN],tempf);
                        fparegs[FD]=fparegs[FN]/tempf;
//                        rpclog("%f  %07X\n",fparegs[RD],PC);
                        return;
                        case 0x008000: /*MVF*/
//                        rpclog("MVF %f=\n",tempf);
                        fparegs[FD]=tempf;
//                        rpclog("%f\n",fparegs[RD]);
                        return;
                        case 0x108000: /*MNF*/
//                        rpclog("MNF %f=\n",tempf);
                        fparegs[FD]=-tempf;
//                        rpclog("%f\n",fparegs[RD]);
                        return;
                        case 0x208000: /*ABS*/
                        fparegs[FD]=fabs(tempf);
                        return;
                        case 0x408000: /*SQT*/
                        fparegs[FD]=sqrt(tempf);
                        return;
                        case 0x508000: /*LOG*/
                        fparegs[FD]=log10(tempf);
                        return;
                        case 0x608000: /*LGN*/
                        fparegs[FD]=log(tempf);
                        return;
                        case 0x708000: /*EXP*/
                        fparegs[FD]=exp(tempf);
                        return;
                        case 0x808000: /*SIN*/
//                        rpclog("SIN of %f is ",tempf);
                        fparegs[FD]=sin(tempf);
//                        rpclog("%f\n",fparegs[FD]);
                        return;
                        case 0x908000: /*COS*/
                        fparegs[FD]=cos(tempf);
                        return;
                        case 0xA08000: /*TAN*/
                        fparegs[FD]=tan(tempf);
                        return;
                        case 0xB08000: /*ASN*/
                        fparegs[FD]=asin(tempf);
                        return;
                        case 0xC08000: /*ACS*/
                        fparegs[FD]=acos(tempf);
                        return;
                        case 0xD08000: /*ATN*/
                        fparegs[FD]=atan(tempf);
                        return;
                }
                /*Data processing*/
                error("Bad data opcode %08X %06X\n",opcode,opcode&0xF08000);
                rpclog("Bad data opcode %08X %06X\n",opcode,opcode&0xF08000);
                rpclog("Fm is equal to %f\n",tempf);
                dumpregs();
                exit(-1);
                return;
        }
}
