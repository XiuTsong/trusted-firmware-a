/*
 * Copyright (c) 2019, Xu Tianqiang. All rights reserved.
 *
 */


/*******************************************************************************
 * This is the Secure Payload Dispatcher (SPD). The dispatcher is meant to be a
 * plug-in component to the Secure Monitor, registered as a runtime service. The
 * SPD is expected to be a functional extension of the Secure Payload (SP) that
 * executes in Secure EL1. The Secure Monitor will delegate all SMCs targeting
 * the Trusted OS/Applications range to the dispatcher. The SPD will either
 * handle the request locally or delegate it to the Secure Payload. It is also
 * responsible for initialising and maintaining communication with the SP.
 ******************************************************************************/
#include <assert.h>
#include <errno.h>
#include <stddef.h>

#include <arch.h>
#include <arch_helpers.h>
#include <bl31/bl31.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/runtime_svc.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <plat/common/platform.h>
#include <tools_share/uuid.h>

#include "common/ep_info.h"
#include "titanium_private.h"
#include "teesmc_titanium.h"
#include "teesmc_titanium_macros.h"
#include "titanium_vm_exit_defs.h"

/*******************************************************************************
 * Address of the entrypoint vector table in TITANIUM. It is
 * initialised once on the primary core after a cold boot.
 ******************************************************************************/
struct titanium_vectors *titanium_vector_table;

/*******************************************************************************
 * Array to keep track of per-cpu TITANIUM state
 ******************************************************************************/
titanium_context_t titanium_sp_context[TITANIUM_CORE_COUNT];
uint32_t titanium_rw;

static int32_t titanium_init(void);

#define VAL_EXTRACT_BITS(data, start, end) ((data >> start) & ((1ul << (end-start+1))-1))
#define INTR_TYPE_S_EL2                      U(3)

void read_curEL_with_string(char *s){
	unsigned long ret = 0;
	unsigned long data = 0;

	//read CurrentEL
	asm(
		"MRS %0, CurrentEL\n\t"
		: "+r"(ret)
		:
		:
	);

	data = VAL_EXTRACT_BITS(ret, 2, 4);

	printf("The CurrentEL.EL %s is : %lu\n", s, data);
}

void cm_set_sre_el1(uint32_t security_state, uint64_t sre_value)
{
	cpu_context_t *ctx;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	ctx->icc_sre_el1 = sre_value;
}


/*******************************************************************************
 * This function retrieves vbar_el2 member of 'cpu_context' pertaining to the
 * given security state.
 ******************************************************************************/
uint64_t cm_get_vbar_el2(uint32_t security_state)
{
	cpu_context_t *ctx;
	el2_sys_regs_t *state;

	ctx = cm_get_context(security_state);
	assert(ctx != NULL);

	state = get_el2_sysregs_ctx(ctx);
	return (uint64_t)read_ctx_reg(state, CTX_VBAR_EL2);
}




//SPD: Secure Payload Dispatcher

/*******************************************************************************
 * This function is the handler registered for S-EL1 interrupts by the
 * TITANIUM. It validates the interrupt and upon success arranges entry into
 * the TITANIUM at 'titanium_fiq_entry()' for handling the interrupt.
 ******************************************************************************/
static uint64_t titanium_sel2_interrupt_handler(uint32_t id,
					    uint32_t flags,
					    void *handle,
					    void *cookie)
{
	uint32_t linear_id;
	titanium_context_t *titanium_ctx;

	/* Check the security state when the exception was generated */
	assert(get_interrupt_src_ss(flags) == NON_SECURE);

	/* Sanity check the pointer to this cpu's context */
	assert(handle == cm_get_context(NON_SECURE));

	/* Save the non-secure context before entering the TITANIUM */
#ifndef DISABLE_SEL2
	cm_el2_sysregs_context_save(NON_SECURE, 0);
#else
	cm_el1_sysregs_context_save(NON_SECURE);
#endif

	/* Get a reference to this cpu's TITANIUM context */
	linear_id = plat_my_core_pos();
	titanium_ctx = &titanium_sp_context[linear_id];
	assert(&titanium_ctx->cpu_ctx == cm_get_context(SECURE));

	cm_set_elr_el3(SECURE, (uint64_t)&titanium_vector_table->fiq_entry);
#ifndef DISABLE_SEL2
	cm_el2_sysregs_context_restore(SECURE, 0);
#else
	cm_el1_sysregs_context_restore(SECURE);
#endif
	cm_set_next_eret_context(SECURE);

	/*read_actlr_el2
	 * Tell the TITANIUM that it has to handle an FIQ (synchronously).
	 * Also the instruction in normal world where the interrupt was
	 * generated is passed for debugging purposes. It is safe to
	 * retrieve this address from ELR_EL3 as the secure context will
	 * not take effect until el3_exit().
	 */
	SMC_RET1(&titanium_ctx->cpu_ctx, read_elr_el3());
}

/*******************************************************************************
 * TITANIUM Dispatcher setup. The TITANIUM finds out the TITANIUM entrypoint and type
 * (aarch32/aarch64) if not already known and initialises the context for entry
 * into TITANIUM for its initialization.
 ******************************************************************************/
//bl31_main jumps here!
static int32_t titanium_setup(void)
{
	entry_point_info_t *titanium_ep_info;
	uint32_t linear_id;
	uint64_t titanium_pageable_part;
	uint64_t titanium_mem_limit;
	uint64_t dt_addr;

	printf("in titanium_setup in titanium_main.c\n");
	printf("the titanium_setup function: %p\n", (void *)titanium_setup);

	read_curEL_with_string("titanium_setup");

	/* Get a reference to this cpu's TITANIUM context */
	linear_id = plat_my_core_pos();
	printf("the linear_id: %u\n", linear_id);
	/*
	 * Get information about the Secure Payload (BL32) image. Its
	 * absence is a critical failure.  TODO: Add support to
	 * conditionally include the SPD service
	 */
	titanium_ep_info = bl31_plat_get_next_image_ep_info(SECURE);
	printf("titanium_ep_info: %p\n", (void *)titanium_ep_info);
	if (!titanium_ep_info) {
		WARN("No TITANIUM provided by BL2 boot loader, Booting device"
			" without TITANIUM initialization. SMC`s destined for TITANIUM"
			" will return SMC_UNK\n");
		return 1;
	}

	/*
	 * If there's no valid entry point for SP, we return a non-zero value
	 * signalling failure initializing the service. We bail out without
	 * registering any handlers
	 */
	if (!titanium_ep_info->pc)
		return 1;

	titanium_rw = titanium_ep_info->args.arg0;
	titanium_pageable_part = titanium_ep_info->args.arg1;
	titanium_mem_limit = titanium_ep_info->args.arg2;
	dt_addr = titanium_ep_info->args.arg3;

	printf("titanium_rw: %u | titanium_ep_info->pc: %p | titanium_pageable_part: %p | titanium_mem_limit: %p | dt_addr: %p\n",
		titanium_rw, (void *)titanium_ep_info->pc, (void*)titanium_pageable_part, (void*)titanium_mem_limit, (void*)dt_addr);

	titanium_init_titanium_ep_state(titanium_ep_info,
				titanium_rw,
				titanium_ep_info->pc,
				titanium_pageable_part,
				titanium_mem_limit,
				dt_addr,
				&titanium_sp_context[linear_id]);

	/*
	 * All TITANIUM initialization done. Now register our init function with
	 * BL31 for deferred invocation
	 */
	bl31_register_bl32_init(&titanium_init);

	return 0;
}

#ifdef DISABLE_SEL2
static inline void cleanup_el1_sys_registers() {
	/* clean up all el1 registertcrs */
	__asm__ volatile ("msr spsr_el1, xzr");
	__asm__ volatile ("msr elr_el1, xzr");
	__asm__ volatile ("msr sctlr_el1, xzr");
	// __asm__ volatile ("msr sp_el1, xzr");
	// __asm__ volatile ("msr sp_el0, xzr");
	// __asm__ volatile ("msr esr_el1, xzr");
	__asm__ volatile ("msr vbar_el1, xzr");
	__asm__ volatile ("msr ttbr0_el1, xzr");
	__asm__ volatile ("msr ttbr1_el1, xzr");
	__asm__ volatile ("msr mair_el1, xzr");
	__asm__ volatile ("msr amair_el1, xzr");
	__asm__ volatile ("msr tcr_el1, xzr");
	__asm__ volatile ("msr tpidr_el1, xzr");
}
#endif

/*******************************************************************************
 * This function passes control to the TITANIUM image (BL32) for the first time
 * on the primary cpu after a cold boot. It assumes that a valid secure
 * context has already been created by titanium_setup() which can be directly
 * used.  It also assumes that a valid non-secure context has been
 * initialised by PSCI so it does not need to save and restore any
 * non-secure state. This function performs a synchronous entry into
 * TITANIUM. TITANIUM passes control back to this routine through a SMC.
 ******************************************************************************/
static int32_t titanium_init(void)
{
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];
	entry_point_info_t *titanium_entry_point;
	uint64_t rc;
    uint32_t scr_el3;
//	, spsr_el3;
//	uint32_t cptr_el2, sctlr_el2;
//	uint64_t hcr_el2 = 0;

	printf("in titanium_init in spd/titanium/titanium_main.c\n");

	/*
	 * Get information about the TITANIUM (BL32) image. Its
	 * absence is a critical failure.
	 */
	//get titanium entry_point and this entry point is the titanium os entry point
	titanium_entry_point = bl31_plat_get_next_image_ep_info(SECURE);
	assert(titanium_entry_point);

	cm_init_my_context(titanium_entry_point);

	scr_el3 = (uint32_t)read_scr();
	scr_el3 |= SCR_EEL2_BIT;
	scr_el3 |= SCR_HCE_BIT;
	scr_el3 &= ~SCR_FIQ_BIT;  //do not trap FIQ to el3
	scr_el3 &= ~SCR_IRQ_BIT;  //do not trap RIQ to el3
	write_scr(scr_el3);

#ifdef DISABLE_SEL2
	cleanup_el1_sys_registers();
#endif
	//plat_ic_set_interrupt_type(27, INTR_TYPE_S_EL1);
	/*
	 * Arrange for an entry into TITANIUM. It will be returned via
	 * TITANIUM_ENTRY_DONE case
	 */
	rc = titanium_synchronous_sp_entry(titanium_ctx);
	assert(rc != 0);

	return rc;
}

#ifdef DISABLE_SEL2
static void pass_el2_return_state_to_el1(titanium_context_t *titanium_ctx) {
	uint64_t elr_el2 = read_elr_el2();
	uint64_t spsr_el2 = read_spsr_el2();

	/* Pass ELR_EL2 to ELR_EL1 */
	write_ctx_reg(get_el1_sysregs_ctx(&titanium_ctx->cpu_ctx),
			CTX_ELR_EL1,
			elr_el2);
	// printf("ELR_EL2: %llx\n", elr_el2);
	// printf("ELR_EL1: %llx\n", read_ctx_reg(get_el1_sysregs_ctx(&titanium_ctx->cpu_ctx), CTX_ELR_EL1));

	/* Pass SPSR_EL2 to SPSR_EL1 */
	write_ctx_reg(get_el1_sysregs_ctx(&titanium_ctx->cpu_ctx),
			CTX_SPSR_EL1,
			spsr_el2);
	// printf("SPSR_EL1: %llx\n", read_ctx_reg(get_el1_sysregs_ctx(&titanium_ctx->cpu_ctx), CTX_SPSR_EL1));
	// printf("SPSR_EL2: %llx\n", spsr_el2);
}

static void pass_el1_return_state_to_el2(titanium_context_t *titanium_ctx) {
	uint64_t elr_el1 = read_elr_el1();
	uint64_t spsr_el1 = read_spsr_el1();

	/* Pass ELR_EL1 to ELR_EL2 */
	// write_ctx_reg(get_el2_sysregs_ctx(&titanium_ctx->cpu_ctx),
	// 		CTX_ELR_EL2,
	// 		elr_el1);
	write_elr_el2(elr_el1);

	/* Pass SPSR_EL1 to SPSR_EL2 */
	// write_ctx_reg(get_el2_sysregs_ctx(&titanium_ctx->cpu_ctx),
	// 		CTX_SPSR_EL2,
	// 		spsr_el1);
	write_spsr_el2(spsr_el1);
}

static void pass_el1_fault_state_to_el2(titanium_context_t *titanium_ctx) {
	unsigned long esr_el1 = read_esr_el1();
	unsigned long far_el1 = read_far_el1();
	unsigned long hpfar_el2 = read_far_el1();
    uint64_t kvm_exit_reason = ESR_EL_EC(esr_el1);
	/* Pass ESR_EL1 to ESR_EL2 */
    if (kvm_exit_reason == ESR_ELx_EC_IABT_CUR ||
            kvm_exit_reason == ESR_ELx_EC_DABT_CUR) {
		/* Change IABT/DABT_CUR to IABT/DABT_LOW.
		 * Just clear EC bits[0], or ESR_EL bits[26]
		 */
		esr_el1 &= (~BIT(26));
	}
	write_esr_el2(esr_el1);
	printf("ESR_EL1: %lx\n", esr_el1);
	printf("ESR_EL2: %lx\n", read_esr_el2());

	/* Pass FAR_EL1 to HPFAR_EL2 */
	hpfar_el2 = far_el1 >> 8;
	write_hpfar_el2(hpfar_el2);
	printf("FAR_EL1: %lx\n", far_el1);
	printf("HPFAR_EL2: %lx\n", read_hpfar_el2());
}
#endif

long enter_titanium_count = 0;
long leave_titanium_count = 0;
/*******************************************************************************
 * This function is responsible for handling all SMCs in the Trusted OS/App
 * range from the non-secure state as defined in the SMC Calling Convention
 * Document. It is also responsible for communicating with the Secure
 * payload to delegate work and return results back to the non-secure
 * state. Lastly it will also return any information that TITANIUM needs to do
 * the work assigned to it.
 ******************************************************************************/
static uintptr_t titanium_smc_handler(uint32_t smc_fid,
			 u_register_t x1,
			 u_register_t x2,
			 u_register_t x3,
			 u_register_t x4,
			 void *cookie,
			 void *handle,
			 u_register_t flags)
{
	cpu_context_t *ns_cpu_context;
	uint32_t smc_imm = 0;
	uint32_t exit_value = 0;
	uint32_t is_kvm_trap = 0;
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];
	uint64_t rc;
	uint32_t icc_sre_el1;
	/*
	 * Determine which security state this SMC originated from
	 */

	smc_imm = read_esr_el3() & 0xffff;

	if (smc_imm != 0) {
		is_kvm_trap = 1;
		exit_value = smc_imm - 1;
	}

	if (is_caller_non_secure(flags)) {
		/*
		 * This is a fresh request from the non-secure client.
		 * The parameters are in x1 and x2. Figure out which
		 * registers need to be preserved, save the non-secure
		 * state and send the request to the secure payload.
		 */
		assert(handle == cm_get_context(NON_SECURE));

		if (is_kvm_trap == 1) {

#ifndef DISABLE_SEL2
			cm_el2_sysregs_context_save(NON_SECURE, 1);
#else
			cm_el1_sysregs_context_save(NON_SECURE);
			cm_el2_sysregs_context_save(NON_SECURE, 1);
#endif

			icc_sre_el1 = (uint32_t)read_icc_sre_el1();
			icc_sre_el1 &= ~(1UL);  //clear SRE bit
//			write_icc_sre_el1(icc_sre_el1);
			cm_set_sre_el1(SECURE, (uint64_t)icc_sre_el1);

		} else {
			cm_set_sre_el1(SECURE, 0);
#ifndef DISABLE_SEL2
			cm_el2_sysregs_context_save(NON_SECURE, 0);
#else
			cm_el1_sysregs_context_save(NON_SECURE);
			cm_el2_sysregs_context_save(NON_SECURE, 0);
#endif
		}

		/*
		 * We are done stashing the non-secure context. Ask the
		 * TITANIUM to do the work now.
		 */

		/*
		 * Verify if there is a valid context to use, copy the
		 * operation type and parameters to the secure context
		 * and jump to the fast smc entry point in the secure
		 * payload. Entry into S-EL1 will take place upon exit
		 * from this function.
		 */
		assert(&titanium_ctx->cpu_ctx == cm_get_context(SECURE));


		/* Set appropriate entry for SMC.
		*/
		if (is_kvm_trap == 1) {
			switch (smc_imm) {
				case SMC_IMM_KVM_TO_TITANIUM_TRAP:
					cm_set_elr_el3(SECURE, (uint64_t)
						&titanium_vector_table->kvm_trap_smc_entry);
					break;
				case SMC_IMM_KVM_TO_TITANIUM_SHARED_MEMORY_REGISTER:
					cm_set_elr_el3(SECURE, (uint64_t)
						&titanium_vector_table->kvm_shared_memory_register_entry);
					break;
				case SMC_IMM_KVM_TO_TITANIUM_SHARED_MEMORY_HANDLE:
					cm_set_elr_el3(SECURE, (uint64_t)
						&titanium_vector_table->kvm_shared_memory_handle_entry);
					break;
				default:
					panic();
			}
		} else if (GET_SMC_TYPE(smc_fid) == SMC_TYPE_FAST) {
			cm_set_elr_el3(SECURE, (uint64_t)
					&titanium_vector_table->fast_smc_entry);
		} else {
			cm_set_elr_el3(SECURE, (uint64_t)
					&titanium_vector_table->yield_smc_entry);
		}

#ifndef DISABLE_SEL2
		if (is_kvm_trap == 1) {
			cm_el2_sysregs_context_restore(SECURE, 1);
		} else {
			cm_el2_sysregs_context_restore(SECURE, 0);
		}
#else
		if (smc_imm == SMC_IMM_KVM_TO_TITANIUM_TRAP) {
			pass_el2_return_state_to_el1(titanium_ctx);
		}
		cm_el1_sysregs_context_restore(SECURE);
#endif

		cm_set_next_eret_context(SECURE);

		if (is_kvm_trap == 1) {
			switch (smc_imm) {
				case SMC_IMM_KVM_TO_TITANIUM_TRAP:
				case SMC_IMM_KVM_TO_TITANIUM_SHARED_MEMORY_REGISTER:
					memcpy(get_gpregs_ctx(&titanium_ctx->cpu_ctx),
						get_gpregs_ctx(handle), sizeof(gp_regs_t));
					break;
				case SMC_IMM_KVM_TO_TITANIUM_SHARED_MEMORY_HANDLE:
					break;
				default:
					panic();
			}

			SMC_RET0(&titanium_ctx->cpu_ctx);
		} else {
			write_ctx_reg(get_gpregs_ctx(&titanium_ctx->cpu_ctx),
					CTX_GPREG_X4,
					read_ctx_reg(get_gpregs_ctx(handle),
						CTX_GPREG_X4));
			write_ctx_reg(get_gpregs_ctx(&titanium_ctx->cpu_ctx),
					CTX_GPREG_X5,
					read_ctx_reg(get_gpregs_ctx(handle),
						CTX_GPREG_X5));
			write_ctx_reg(get_gpregs_ctx(&titanium_ctx->cpu_ctx),
					CTX_GPREG_X6,
					read_ctx_reg(get_gpregs_ctx(handle),
						CTX_GPREG_X6));
			/* Propagate hypervisor client ID */
			write_ctx_reg(get_gpregs_ctx(&titanium_ctx->cpu_ctx),
					CTX_GPREG_X7,
					read_ctx_reg(get_gpregs_ctx(handle),
						CTX_GPREG_X7));

		}
		SMC_RET4(&titanium_ctx->cpu_ctx, smc_fid, x1, x2, x3);
	}

	/*
	 * Returning from TITANIUM
	 */

	/* set this value to 0 to let el3_exit to not change ICC_SRE_EL1 */
	cm_set_sre_el1(NON_SECURE, 0);

	if (is_kvm_trap == 1) {

#ifndef DISABLE_SEL2
		cm_el2_sysregs_context_save(SECURE, 1);
#else
		cm_el1_sysregs_context_save(SECURE);
		pass_el1_return_state_to_el2(titanium_ctx);
		pass_el1_fault_state_to_el2(titanium_ctx);
#endif

		/* Get a reference to the non-secure context */
		ns_cpu_context = cm_get_context(NON_SECURE);
		assert(ns_cpu_context);

		/* Restore icc_sre_el1 and set SRE value */
		cm_set_sre_el1(NON_SECURE, 0);

		/* Restore non-secure state */

#ifndef DISABLE_SEL2
		cm_el2_sysregs_context_restore(NON_SECURE, 1);
#else
		cm_el1_sysregs_context_restore(NON_SECURE);
#endif
		cm_set_next_eret_context(NON_SECURE);
		switch (smc_imm) {
			case SMC_IMM_TITANIUM_TO_KVM_TRAP_SYNC: case SMC_IMM_TITANIUM_TO_KVM_TRAP_IRQ:
				memcpy(get_gpregs_ctx(ns_cpu_context), get_gpregs_ctx(handle), sizeof(gp_regs_t));
				cm_set_elr_el3(NON_SECURE, (uint64_t)cm_get_vbar_el2(NON_SECURE) + (8+exit_value) * 0x80);//skip the first eight handler
				break;
			case SMC_IMM_TITANIUM_TO_KVM_SHARED_MEMORY:
				//printf("jump away from save titanium registers\n");
				//printf("jump away from redirect elr registers to vbar addr\n");
				break;
			default:
				panic();
		}

//		cm_set_elr_el3(NON_SECURE, (uint64_t)cm_get_elr_el3(NON_SECURE));
		SMC_RET0(ns_cpu_context);
	}

	switch (smc_fid) {
		/*
		 * TITANIUM has finished initialising itself after a cold boot
		 */
		case TEESMC_TITANIUM_RETURN_ENTRY_DONE:
			/*
			 * Stash the TITANIUM entry points information. This is done
			 * only once on the primary cpu
			 */
			assert(titanium_vector_table == NULL);
			titanium_vector_table = (titanium_vectors_t *) x1;

			if (titanium_vector_table) {
				set_titanium_pstate(titanium_ctx->state, TITANIUM_PSTATE_ON);

				/*
				 * TITANIUM has been successfully initialized.
				 * Register power management hooks with PSCI
				 */
				psci_register_spd_pm_hook(&titanium_pm);

				/*
				 * Register an interrupt handler for S-EL1 interrupts
				 * when generated during code executing in the
				 * non-secure state.
				 */
				flags = 0;
				set_interrupt_rm_flag(flags, NON_SECURE);
				rc = register_interrupt_type_handler(INTR_TYPE_S_EL2,
						titanium_sel2_interrupt_handler,
						flags);
				if (rc)
					panic();
			}

			/*
			 * TITANIUM reports completion. The TITANIUM must have initiated
			 * the original request through a synchronous entry into
			 * TITANIUM. Jump back to the original C runtime context.
			 */
			titanium_synchronous_sp_exit(titanium_ctx, x1);
			break;


			/*
			 * These function IDs is used only by OP-TEE to indicate it has
			 * finished:
			 * 1. turning itself on in response to an earlier psci
			 *    cpu_on request
			 * 2. resuming itself after an earlier psci cpu_suspend
			 *    request.
			 */
		case TEESMC_TITANIUM_RETURN_ON_DONE:
		case TEESMC_TITANIUM_RETURN_RESUME_DONE:


			/*
			 * These function IDs is used only by the SP to indicate it has
			 * finished:
			 * 1. suspending itself after an earlier psci cpu_suspend
			 *    request.
			 * 2. turning itself off in response to an earlier psci
			 *    cpu_off request.
			 */
		case TEESMC_TITANIUM_RETURN_OFF_DONE:
		case TEESMC_TITANIUM_RETURN_SUSPEND_DONE:
		case TEESMC_TITANIUM_RETURN_SYSTEM_OFF_DONE:
		case TEESMC_TITANIUM_RETURN_SYSTEM_RESET_DONE:

			/*
			 * TITANIUM reports completion. The TITANIUM must have initiated the
			 * original request through a synchronous entry into TITANIUM.
			 * Jump back to the original C runtime context, and pass x1 as
			 * return value to the caller
			 */
			titanium_synchronous_sp_exit(titanium_ctx, x1);
			break;

			/*
			 * TITANIUM is returning from a call or being preempted from a call, in
			 * either case execution should resume in the normal world.
			 */
		case TEESMC_TITANIUM_RETURN_CALL_DONE:
			/*
			 * This is the result from the secure client of an
			 * earlier request. The results are in x0-x3. Copy it
			 * into the non-secure context, save the secure state
			 * and return to the non-secure state.
			 */
			assert(handle == cm_get_context(SECURE));

#ifndef DISABLE_SEL2
			cm_el2_sysregs_context_save(SECURE, 0);
#else
			cm_el1_sysregs_context_save(SECURE);
#endif

			/* Get a reference to the non-secure context */
			ns_cpu_context = cm_get_context(NON_SECURE);
			assert(ns_cpu_context);

			/* Restore non-secure state */

#ifndef DISABLE_SEL2
			cm_el2_sysregs_context_restore(NON_SECURE, 0);
#else
			cm_el1_sysregs_context_restore(NON_SECURE);
#endif
			cm_set_next_eret_context(NON_SECURE);

			SMC_RET4(ns_cpu_context, x1, x2, x3, x4);

			/*
			 * TITANIUM has finished handling a S-EL1 FIQ interrupt. Execution
			 * should resume in the normal world.
			 */
		case TEESMC_TITANIUM_RETURN_FIQ_DONE:
			/* Get a reference to the non-secure context */
			ns_cpu_context = cm_get_context(NON_SECURE);
			assert(ns_cpu_context);

			/*
			 * Restore non-secure state. There is no need to save the
			 * secure system register context since TITANIUM was supposed
			 * to preserve it during S-EL1 interrupt handling.
			 */

#ifndef DISABLE_SEL2
			cm_el1_sysregs_context_restore(NON_SECURE);
#else
			cm_set_next_eret_context(NON_SECURE);
#endif

			SMC_RET0((uint64_t) ns_cpu_context);

		default:
			panic();
	}
}

/* Define an TITANIUM runtime service descriptor for fast SMC calls */
DECLARE_RT_SVC(
	titanium_fast,

	OEN_TOS_START,
	OEN_TOS_END,
	SMC_TYPE_FAST,
	titanium_setup,
	titanium_smc_handler
);

/* Define an TITANIUM runtime service descriptor for yielding SMC calls */
DECLARE_RT_SVC(
	titanium_std,

	OEN_TOS_START,
	OEN_TOS_END,
	SMC_TYPE_YIELD,
	NULL,
	titanium_smc_handler
);
