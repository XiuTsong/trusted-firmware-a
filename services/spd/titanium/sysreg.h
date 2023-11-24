#pragma once
/*
 * ARMv8 ARM reserves the following encoding for system registers:
 * (Ref: ARMv8 ARM, Section: "System instruction class encoding overview",
 *  C5.2, version:ARM DDI 0487A.f)
 *	[20-19] : Op0
 *	[18-16] : Op1
 *	[15-12] : CRn
 *	[11-8]  : CRm
 *	[7-5]   : Op2
 */
#define Op0_shift	19
#define Op0_mask	0x3
#define Op1_shift	16
#define Op1_mask	0x7
#define CRn_shift	12
#define CRn_mask	0xf
#define CRm_shift	8
#define CRm_mask	0xf
#define Op2_shift	5
#define Op2_mask	0x7

#define sys_reg(op0, op1, crn, crm, op2) \
	(((op0) << Op0_shift) | ((op1) << Op1_shift) | \
	 ((crn) << CRn_shift) | ((crm) << CRm_shift) | \
	 ((op2) << Op2_shift))

#define __SYS__LR0_EL2(x)		sys_reg(3, 4, 12, 12, x)
#define SYS_ICH_LR0_EL2			__SYS__LR0_EL2(0)
#define SYS_ICH_LR1_EL2			__SYS__LR0_EL2(1)
#define SYS_ICH_LR2_EL2			__SYS__LR0_EL2(2)
#define SYS_ICH_LR3_EL2			__SYS__LR0_EL2(3)
#define SYS_ICH_LR4_EL2			__SYS__LR0_EL2(4)
#define SYS_ICH_LR5_EL2			__SYS__LR0_EL2(5)
#define SYS_ICH_LR6_EL2			__SYS__LR0_EL2(6)
#define SYS_ICH_LR7_EL2			__SYS__LR0_EL2(7)
#define SYS_ICC_EOIR1_EL1		sys_reg(3, 0, 12, 12, 1)
#define SYS_ICC_IAR1_EL1		sys_reg(3, 0, 12, 12, 0)

#define __stringify_1(x...)	#x
#define __stringify(x...)	__stringify_1(x)

#define __emit_inst(x)          ".inst " __stringify((x)) "\n\t"

asm(
"	.irp	num,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30\n"
"	.equ	.L__reg_num_x\\num, \\num\n"
"	.endr\n"
"	.equ	.L__reg_num_xzr, 31\n"
"\n"
"	.macro	mrs_s, rt, sreg\n"
	__emit_inst(0xd5200000|(\\sreg)|(.L__reg_num_\\rt))
"	.endm\n"
"\n"
"	.macro	msr_s, sreg, rt\n"
	__emit_inst(0xd5000000|(\\sreg)|(.L__reg_num_\\rt))
"	.endm\n"
);
/*
 * For registers without architectural names, or simply unsupported by
 * GAS.
 */
#define read_sysreg_s(r) ({						\
	uint64_t __val;							\
	asm volatile("mrs_s %0, " __stringify(r) : "=r" (__val));	\
	__val;								\
})

#define write_sysreg_s(v, r) do {					\
	uint64_t __val = (uint64_t)(v);						\
	asm volatile("msr_s " __stringify(r) ", %x0" : : "rZ" (__val));	\
} while (0)
