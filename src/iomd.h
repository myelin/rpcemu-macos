#ifndef __IOMD__
#define __IOMD__

/* The individual bits of the IRQ registers */
#define IOMD_IRQA_PARALLEL		0x01
// Unused				0x02
#define IOMD_IRQA_FLOPPY_INDEX		0x04
#define IOMD_IRQA_FLYBACK		0x08
#define IOMD_IRQA_POWER_ON_RESET	0x10
#define IOMD_IRQA_TIMER_0		0x20
#define IOMD_IRQA_TIMER_1		0x40
#define IOMD_IRQA_FORCE_BIT		0x80

#define IOMD_IRQB_PODULE_FIQ_AS_IRQ	0x01 /* Podule FIQ downgraded to IRQ */
#define IOMD_IRQB_IDE			0x02
// Unused				0x04
// Unused				0x08
#define IOMD_IRQB_FLOPPY		0x10
#define IOMD_IRQB_PODULE		0x20
#define IOMD_IRQB_KEYBOARD_TX		0x40
#define IOMD_IRQB_KEYBOARD_RX		0x80

/* IRQC and IRQD are ARM7500 and ARM7500FE only
   IRQC is unused */
#define IOMD_IRQD_MOUSE_RX		0x01
#define IOMD_IRQD_MOUSE_TX		0x02
// Unused				0x04
#define IOMD_IRQD_EVENT_1		0x08
#define IOMD_IRQD_EVENT_2		0x10
// Unused				0x20
// Unused				0x40
// Unused				0x80

/* Fast interupt register */
#define IOMD_FIQ_FLOPPY_DMA_REQUEST	0x01
// Unused				0x02
// Unused				0x04
// Unused				0x08
#define IOMD_FIQ_SERIAL			0x10
// Unused				0x20
#define IOMD_FIQ_PODULE			0x40
#define IOMD_FIQ_FORCE_BIT		0x80

/* IRQ DMA request register */
#define IOMD_IRQDMA_IO_0		0x01
#define IOMD_IRQDMA_IO_1		0x02
#define IOMD_IRQDMA_IO_2		0x04
#define IOMD_IRQDMA_IO_3		0x08
#define IOMD_IRQDMA_SOUND_0		0x10
#define IOMD_IRQDMA_SOUND_1		0x20
// Unused				0x40
// Unused				0x80

/* Bits within IOMD DMA status registers (applies to Sound DMA also) */
#define IOMD_DMA_STATUS_BUFFER		0x01	/**< A or B buffer indicator */
#define IOMD_DMA_STATUS_INTERRUPT	0x02
#define IOMD_DMA_STATUS_OVERRUN		0x04

/**
 * IOMD component supports various types of IOMD hardware
 */
typedef enum {
	IOMDType_IOMD,
	IOMDType_ARM7500,
	IOMDType_ARM7500FE
} IOMDType;

typedef struct {
        uint8_t status;
        uint8_t mask;
} iomd_irq;

typedef struct {
	uint32_t in_latch;
	int32_t  counter;
	uint32_t out_latch;
} iomd_timer;

struct iomd
{
        iomd_irq irqa;
        iomd_irq irqb;
        iomd_irq irqc;
        iomd_irq irqd;
        iomd_irq fiq;
        iomd_irq irqdma;
	uint8_t romcr0; /**< ROM Control 0 */
	uint8_t romcr1; /**< ROM Control 1 */
        uint32_t vidstart,vidend,vidcur,vidinit;
        iomd_timer t0;
        iomd_timer t1;
	uint8_t ctrl; /**< I/O Control */
        unsigned char vidcr;
        unsigned char sndstat;
	uint8_t refcr; /**< IOMD VRAM control, IOMD/7500/FE DRAM refresh speed */
	uint8_t iotcr; /**< I/O Timing Control */
	uint8_t ectcr; /**< Expansion card timing */

	/* IOMD21 only */
	uint16_t mousex; /**< Quadrature mouse X */
	uint16_t mousey; /**< Quadrature mouse Y */
	uint8_t  dmaext; /**< DMA external control */

	/* ARM7500/ARM7500FE only */
	uint8_t susmode; /**< SUSPEND Mode */
	uint8_t clkctrl; /**< Clock Control */
	uint8_t vidimux; /**< LCD and IIS control bits */
	uint8_t dramcr;  /**< DRAM Control */
	uint8_t selfref; /**< DRAM Self-Refresh Control */
};

extern struct iomd iomd;

extern  int kcallback,mcallback;

extern  uint32_t cinit;

extern void iomd_reset(IOMDType type);
extern void endiomd(void);
extern uint32_t readiomd(uint32_t addr);
extern void writeiomd(uint32_t addr, uint32_t val);
extern uint8_t mouse_buttons_read(void);
extern void iomdvsync(int vsync);

#endif //__IOMD__
