#ifndef ARM_COMMON_H
#define ARM_COMMON_H

#define checkneg(v)	(v & 0x80000000)
#define checkpos(v)	(!(v & 0x80000000))

static inline void
setadd(uint32_t op1, uint32_t op2, uint32_t result)
{
	uint32_t flags = 0;

	if (result == 0) {
		flags = ZFLAG;
	} else if (checkneg(result)) {
		flags = NFLAG;
	}
	if (result < op1) {
		flags |= CFLAG;
	}
	if ((op1 ^ result) & (op2 ^ result) & 0x80000000) {
		flags |= VFLAG;
	}
	*pcpsr = ((*pcpsr) & 0x0fffffff) | flags;
}

static inline void
setsub(uint32_t op1, uint32_t op2, uint32_t result)
{
	uint32_t flags = 0;

	if (result == 0) {
		flags = ZFLAG;
	} else if (checkneg(result)) {
		flags = NFLAG;
	}
	if (result <= op1) {
		flags |= CFLAG;
	}
	if ((op1 ^ op2) & (op1 ^ result) & 0x80000000) {
		flags |= VFLAG;
	}
	*pcpsr = ((*pcpsr) & 0x0fffffff) | flags;
}

static inline void
setsbc(uint32_t op1, uint32_t op2, uint32_t result)
{
	armregs[cpsr] &= ~0xf0000000;

	if (result == 0) {
		armregs[cpsr] |= ZFLAG;
	} else if (checkneg(result)) {
		armregs[cpsr] |= NFLAG;
	}
	if ((checkneg(op1) && checkpos(op2)) ||
	    (checkneg(op1) && checkpos(result)) ||
	    (checkpos(op2) && checkpos(result)))
	{
		armregs[cpsr] |= CFLAG;
	}
	if ((checkneg(op1) && checkpos(op2) && checkpos(result)) ||
	    (checkpos(op1) && checkneg(op2) && checkneg(result)))
	{
		armregs[cpsr] |= VFLAG;
	}
}

static inline void
setadc(uint32_t op1, uint32_t op2, uint32_t result)
{
	armregs[cpsr] &= ~0xf0000000;

	if (result == 0) {
		armregs[cpsr] |= ZFLAG;
	} else if (checkneg(result)) {
		armregs[cpsr] |= NFLAG;
	}
	if ((checkneg(op1) && checkneg(op2)) ||
	    (checkneg(op1) && checkpos(result)) ||
	    (checkneg(op2) && checkpos(result)))
	{
		armregs[cpsr] |= CFLAG;
	}
	if ((checkneg(op1) && checkneg(op2) && checkpos(result)) ||
	    (checkpos(op1) && checkpos(op2) && checkneg(result)))
	{
		armregs[cpsr] |= VFLAG;
	}
}

static inline void
setzn(uint32_t op)
{
	uint32_t flags;

	if (op == 0) {
		flags = ZFLAG;
	} else if (checkneg(op)) {
		flags = NFLAG;
	} else {
		flags = 0;
	}
	*pcpsr = flags | ((*pcpsr) & 0x3fffffff);
}

/**
 * Update the N and Z flags following a long multiply instruction.
 *
 * The Z flag will be set if the result equals 0.
 * The N flag will be set if the result has bit 63 set.
 *
 * @param result The result of the long multiply instruction
 */
static inline void
arm_flags_long_multiply(uint64_t result)
{
	uint32_t flags;

	if (result == 0) {
		flags = ZFLAG;
	} else {
		flags = 0;
	}

	/* N flag set if bit 63 of result is set.
	   N flag in CPSR is bit 31, so shift down by 32 */
	flags |= (((uint32_t) (result >> 32)) & NFLAG);

	*pcpsr = ((*pcpsr) & 0x3fffffff) | flags;
}

/**
 * Handle writes to Data Processing destination register with S flag clear
 *
 * @param opcode Opcode of instruction being emulated
 * @param dest   Value for destination register
 */
static inline void
arm_write_dest(uint32_t opcode, uint32_t dest)
{
	uint32_t rd = RD;

	if (rd == 15) {
		dest = ((dest + 4) & r15mask) | (armregs[15] & ~r15mask);
		refillpipeline();
	}
	armregs[rd] = dest;
}

#endif

