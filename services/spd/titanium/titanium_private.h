/*
 * Copyright (c) 2013-2017, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TITANIUM_PRIVATE_H
#define TITANIUM_PRIVATE_H

#include <platform_def.h>

#include <arch.h>
#include <bl31/interrupt_mgmt.h>
#include <context.h>
#include <lib/psci/psci.h>

/*******************************************************************************
 * TITANIUM PM state information e.g. TITANIUM is suspended, uninitialised etc
 * and macros to access the state information in the per-cpu 'state' flags
 ******************************************************************************/
#define TITANIUM_PSTATE_OFF		0
#define TITANIUM_PSTATE_ON			1
#define TITANIUM_PSTATE_SUSPEND		2
#define TITANIUM_PSTATE_SHIFT		0
#define TITANIUM_PSTATE_MASK		0x3
#define get_titanium_pstate(state)	((state >> TITANIUM_PSTATE_SHIFT) & \
				 TITANIUM_PSTATE_MASK)
#define clr_titanium_pstate(state)	(state &= ~(TITANIUM_PSTATE_MASK \
					    << TITANIUM_PSTATE_SHIFT))
#define set_titanium_pstate(st, pst) do {					       \
					clr_titanium_pstate(st);		       \
					st |= (pst & TITANIUM_PSTATE_MASK) <<     \
						TITANIUM_PSTATE_SHIFT;	       \
				} while (0)


/*******************************************************************************
 * TITANIUM execution state information i.e. aarch32 or aarch64
 ******************************************************************************/
#define TITANIUM_AARCH32		MODE_RW_32
#define TITANIUM_AARCH64		MODE_RW_64

/*******************************************************************************
 * The TITANIUM should know the type of TITANIUM
 ******************************************************************************/
#define TITANIUM_TYPE_UP		PSCI_TOS_NOT_UP_MIG_CAP
#define TITANIUM_TYPE_UPM		PSCI_TOS_UP_MIG_CAP
#define TITANIUM_TYPE_MP		PSCI_TOS_NOT_PRESENT_MP

/*******************************************************************************
 * TITANIUM migrate type information as known to the TITANIUM. We assume that
 * the TITANIUM is dealing with an MP Secure Payload.
 ******************************************************************************/
#define TITANIUM_MIGRATE_INFO		TITANIUM_TYPE_MP

/*******************************************************************************
 * Number of cpus that the present on this platform. TODO: Rely on a topology
 * tree to determine this in the future to avoid assumptions about mpidr
 * allocation
 ******************************************************************************/
#define TITANIUM_CORE_COUNT		PLATFORM_CORE_COUNT

/*******************************************************************************
 * Constants that allow assembler code to preserve callee-saved registers of the
 * C runtime context while performing a security state switch.
 ******************************************************************************/
#define TITANIUM_C_RT_CTX_X19		0x0
#define TITANIUM_C_RT_CTX_X20		0x8
#define TITANIUM_C_RT_CTX_X21		0x10
#define TITANIUM_C_RT_CTX_X22		0x18
#define TITANIUM_C_RT_CTX_X23		0x20
#define TITANIUM_C_RT_CTX_X24		0x28
#define TITANIUM_C_RT_CTX_X25		0x30
#define TITANIUM_C_RT_CTX_X26		0x38
#define TITANIUM_C_RT_CTX_X27		0x40
#define TITANIUM_C_RT_CTX_X28		0x48
#define TITANIUM_C_RT_CTX_X29		0x50
#define TITANIUM_C_RT_CTX_X30		0x58
#define TITANIUM_C_RT_CTX_SIZE		0x60
#define TITANIUM_C_RT_CTX_ENTRIES		(TITANIUM_C_RT_CTX_SIZE >> DWORD_SHIFT)

#ifndef __ASSEMBLER__

#include <stdint.h>

#include <lib/cassert.h>

#define SMC_IMM_KVM_TO_TITANIUM_TRAP 0x1
#define SMC_IMM_TITANIUM_TO_KVM_TRAP_SYNC 0x1
#define SMC_IMM_TITANIUM_TO_KVM_TRAP_IRQ 0x2
#define SMC_IMM_KVM_TO_TITANIUM_SHARED_MEMORY_REGISTER 0x10
#define SMC_IMM_KVM_TO_TITANIUM_SHARED_MEMORY_HANDLE 0x18
#define SMC_IMM_TITANIUM_TO_KVM_SHARED_MEMORY 0x10


typedef uint32_t titanium_vector_isn_t;

typedef struct titanium_vectors {
	titanium_vector_isn_t yield_smc_entry;
	titanium_vector_isn_t fast_smc_entry;
	titanium_vector_isn_t kvm_trap_smc_entry;
	titanium_vector_isn_t kvm_shared_memory_register_entry;
	titanium_vector_isn_t kvm_shared_memory_handle_entry;
	titanium_vector_isn_t cpu_on_entry;
	titanium_vector_isn_t cpu_off_entry;
	titanium_vector_isn_t cpu_resume_entry;
	titanium_vector_isn_t cpu_suspend_entry;
	titanium_vector_isn_t fiq_entry;
	titanium_vector_isn_t system_off_entry;
	titanium_vector_isn_t system_reset_entry;
} titanium_vectors_t;

/*
 * The number of arguments to save during a SMC call for TITANIUM.
 * Currently only x1 and x2 are used by TITANIUM.
 */
#define TITANIUM_NUM_ARGS	0x2

/* AArch64 callee saved general purpose register context structure. */
DEFINE_REG_STRUCT(c_rt_regs, TITANIUM_C_RT_CTX_ENTRIES);

/*
 * Compile time assertion to ensure that both the compiler and linker
 * have the same double word aligned view of the size of the C runtime
 * register context.
 */
CASSERT(TITANIUM_C_RT_CTX_SIZE == sizeof(c_rt_regs_t),	\
	assert_spd_c_rt_regs_size_mismatch);

/*******************************************************************************
 * Structure which helps the TITANIUM to maintain the per-cpu state of TITANIUM.
 * 'state'          - collection of flags to track TITANIUM state e.g. on/off
 * 'mpidr'          - mpidr to associate a context with a cpu
 * 'c_rt_ctx'       - stack address to restore C runtime context from after
 *                    returning from a synchronous entry into TITANIUM.
 * 'cpu_ctx'        - space to maintain TITANIUM architectural state
 ******************************************************************************/
typedef struct titanium_context {
	uint32_t state;
	uint64_t mpidr;
	uint64_t c_rt_ctx;
	cpu_context_t cpu_ctx;
} titanium_context_t;

/* TITANIUM power management handlers */
extern const spd_pm_ops_t titanium_pm;

/*******************************************************************************
 * Forward declarations
 ******************************************************************************/
struct titanium_vectors;

/*******************************************************************************
 * Function & Data prototypes
 ******************************************************************************/
uint64_t titanium_enter_sp(uint64_t *c_rt_ctx);
void __dead2 titanium_exit_sp(uint64_t c_rt_ctx, uint64_t ret);
uint64_t titanium_synchronous_sp_entry(titanium_context_t *titanium_ctx);
void __dead2 titanium_synchronous_sp_exit(titanium_context_t *titanium_ctx, uint64_t ret);
void titanium_init_titanium_ep_state(struct entry_point_info *titanium_entry_point,
				uint32_t rw,
				uint64_t pc,
				uint64_t pageable_part,
				uint64_t mem_limit,
				uint64_t dt_addr,
				titanium_context_t *titanium_ctx);

extern titanium_context_t titanium_sp_context[TITANIUM_CORE_COUNT];
extern uint32_t titanium_rw;
extern struct titanium_vectors *titanium_vector_table;
#endif /*__ASSEMBLER__*/

#endif /* TITANIUM_PRIVATE_H */
