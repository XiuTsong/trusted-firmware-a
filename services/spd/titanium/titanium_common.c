/*
 * Copyright (c) 2019, Xu Tianqiang. All rights reserved.
 *
 */

#include <assert.h>
#include <string.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/utils.h>

#include "titanium_private.h"

#include <stdio.h>

/*******************************************************************************
 * Given a TITANIUM entrypoint info pointer, entry point PC, register width,
 * cpu id & pointer to a context data structure, this function will
 * initialize TITANIUM context and entry point info for TITANIUM.
 ******************************************************************************/
//important!!!!!!!!
void titanium_init_titanium_ep_state(struct entry_point_info *titanium_entry_point,
				uint32_t rw, uint64_t pc,
				uint64_t pageable_part, uint64_t mem_limit,
				uint64_t dt_addr, titanium_context_t *titanium_ctx)
{
	uint32_t ep_attr;

	/* Passing a NULL context is a critical programming error */
	assert(titanium_ctx);
	assert(titanium_entry_point);
	assert(pc);

	printf("in titanium_init_titanium_ep_state titanium/titanium_common.c\n");

	printf("the vmpidr_el2: %p\n", (void*)read_vmpidr_el2());

	/* Associate this context with the cpu specified */
	titanium_ctx->mpidr = read_vmpidr_el2();
	titanium_ctx->state = 0;
	set_titanium_pstate(titanium_ctx->state, TITANIUM_PSTATE_OFF);

	cm_set_context(&titanium_ctx->cpu_ctx, SECURE);

	/* initialise an entrypoint to set up the CPU context */
	ep_attr = SECURE | EP_ST_ENABLE;
	if (read_sctlr_el3() & SCTLR_EE_BIT){
		ep_attr |= EP_EE_BIG;
		printf("surprisely?! it is big endian\n");
	}
	else{
		printf("hew! it is little endian\n");
	}
	SET_PARAM_HEAD(titanium_entry_point, PARAM_EP, VERSION_1, ep_attr);
	titanium_entry_point->pc = pc;
	if (rw == TITANIUM_AARCH64){
		printf("register width: 64\n");
		titanium_entry_point->spsr = SPSR_64(MODE_EL2, MODE_SP_ELX,
						  DISABLE_ALL_EXCEPTIONS);
	}
	else{
		printf("register width: 32\n");
		titanium_entry_point->spsr = SPSR_MODE32(MODE32_svc, SPSR_T_ARM,
						      SPSR_E_LITTLE,
						      DAIF_FIQ_BIT |
							DAIF_IRQ_BIT |
							DAIF_ABT_BIT);
	}
	zeromem(&titanium_entry_point->args, sizeof(titanium_entry_point->args));
	titanium_entry_point->args.arg0 = pageable_part;
	asm("mov %0, x27\n\t"
		: "+r"(titanium_entry_point->args.arg1)
		:
		:
	);
	//titanium_entry_point->args.arg1 = mem_limit;
	titanium_entry_point->args.arg2 = dt_addr;
}

/*******************************************************************************
 * This function takes an TITANIUM context pointer and:
 * 1. Applies the S-EL2 system register context from titanium_ctx->cpu_ctx.
 * 2. Saves the current C runtime state (callee saved registers) on the stack
 *    frame and saves a reference to this state.
 * 3. Calls el3_exit() so that the EL3 system and general purpose registers
 *    from the titanium_ctx->cpu_ctx are used to enter the TITANIUM image.
 ******************************************************************************/
uint64_t titanium_synchronous_sp_entry(titanium_context_t *titanium_ctx)
{
	uint64_t rc;

	assert(titanium_ctx != NULL);
	assert(titanium_ctx->c_rt_ctx == 0);

	/* Apply the Secure EL2 system register context and switch to it */
	assert(cm_get_context(SECURE) == &titanium_ctx->cpu_ctx);
	cm_el2_sysregs_context_restore(SECURE, 0);
	cm_set_next_eret_context(SECURE);
	
	rc = titanium_enter_sp(&titanium_ctx->c_rt_ctx);
#if ENABLE_ASSERTIONS
	titanium_ctx->c_rt_ctx = 0;
#endif

	return rc;
}


/*******************************************************************************
 * This function takes an TITANIUM context pointer and:
 * 1. Saves the S-EL2 system register context tp titanium_ctx->cpu_ctx.
 * 2. Restores the current C runtime state (callee saved registers) from the
 *    stack frame using the reference to this state saved in titanium_enter_sp().
 * 3. It does not need to save any general purpose or EL3 system register state
 *    as the generic smc entry routine should have saved those.
 ******************************************************************************/
void titanium_synchronous_sp_exit(titanium_context_t *titanium_ctx, uint64_t ret)
{
	assert(titanium_ctx != NULL);
	/* Save the Secure EL2 system register context */
	assert(cm_get_context(SECURE) == &titanium_ctx->cpu_ctx);
	cm_el2_sysregs_context_save(SECURE, 0);

	assert(titanium_ctx->c_rt_ctx != 0);
	titanium_exit_sp(titanium_ctx->c_rt_ctx, ret);

	/* Should never reach here */
	assert(0);
}
