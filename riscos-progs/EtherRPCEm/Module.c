// c.Module
//
//  This program is free software; you can redistribute it and/or modify it 
//  under the terms of version 2 of the GNU General Public License as 
//  published by the Free Software Foundation;
//
//  This program is distributed in the hope that it will be useful, but WITHOUT 
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
//  more details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program; if not, write to the Free Software Foundation, Inc., 59 
//  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//  
//  The full GNU General Public License is included in this distribution in the
//  file called LICENSE.

//  The code in this file is (C) 2003 J Ballance as far as
//  the above GPL permits
//
//  Modifications for RPCEmu (C) 2007 Alex Waugh


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <kernel.h>
#include <swis.h>

#include "ModHdr.h"
#include "Defines.h"
#include "Structs.h"
#include "Module.h"

#include "DCI.h"

#define  UNUSED(x)              (x = x)

_kernel_oserror *networktxswi(struct mbuf *mbufs, int dest, int src, int frametype);
_kernel_oserror *networkrxswi(struct mbuf *mbuf, rx_hdr *hdr, int *valid);
_kernel_oserror *networkirqswi(volatile int *irqstatus);
_kernel_oserror *networkhwaddrswi(unsigned char *hwaddr);
void callrx(dib *dibaddr, struct mbuf *mbuf_chain, int claimaddr, int pwp);

// versionof DCI supported
#define DCIVersion 403

static _kernel_oserror
                     ErrorIllegalFlags   = {0x58CC0,"Illegal flags"},
                     ErrorInvalidMTU     = {0x58CC1,"Invalid MTU requested"},
                     ErrorIllegalSWI     = {0x58CC2,"Not an EtherL SWI"},
                     ErrorNoUnit         = {0x58CC3,"Unit not configured"},
                     ErrorBadClaim       = {0x58CC4,"Illegal frame claim"},
                     ErrorAlreadyClaimed = {0x58CC5,"frame already claimed"},
                     ErrorNoBuff         = {0x58CC6,"No buffer space"};

workspace *work;
static eystatsset supported = {(char)-1,(char)-1,(char)-1,(char)0,
                               -1,-1,-1,-1,-1,-1,-1,-1,-1,
                               (char)-1,(char)-1,(char)-1,(char)-1,(char)-1,(char)-1,(char)0,(char)0,
                               -1,-1,-1,-1,-1,-1,-1,-1,-1,
                               (char)-1,(char)-1,(char)-1,(char)-1,(char)-1,(char)-1,(char)0,(char)0 };

// Status word set to 1 by the emulated hardware when an IRQ generated
volatile int irqstatus = 0;

// mbufs waiting to be used, to save frequent allocation and deallocation
static struct mbuf *rxhdr_mbuf = NULL;
static struct mbuf *data_mbuf = NULL;

#define MBUF_MANAGER_VERSION 100


// startstop = 0 for starting, 1 for stopping
void SendServiceDCIDriverStatus(int startstop, workspace* work)
{
  _kernel_swi_regs r ;
   r.r[0] = (int)&work->base_dib;
   r.r[1] = Service_DCIDriverStatus;
   r.r[2] = startstop;                  // driver stopping
   r.r[3] = DCIVersion;                 // DCI version
   _kernel_swi(OS_ServiceCall,&r,&r);
}

// called in callback time for a variety of reasons
void callback(workspace * work )
{
  SendServiceDCIDriverStatus(0,  work);
}

static _kernel_oserror *open_mbuf_manager_session(dci4_mbctl* mbctl)
{
    _kernel_swi_regs r ;
    memset(mbctl, 0, sizeof(struct mbctl));
    mbctl->mbcsize = sizeof(struct mbctl);
    mbctl->mbcvers = MBUF_MANAGER_VERSION;
    mbctl->flags = 0;
    mbctl->advminubs = 0;
    mbctl->advmaxubs = 0;
    mbctl->mincontig = 0;
    mbctl->spare1 = 0;

    r.r[0] = (int) mbctl;

    return(_kernel_swi(Mbuf_OpenSession, &r, &r));   
}

static _kernel_oserror *close_mbuf_manager_session(dci4_mbctl* mbctl)
{
   _kernel_swi_regs r ;
   r.r[0] = (int) mbctl;
   return(_kernel_swi(Mbuf_CloseSession, &r, &r));
}

// report frame release, unlink it, and free memory
void bounceclaim(claimbuf* cb)
{
  _kernel_swi_regs rg;
  rg.r[0]=(int)&work->base_dib;
  rg.r[1]=Service_DCIFrameTypeFree;
  rg.r[2]=cb->frame | (cb->adtype<<16);;
  rg.r[3]=cb->addresslevel;
  rg.r[4]=cb->errorlevel;
  _kernel_swi(OS_ServiceCall,&rg,&rg);          // report its now nolonger claimed
  if(cb->last)
  {
    ((claimbuf*)(cb->last))->next=cb->next;
  }
  else
  {
    work->claims = cb->next;
  }
  if(cb->next)((claimbuf*)(cb->next))->last=cb->last;          // unlink from chain
  free(cb);
}

// called at mod finalisation to clean up...
_kernel_oserror *finalise(int fatal, int podule, void *private_word)
{

    _kernel_swi_regs sregs;

   // ints off to ensure it stays quite..
    _kernel_swi(OS_IntOff,&sregs,&sregs);

   networkirqswi(0);
   _swix(OS_ReleaseDeviceVector, _INR(0,4), 13, CallEveryVeneer, work, &irqstatus, 1);

  // free any mem owned here
  if(work)
   {
     SendServiceDCIDriverStatus(1,  work);    // flag we're stopping
     while (work->claims!=NULL)      // work list to free claimbufs
     {
       bounceclaim(work->claims);
     }
     if(work->mbctl)
     {
       if(rxhdr_mbuf) work->mbctl->freem(work->mbctl,rxhdr_mbuf);
       if(data_mbuf) work->mbctl->freem(work->mbctl,data_mbuf);

       close_mbuf_manager_session(work->mbctl);
       free(work->mbctl);
       work->mbctl=NULL;
     }  
     free(work);
   }
   return 0;
}                              

static void InitChip(workspace *work)
{
   _kernel_swi_regs sregs;

   work->base_dib.dib_swibase=EtherRPCEm_00;
   work->base_dib.dib_name="rpcem";
   work->base_dib.dib_unit=0;              
   work->base_dib.dib_address=(unsigned char *)work->dev_addr;
   work->base_dib.dib_module="EtherRPCEm";
   work->base_dib.dib_location="Emulated";
   work->base_dib.dib_slot.slotid=0;
   work->base_dib.dib_slot.minor=0;
   work->base_dib.dib_slot.pcmciaslot=0;
   work->base_dib.dib_slot.mbz=0;
   work->base_dib.dib_inquire= EYFlagsSupported;
   work->claims=NULL;             // flag no claimbufs

   sregs.r[0] = (int)"Inet$EtherType";
   sregs.r[1] = (int)work->base_dib.dib_name;
   sregs.r[2] = (int)strlen((char*)sregs.r[1]);
   sregs.r[3] =      0;
   sregs.r[4] =      4;

   _kernel_swi(OS_SetVarVal,&sregs,&sregs);

   sregs.r[0]= (int)(CallBkVeneer);      // where to call
   sregs.r[1]= (int)work;                // work pointer for this
   _kernel_swi(OS_AddCallBack,&sregs,&sregs);
}

_kernel_oserror *initialise(const char *cmd_tail, int podule_base, void *private_word)
{
   _kernel_oserror *err;
   int ribuff[2]={0,0};
   unsigned char *PodMaskAddr;
   unsigned char PodMask;

   UNUSED(cmd_tail);
   UNUSED(podule_base);

// init some of our variables
   work    = 0;
// now set up links to system functions

   if(work = calloc(1,sizeof(workspace)),work==NULL)   // ensure all pointers cleared to Null
   {
     return &ErrorNoBuff;
   }

   networkhwaddrswi((unsigned char *)work->dev_addr);

   work->pwp=private_word;
   // claim mbuf manager link
   if(work->mbctl = calloc(1,sizeof(dci4_mbctl)),work->mbctl==NULL)
   {
     return &ErrorNoBuff;
   }

   if(err=open_mbuf_manager_session(work->mbctl),err!=NULL)
   {
     return err;
   }

   InitChip(work);

   err = _swix(Podule_ReadInfo,_INR(0,3),0x18000,ribuff,sizeof(ribuff),0);
   if (err) return err;

   PodMaskAddr =(unsigned char*)ribuff[0];  // IOC irq reg
   PodMask     =(unsigned char)ribuff[1];   // IOC irq mask for pod

   err = _swix(OS_ClaimDeviceVector, _INR(0,4), 13, CallEveryVeneer, work, &irqstatus, 1);
   if (err) return err;

    *PodMaskAddr |= PodMask;                // set the pod irq

   networkirqswi(&irqstatus);
   return NULL;
}

_kernel_oserror *callevery_handler(workspace * work )
{
  rx_hdr *rxhdr;

  static volatile int sema = 0;

  if (sema) return NULL;
  sema = 1;
  
  do
  {
    if (rxhdr_mbuf == NULL)
    {
      rxhdr_mbuf = work->mbctl->alloc_s(work->mbctl,sizeof(rx_hdr),NULL);
    }
    if (data_mbuf == NULL)
    {
      data_mbuf = work->mbctl->alloc_s(work->mbctl,1500,NULL);
    }
    if (rxhdr_mbuf && data_mbuf)
    {
      int valid;
      rxhdr = (rx_hdr *)(((char *)rxhdr_mbuf) + rxhdr_mbuf->m_off);
      rxhdr_mbuf->m_len = sizeof(rx_hdr);
      if (networkrxswi(data_mbuf, rxhdr, &valid) || !valid)
      {
        // No data
        sema = 0;
        return NULL;
      }

      if (1)  {

           /* do stuff to make a new packet */
           claimbuf           * cb;
           int                  AddrLevel=AlSpecific;
           ClaimType            Claim=NotClaimed;
           

           cb=work->claims;
           while(cb!=NULL)
           {
             switch(cb->adtype)
             {
               case AdSpecific: if(  (AddrLevel<=AlNormal)
                                  && (rxhdr->rx_frame_type==cb->frame)
                                  && (AddrLevel<=cb->addresslevel) )Claim=ClaimSpecific;
                            break;
               case AdMonitor:  if((rxhdr->rx_frame_type>1500) && (AddrLevel<=cb->addresslevel)) Claim=ClaimMonitor;
                            break;
               case AdIeee:     if(rxhdr->rx_frame_type<1501) Claim=ClaimIEEE;
                            break;
               case AdSink:     if(rxhdr->rx_frame_type>1500) Claim=ClaimSink;
               default:
                            break;
             }

             if(Claim != NotClaimed) // someone wants it 
             {                     // so check further
                rxhdr_mbuf->m_next = data_mbuf;      // link them together
                rxhdr_mbuf->m_list = 0;        // this is needed
                //data_mbuf->m_next = NULL;      // link them together
                data_mbuf->m_list = 0;        // this is needed
                // at this point we dont need to check for safe mbuf
                // as we've used the alloc routine that only allocs
                // mbufs that actually are safe

                // now build the rxhdr structure
                rxhdr_mbuf->m_type = MT_HEADER;

                _swix(OS_IntOn, 0);
                callrx(&work->base_dib, rxhdr_mbuf, cb->claimaddress, cb->pwp);
                // The protocol stack is now responsible for freeing the mbufs
                rxhdr_mbuf = NULL;
                data_mbuf = NULL;
                _swix(OS_IntOff, 0);
                break;
             }
             cb=cb->next;                       // missed that frame type, so try another

           }
          }

    }
    else
    {
      // mbuf exhaustion
      sema = 0;
      return NULL;
    }
  }
  while (1);

  sema = 0;
  return NULL;
}

// module service call handler
void service_call(int service_number,_kernel_swi_regs * r,void * pw)
{
  _kernel_swi_regs rg;
  _kernel_oserror *erp;


  switch(service_number)
  {
    case Service_EnumerateNetworkDrivers:  // on entry, r0-> chain of dibs
                             rg.r[0] = RMAClaim;
                             rg.r[3] = sizeof(chaindib);
                             if(erp=_kernel_swi(OS_Module,&rg,&rg),erp==NULL)
                             {   // got extra memory to add chaindib 
                               ((chaindib*)rg.r[2])->chd_dib = &work->base_dib;
                               ((chaindib*)rg.r[2])->chd_next= (chaindib*)r->r[0];
                               r->r[0] = rg.r[2];              // link our chaindib into the chain
                             }
                             break;
                                                                      
    case Service_DCIProtocolStatus:
                        if(r->r[2]==0)  // its protocol starting
                        {
                          rg.r[0]= (int)(CallBkVeneer);    // where to call
                          rg.r[1]= (int)work;                // work pointer for this
                          _kernel_swi(OS_AddCallBack,&rg,&rg);
                        }
                             break;
                                   
    case Service_MbufManagerStatus:
                 switch(r->r[0])
                 {
                   case 0: // Manager started ... better try to restart our stuff
                          if(!work->mbctl)
                          {
                            if(erp=open_mbuf_manager_session(work->mbctl),erp==NULL)
                            {
                              InitChip(work);
                            }
                          }
                          break;
                   case 1: // Manager stopping .. cannot if sessions are active
                          if(work->mbctl)r->r[1] = 0; // claim .. we're active
                          break;
                   default: // Manager scavenge
                          break;
                 }
                             break;
                                   
  }
}

_kernel_oserror *swi_handler(int swi_no, _kernel_swi_regs *r, void *private_word)
{
    claimbuf *thism,*next;
    _kernel_oserror *err;

    UNUSED(private_word);
    switch(swi_no)
    {
// return version num of DCI spec implelmented
// on entry:     r0 = flags (all zero)
// on exit:      r1 = version (4.03 at present)
      case SWIDCIVersion:
          if(r->r[0]==0)
          {
            r->r[1] = DCIVersion;
            return 0;
          }
          else
          {
            return &ErrorIllegalFlags;
          }
          break;     

// return bitmap of supported features
// on entry:     r0 = flags (all zero)
//               r1 = unit number
// on exit:      r2 = info word
      case SWIInquire:
          if(r->r[0]==0)
          {
            r->r[2] = EYFlagsSupported;
          }
          else
          {
            return &ErrorIllegalFlags;
          }
          break;     

// return physical MTU size of supported network
// on entry:     r0 = flags (all zero)
//               r1 = unit number
// on exit:      r2 = MTU size
      case SWIGetNetworkMTU:
          if(r->r[0]==0)
          {
            r->r[2] = EY_MTU;
          }
          else
          {
            return &ErrorIllegalFlags;
          }
          break;     

// set physical MTU size of supported network
// on entry:     r0 = flags (all zero)
//               r1 = unit number
//               r2 = new MTU
// if size change not supported, return illegal op error
// on exit, illegal op error!
      case SWISetNetworkMTU:
          if(r->r[0]==0)
          {
            return &ErrorInvalidMTU;
          }
          else
          {
            return &ErrorIllegalFlags;
          }
         break;     

// transmit data
// on entry:     r0 = flags (see below)
//               r1 = unit number
//               r2 = frame type
//               r3 -> mbuf chain of data to tx
//               r4 -> dest h/w address (byte aligned)
//               r5 -> srce h/w address (byte aligned)(if needed)
// on exit:      all regs preserved
//
// Flags:        bit 0:  0 = use own H/W address
//                       1 = use r5 as srce H/W addr
//               bit 1:  0 = driver to assume ownership of mbuf chain
//                       1 = driver does not own mbuf chain
      case SWITransmit: {
        struct mbuf *mbufchain = (struct mbuf *)r->r[3];

        err = NULL;

        while (mbufchain) {
          struct mbuf *next = mbufchain->m_list;

          if (err == NULL) err = networktxswi(mbufchain, r->r[4], (r->r[0] & 1) ? r->r[5] : 0, r->r[2]);

          if((r->r[0] & 2) == 0) work->mbctl->freem(work->mbctl,mbufchain);
          mbufchain = next;
        }
        if (err) return err;
        break;
      }
// on entry:     r0 = flags (see below)
//               r1 = unit number
//               r2 = frame type    bottom 16 bits, frame type, top 16.. addr class 
//                                  1.. specific        2.. sink
//                                  3.. monitor         4.. ieee
//               r3 = address level (for write)
//               r4 = error level (for write)
//               r5 = handlers private word pointer
//               r6 = address of routine to receive this frame
//
// on exit:      all regs preserved
//
// Flags:        bit 0:  0 = claim frame type
//                       1 = release frame type
//               bit 1:  0 = drivers can pass unsafe mbuf chains back
//                       1 = ensure_safe required before mbufs passed back
      case SWIFilter:
          if((r->r[0] & 1)==1)
          {   // release frame
              if(r->r[1]!=0) return &ErrorNoUnit; // only unit 0 supported at present
              thism=work->claims;  // first pointer locn
              while(thism!=NULL)
              {                                   // find list end
                if((thism->unit==r->r[1]) &&
                   (thism->frame==(r->r[2] &0xffff)) &&
                   (thism->adtype==(r->r[2] >>16)) &&
                   (thism->addresslevel==r->r[3]) &&
                   (thism->errorlevel==r->r[4]) &&
                   (thism->pwp==r->r[5]) &&
                   (thism->claimaddress==r->r[6]))   // we've found the claim
                {
                  bounceclaim(thism);                // unlink it
                  break;
                }
                thism=thism->next;
              }
 
          }
          else
          {   // claim frame type
              if(r->r[1]!=0) return &ErrorNoUnit; // only unit 0 supported at present
              thism=next=work->claims;
              while(next!=NULL)
              {                                   // find list end or current claimer
                int adtype,used;
                adtype=r->r[2]>>16;
                thism=next;
                used=0;
                if((thism->frame==r->r[2] &0xffff)  // already a claimer..
                   || (adtype==AdMonitor))         // or we're greedy
                {
                  switch(adtype)                 // check
                  {
                    case AdSpecific: return &ErrorAlreadyClaimed;
                                break;
                    case AdSink:   if (thism->addresslevel==AdSpecific)
                                   {
                                     bounceclaim(thism);
                                     goto claimframe;
                                   }
                                   else return &ErrorAlreadyClaimed;
                                break;
                    case AdMonitor:
                        if (thism->adtype<AdMonitor)  // bounce anything less
                                   {
                                     next=thism->next;
                                     bounceclaim(thism);
                                     used=1;
                                     break;
                                   }
                                   if(thism->adtype!=AdIeee) return &ErrorAlreadyClaimed;
                                break;
                    case AdIeee: return &ErrorAlreadyClaimed;
                                break;
                    default: return &ErrorBadClaim;
                                break;
                  }
                  if(thism->adtype >adtype) //
                  {
                  }
                }
                  if(!used)next=next->next;

              }
              // thism points to first NULL claimbuf ptr
              if(next=calloc(1,sizeof(claimbuf)),next==NULL)
              {
                  return &ErrorNoBuff;
              }    
              if(thism!=NULL)
              {
                 thism->next=next;
                 next->last=thism;
              }
              else
              {
                 work->claims=next;
                 next->last=NULL;
              }
claimframe:
              next->flags=(char)r->r[0]&1;
              next->unit=r->r[1];
              next->frame=r->r[2]&0xffff;
              next->adtype=r->r[2]>>16;
              next->addresslevel=r->r[3];
              next->errorlevel=r->r[4];
              next->pwp=r->r[5];
              next->claimaddress=r->r[6];
          }
          // now check all claims and set multicast or promiscuous as needed
          next=work->claims;
          work->flags=0;
          while(next)
          {
            if(next->addresslevel==AlMulticast)work->flags|=IFF_ALLMULTI;
            if(next->addresslevel==AlPromiscuous)work->flags|=IFF_PROMISC;
            next=next->next;
          }
          break;     

// return unit statistics
// on entry:     r0 = flags (see below)
//               r1 = unit number
//               r2 -> buffer for results
// on exit:      all regs preserved
//
// Flags:        bit 0:  0 = indicate which stats are gathered
//                       1 = return actual stats
      case SWIStats:
            if((r->r[0] & 1))    // return actual stats
            { //fill in current state, then  return full info
/*              lp->stat.st_link_status =0;
              {
                 char saved_bank;
                 int a;
                 _kernel_irqs_off();
                 saved_bank = INB( work->basedev, BANK_SELECT );
                 SMC_SELECT_BANK( work->basedev,0 );
  
                 a=INW(work->basedev,RPC_REG);
                 lp->stat.st_interface_type= (a&RPC_SPEED)?12:3;
                 lp->stat.st_link_status |= (a&RPC_DPLX) ?1:0;
                 SMC_SELECT_BANK( work->basedev,saved_bank );
                 _kernel_irqs_on();
              } 
              lp->stat.st_link_status |=(((work->basedev->flags)&3)<<2) |1 ;
              lp->stat.st_link_status |=(work->basedev->claims)?2:0;
              memcpy((void*)r->r[2],&work->basedev->priv->stat,sizeof(eystatsset)-4);*/
            }
            else
            { // indicate whats supported
              memcpy((void*)r->r[2],&supported.st_interface_type,sizeof(eystatsset)-4);
            }
          break;     

// on entry:     r0 = flags (see below)
//               r1 = unit number
//               r2 = frame type
//               r3 ->(byte aligned) multicast hardware (MAC) address
//               r4 ->(word aligned) multicast IP address
//               r5 = priv word ptr
//               r6 = address of handler routine to receive frames
// on exit:      all regs preserved
//
// Flags:        bit 0:  0 = request an mc addr
//                       1 = release an mc addr
//               bit 1:  0 = request/release specific mc addr (in r3 & r4)
//                       1 = request/release all mc addrs
     case SWIMulticastreq:
          if(r->r[0] && ~3)
          {
           printf("\n EYmulticast not fully implemented yet\n");
          }
          break;     

      default:
            return &ErrorIllegalSWI;
          break;     
    }
    return 0;
}


