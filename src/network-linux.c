/* RPCemu networking */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <linux/sockios.h>

#include "rpcemu.h"
#include "mem.h"
#include "podules.h"

/* The opened tunnel device */
static int tunfd = -1;

/* Max packet is 1500 bytes plus headers */
static unsigned char buffer[1522];

/* Hardware address */
static unsigned char hwaddr[6];

static podule *poduleinfo = NULL;

/* Structures to represent the RISC OS view of things */
struct pkthdr {
    uint32_t len;
    uint32_t rcvif;
};

struct mbuf {
    uint32_t m_next;
    uint32_t m_list;
    uint32_t m_off;
    uint32_t m_len;
    uint32_t m_inioff;
    uint32_t m_inilen;
    uint8_t m_type;
    uint8_t m_sys1;
    uint8_t m_sys2;
    uint8_t m_flags;
    struct pkthdr m_pkthdr;
};

struct rx_hdr {
    uint32_t rx_ptr;
    uint32_t rx_tag;
    uint8_t rx_src_addr[6];
    uint8_t _spad[2];
    uint8_t rx_dst_addr[6];
    uint8_t _dpad[2];
    uint32_t rx_frame_type;
    uint32_t rx_error_level;
    uint32_t rx_cksum;
};

static void nametouidgid(const char* user, uid_t *uid, gid_t *gid)
{
    struct passwd* pw;
    pw = (struct passwd*) getpwnam(user);
  
    if (pw) {
        *uid = pw->pw_uid;
        if (gid) {
            *gid = pw->pw_gid;
        }
    }
}

static int dropprivileges(uid_t uid, gid_t gid)
{
    if (setgid(gid) != 0 || setuid(uid) != 0)
        return -1;
    else
        return 0;
}

static int parsemacaddr(const char *text, unsigned char *hwaddr)
{
    const char *textptr = text;
    int byte;

    for (byte = 0; byte < 6; byte++) {
        int nybble;

        hwaddr[byte] = 0;

        for (nybble = 0; nybble < 2; nybble++) {
            hwaddr[byte] <<= 4;

            if (isdigit(*textptr)) {
                hwaddr[byte] |= *textptr - '0';
            } else if (isxdigit(*textptr)) {
                hwaddr[byte] |= (toupper(*textptr) - 'A') + 10;
            } else {
                goto fail;
            }

            textptr++;
        }

        if (*textptr == ':') {
            textptr++;
        } else if (byte < 5) {
            goto fail;
        }
    }

    if (*textptr)
        goto fail;

    return 0;

fail:
    fprintf(stderr, "Error parsing hardware address %s\n", text);
    return -1;
}

/* Open and configure the tunnel device */
static int tun_alloc(void)
{
    struct ifreq ifr;
    struct sockaddr_in *addr;
    int fd;
    int sd;
    
    if (config.ipaddress == NULL && config.bridgename == NULL) {
        fprintf(stderr, "IP address not configured\n");
        return -1;
    }

    if ((fd = open("/dev/net/tun", O_RDWR)) < 0) {
        fprintf(stderr, "Error opening /dev/net/tun device: %s\n", strerror(errno));
        return -1;
    }
    
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP; 
    
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        fprintf(stderr, "Error setting TAP on tunnel device: %s\n", strerror(errno));
        return -1;
    }

    ioctl(fd, TUNSETNOCSUM, 1);

    if ((sd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
        fprintf(stderr, "Error getting socket: %s\n", strerror(errno));
        return -1;
    }

    if (config.ipaddress != NULL) {
        addr = (struct sockaddr_in *)(&(ifr.ifr_addr));
        addr->sin_family = AF_INET;
        addr->sin_port = 0;
        inet_aton(config.ipaddress, &(addr->sin_addr));
        if (ioctl(sd, SIOCSIFADDR, &ifr) == -1) {
            fprintf(stderr, "Error assigning %s addr: %s\n",
                    ifr.ifr_name, strerror(errno));
            return -1;
        }
    } else if (config.bridgename != NULL) {
        struct ifreq brifr;

        if (ioctl(sd, SIOCGIFINDEX, &ifr) == -1) {
            fprintf(stderr, "Error interface index for %s: %s\n",
                    ifr.ifr_name, strerror(errno));
            return -1;
        }
        strncpy(brifr.ifr_name, config.bridgename, IFNAMSIZ);
        brifr.ifr_name[IFNAMSIZ - 1] = '\0';
        brifr.ifr_ifindex = ifr.ifr_ifindex;
        if (ioctl(sd, SIOCBRADDIF, &brifr) == -1) {
            fprintf(stderr, "Error adding %s to bridge: %s\n",
                    ifr.ifr_name, strerror(errno));
            return -1;
        }
    }

    /* Get the current flags */
    if (ioctl(sd, SIOCGIFFLAGS, &ifr) == -1) {
        fprintf(stderr, "Error getting %s flags: %s\n", ifr.ifr_name, strerror(errno));
        return -1;
    }
    
    /* Turn on the UP flag */
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(sd, SIOCSIFFLAGS, &ifr) == -1) {
        fprintf(stderr, "Error setting %s flags: %s\n", ifr.ifr_name, strerror(errno));
        return -1;
    }

    if (config.macaddress == NULL || parsemacaddr(config.macaddress, hwaddr) < 0) {
        /* Get the hardware address */
        if (ioctl(sd, SIOCGIFHWADDR, &ifr) == -1) {
            fprintf(stderr, "Error getting %s hardware address: %s\n",
                    ifr.ifr_name, strerror(errno));
            return -1;
        }

        /* Calculate the emulated hardware address */
        hwaddr[0] = 0x02;
        hwaddr[1] = 0x00;
        hwaddr[2] = 0xA4;
        hwaddr[3] = ifr.ifr_hwaddr.sa_data[3];
        hwaddr[4] = ifr.ifr_hwaddr.sa_data[4];
        hwaddr[5] = ifr.ifr_hwaddr.sa_data[5];
    }

    close(sd);

    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        fprintf(stderr, "Error setting %s non-blocking: %s\n", ifr.ifr_name, strerror(errno));
        return -1;
    }

    return fd;
}


void memcpytohost(void *dest, uint32_t src, uint32_t len)
{
    char *dst = dest;
    while (len--) {
        *dst++ = readmemb(src);
        src++;
    }
}

void memcpyfromhost(uint32_t dest, const void *source, uint32_t len)
{
    const char *src = source;
    while (len--) {
        writememb(dest, *src);
        src++;
        dest++;
    }
}

void strcpyfromhost(uint32_t dest, const char *source)
{
    memcpyfromhost(dest, source, strlen(source) + 1);
}

#define HEADERLEN 18

/* Transmit data
   errbuf - pointer to buffer to return error string
   mbufs - pointer to mbuf chain containing data to send
   dest - pointer to detination MAC address
   src - pointer to source MAC address, or 0 to use default

   returns errbuf on error, else zero
*/
static uint32_t tx(uint32_t errbuf, uint32_t mbufs, uint32_t dest, uint32_t src, uint32_t frametype)
{
    unsigned char *buf = buffer;
    struct mbuf txb;
    int packetlength;
    int ret;
    int i;

    if (tunfd == -1) {
        strcpyfromhost(errbuf, "RPCemu: Networking not available");
        return errbuf;
    }

    /* Ethernet packet is
       4 bytes preamble
       6 bytes destination MAC address
       6 bytes source MAC address
       2 bytes frame type (Ethernet II) or length (IEEE 802.3)
       up to 1500 bytes payload
    */

    for (i = 0; i < 4; i++) *buf++ = 0;

    memcpytohost(buf, dest, 6);
    buf += 6;

    if (src) {
        memcpytohost(buf, src, 6);
    } else {
        for (i = 0; i < 6; i++) {
            buf[i] = hwaddr[i];
        }
    }
    buf += 6;

    *buf++ = (frametype>>8) & 0xFF;
    *buf++ = frametype & 0xFF;

    packetlength = HEADERLEN;


    /* Copy the mbuf chain as the payload */
    while (mbufs) {
        memcpytohost(&txb, mbufs, sizeof(struct mbuf));
        packetlength += txb.m_len;
        if (packetlength > sizeof(buffer)) {
            strcpyfromhost(errbuf, "RPCemu: Packet too large to send");
            return errbuf;
        }
        memcpytohost(buf, mbufs + txb.m_off, txb.m_len);
        buf += txb.m_len;
        mbufs = txb.m_next;
    }

    do {
        ret = write(tunfd, buffer, packetlength);
    } while ((ret == -1) && (errno == EAGAIN));
    if (ret == -1) {
        strcpyfromhost(errbuf, strerror(errno));
        return errbuf;
    }

    return 0;
}


static uint32_t rx(uint32_t errbuf, uint32_t mbuf, uint32_t rxhdr, uint32_t *dataavail)
{
    struct mbuf rxb;
    struct rx_hdr hdr;
    int packetlength;

    *dataavail = 0;

    if (tunfd == -1) {
        strcpyfromhost(errbuf, "RPCemu: Networking not available");
        return errbuf;
    }

    memset(&hdr, 0, sizeof(hdr));

    packetlength = read(tunfd, buffer, sizeof(buffer));
    if (packetlength == -1) {
        if (errno == EAGAIN) return 0;
        strcpyfromhost(errbuf, strerror(errno));
        return errbuf;
    }
        
    if (mbuf && packetlength > HEADERLEN) {
        unsigned char *payload = buffer + HEADERLEN;

        /* Fill in recieved header structure */
        memcpy(hdr.rx_dst_addr, buffer + 4, 6);
        memcpy(hdr.rx_src_addr, buffer + 10, 6);
        hdr.rx_frame_type = (buffer[16] << 8) | buffer[17];
        hdr.rx_error_level = 0;
        memcpyfromhost(rxhdr, &hdr, sizeof(hdr));

        packetlength -= HEADERLEN;
        
        memcpytohost(&rxb, mbuf, sizeof(rxb));

        if (packetlength > rxb.m_inilen) {
            strcpyfromhost(errbuf, "RPCemu: Mbuf too small for recieved packet");
            return errbuf;
        } else {
            /* Copy payload in to the mbuf */
            rxb.m_off = rxb.m_inioff;
            memcpyfromhost(mbuf + rxb.m_off, payload, packetlength);
            rxb.m_len = packetlength;
            memcpyfromhost(mbuf, &rxb, sizeof(rxb));

            *dataavail = 1;
        }
    }

    return 0;
}

// Pointer to a word in RMA, used as the IRQ status register
static uint32_t irqstatus = 0;

static void sig_io(int sig) 
{
    writememb(irqstatus, 1);
    if (poduleinfo != NULL) {
        poduleinfo->irq = 1;
    }
    rethinkpoduleints();
}

static void set_irqstatus(uint32_t address)
{
    struct sigaction sa;
    int flags;

    memset(&sa, 0, sizeof(sa));

    if ((flags = fcntl(tunfd, F_GETFL)) == -1) {
        fprintf(stderr, "Error getting flags for network device: %s\n",
                strerror(errno));
        return;
    }

    if (irqstatus) {
        if (fcntl(tunfd, F_SETFL, flags & ~O_ASYNC) == -1) {
            fprintf(stderr, "Error disabling SIGIO for network device: %s\n",
                    strerror(errno));
            return;
        }

        sa.sa_handler = SIG_DFL;

        if (sigaction(SIGIO, &sa, NULL) == -1) {
            fprintf(stderr, "Error uninstalling SIGIO handler: %s\n",
                    strerror(errno));
            return;
        }
    }

    irqstatus = address;

    if (irqstatus) {
        sa.sa_handler = sig_io;

        if (sigaction(SIGIO, &sa, NULL) == -1) {
            fprintf(stderr, "Error installing SIGIO handler: %s\n",
                    strerror(errno));
            return;
        }

        if (fcntl(tunfd, F_SETFL, flags | O_ASYNC) == -1) {
            fprintf(stderr, "Error enabling SIGIO for network device: %s\n",
                    strerror(errno));
            return;
        }
    }
}

// r0: Reason code in r0
// r1: Pointer to buffer for any error string
// r2-r5: Reason code dependent
// Returns 0 in r0 on success, non 0 otherwise and error buffer filled in
void networkswi(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t *retr0, uint32_t *retr1)
{
    switch (r0) {
    case 0:
        *retr0 = tx(r1, r2, r3, r4, r5);
        break;
    case 1:
        *retr0 = rx(r1, r2, r3, retr1);
        break;
    case 2:
        set_irqstatus(r2);
        *retr0 = 0;
        break;
    case 3:
        if (poduleinfo) poduleinfo->irq = r2;
        rethinkpoduleints();
        *retr0 = 0;
        break;
    case 4:
       memcpyfromhost(r2, hwaddr, sizeof(hwaddr));
       *retr0 = 0;
       break;
    default:
        strcpyfromhost(r1, "Unknown RPCemu network SWI");
        *retr0 = r1;
    }
}

void initnetwork(void) 
{ 
    uid_t uid = 0;
    gid_t gid = 0;
    const char *user = config.username;
    /* Use configured username if available, otherwise see if
       we are running from a sudo command */
    if (user == NULL) user = getenv("SUDO_USER");

    tunfd = tun_alloc();
    if (tunfd == -1) fprintf(stderr, "Networking unavailable\n");

    /* Once the network has been configured we no longer need root
       privileges, so drop them if possible */
    if (user) {
        nametouidgid(user, &uid, &gid);
        if (dropprivileges(uid, gid) < 0) {
            fprintf(stderr, "Error dropping privileges: %s\n", strerror(errno));
        }
    }

    if (tunfd != -1) {
        poduleinfo = addpodule(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0);
        if (poduleinfo == NULL) fprintf(stderr, "No free podule for networking\n");
    }
}

