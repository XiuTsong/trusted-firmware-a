/*
 * Copyright (c) 2013-2017, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <plat/common/platform.h>

#include "titanium_private.h"

/*******************************************************************************
 * The target cpu is being turned on. Allow the TITANIUM to perform any
 * actions needed. Nothing at the moment.
 ******************************************************************************/
static void titanium_cpu_on_handler(u_register_t target_cpu)
{
}

/*******************************************************************************
 * This cpu is being turned off. Allow the TITANIUM to perform any actions
 * needed
 ******************************************************************************/
static int32_t titanium_cpu_off_handler(u_register_t unused)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];

	assert(titanium_vector_table);
	assert(get_titanium_pstate(titanium_ctx->state) == TITANIUM_PSTATE_ON);

	/* Program the entry point and enter TITANIUM */
	cm_set_elr_el3(SECURE, (uint64_t) &titanium_vector_table->cpu_off_entry);
	rc = titanium_synchronous_sp_entry(titanium_ctx);

	/*
	 * Read the response from TITANIUM. A non-zero return means that
	 * something went wrong while communicating with TITANIUM.
	 */
	if (rc != 0)
		panic();

	/*
	 * Reset TITANIUM's context for a fresh start when this cpu is turned on
	 * subsequently.
	 */
	set_titanium_pstate(titanium_ctx->state, TITANIUM_PSTATE_OFF);

	 return 0;
}

/*******************************************************************************
 * This cpu is being suspended. S-EL1 state must have been saved in the
 * resident cpu (mpidr format) if it is a UP/UP migratable TITANIUM.
 ******************************************************************************/
static void titanium_cpu_suspend_handler(u_register_t max_off_pwrlvl)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];

	assert(titanium_vector_table);
	assert(get_titanium_pstate(titanium_ctx->state) == TITANIUM_PSTATE_ON);

	write_ctx_reg(get_gpregs_ctx(&titanium_ctx->cpu_ctx), CTX_GPREG_X0,
		      max_off_pwrlvl);

	/* Program the entry point and enter TITANIUM */
	cm_set_elr_el3(SECURE, (uint64_t) &titanium_vector_table->cpu_suspend_entry);
	rc = titanium_synchronous_sp_entry(titanium_ctx);

	/*
	 * Read the response from TITANIUM. A non-zero return means that
	 * something went wrong while communicating with TITANIUM.
	 */
	if (rc != 0)
		panic();

	/* Update its context to reflect the state TITANIUM is in */
	set_titanium_pstate(titanium_ctx->state, TITANIUM_PSTATE_SUSPEND);
}

/*******************************************************************************
 * This cpu has been turned on. Enter TITANIUM to initialise S-EL1 and other bits
 * before passing control back to the Secure Monitor. Entry in S-El1 is done
 * after initialising minimal architectural state that guarantees safe
 * execution.
 ******************************************************************************/
static void titanium_cpu_on_finish_handler(u_register_t unused)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];
	entry_point_info_t titanium_on_entrypoint;

	assert(titanium_vector_table);
	assert(get_titanium_pstate(titanium_ctx->state) == TITANIUM_PSTATE_OFF);

	titanium_init_titanium_ep_state(&titanium_on_entrypoint, titanium_rw,
				(uint64_t)&titanium_vector_table->cpu_on_entry,
				0, 0, 0, titanium_ctx);

	/* Initialise this cpu's secure context */
	cm_init_my_context(&titanium_on_entrypoint);

	/* Enter TITANIUM */
	rc = titanium_synchronous_sp_entry(titanium_ctx);

	/*
	 * Read the response from TITANIUM. A non-zero return means that
	 * something went wrong while communicating with TITANIUM.
	 */
	if (rc != 0)
		panic();

	/* Update its context to reflect the state TITANIUM is in */
	set_titanium_pstate(titanium_ctx->state, TITANIUM_PSTATE_ON);
}

/*******************************************************************************
 * This cpu has resumed from suspend. The TITANIUM saved the TITANIUM context when it
 * completed the preceding suspend call. Use that context to program an entry
 * into TITANIUM to allow it to do any remaining book keeping
 ******************************************************************************/
static void titanium_cpu_suspend_finish_handler(u_register_t max_off_pwrlvl)
{
	int32_t rc = 0;
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];

	assert(titanium_vector_table);
	assert(get_titanium_pstate(titanium_ctx->state) == TITANIUM_PSTATE_SUSPEND);

	/* Program the entry point, max_off_pwrlvl and enter the SP */
	write_ctx_reg(get_gpregs_ctx(&titanium_ctx->cpu_ctx),
		      CTX_GPREG_X0,
		      max_off_pwrlvl);
	cm_set_elr_el3(SECURE, (uint64_t) &titanium_vector_table->cpu_resume_entry);
	rc = titanium_synchronous_sp_entry(titanium_ctx);

	/*
	 * Read the response from TITANIUM. A non-zero return means that
	 * something went wrong while communicating with TITANIUM.
	 */
	if (rc != 0)
		panic();

	/* Update its context to reflect the state TITANIUM is in */
	set_titanium_pstate(titanium_ctx->state, TITANIUM_PSTATE_ON);
}

/*******************************************************************************
 * Return the type of TITANIUM the TITANIUM is dealing with. Report the current
 * resident cpu (mpidr format) if it is a UP/UP migratable TITANIUM.
 ******************************************************************************/
static int32_t titanium_cpu_migrate_info(u_register_t *resident_cpu)
{
	return TITANIUM_MIGRATE_INFO;
}

/*******************************************************************************
 * System is about to be switched off. Allow the TITANIUM to perform
 * any actions needed.
 ******************************************************************************/
static void titanium_system_off(void)
{
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];

	assert(titanium_vector_table);
	assert(get_titanium_pstate(titanium_ctx->state) == TITANIUM_PSTATE_ON);

	/* Program the entry point */
	cm_set_elr_el3(SECURE, (uint64_t) &titanium_vector_table->system_off_entry);

	/* Enter TITANIUM. We do not care about the return value because we
	 * must continue the shutdown anyway */
	titanium_synchronous_sp_entry(titanium_ctx);
}

/*******************************************************************************
 * System is about to be reset. Allow the TITANIUM to perform
 * any actions needed.
 ******************************************************************************/
static void titanium_system_reset(void)
{
	uint32_t linear_id = plat_my_core_pos();
	titanium_context_t *titanium_ctx = &titanium_sp_context[linear_id];

	assert(titanium_vector_table);
	assert(get_titanium_pstate(titanium_ctx->state) == TITANIUM_PSTATE_ON);

	/* Program the entry point */
	cm_set_elr_el3(SECURE, (uint64_t) &titanium_vector_table->system_reset_entry);

	/* Enter TITANIUM. We do not care about the return value because we
	 * must continue the reset anyway */
	titanium_synchronous_sp_entry(titanium_ctx);
}


/*******************************************************************************
 * Structure populated by the TITANIUM Dispatcher to be given a chance to
 * perform any TITANIUM bookkeeping before PSCI executes a power mgmt.
 * operation.
 ******************************************************************************/
const spd_pm_ops_t titanium_pm = {
	.svc_on = titanium_cpu_on_handler,
	.svc_off = titanium_cpu_off_handler,
	.svc_suspend = titanium_cpu_suspend_handler,
	.svc_on_finish = titanium_cpu_on_finish_handler,
	.svc_suspend_finish = titanium_cpu_suspend_finish_handler,
	.svc_migrate = NULL,
	.svc_migrate_info = titanium_cpu_migrate_info,
	.svc_system_off = titanium_system_off,
	.svc_system_reset = titanium_system_reset,
};
