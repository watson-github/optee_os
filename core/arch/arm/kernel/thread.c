/*
 * Copyright (c) 2016, Linaro Limited
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <platform_config.h>
#include <kernel/panic.h>
#include <kernel/thread.h>
#include <kernel/thread_defs.h>
#include "thread_private.h"
#include <sm/sm_defs.h>
#include <sm/sm.h>
#include <optee_msg.h>
#include <sm/optee_smc.h>
#include <arm.h>
#include <kernel/tz_proc_def.h>
#include <kernel/tz_proc.h>
#include <kernel/misc.h>
#include <mm/tee_mmu.h>
#include <mm/core_memprot.h>
#include <mm/tee_mmu_defs.h>
#include <mm/tee_mm.h>
#include <mm/tee_pager.h>
#include <kernel/tee_ta_manager.h>
#include <util.h>
#include <trace.h>
#include <assert.h>
#include <keep.h>

#ifdef ARM32
#define STACK_TMP_SIZE		1024
#define STACK_THREAD_SIZE	8192

#if TRACE_LEVEL > 0
#define STACK_ABT_SIZE		2048
#else
#define STACK_ABT_SIZE		1024
#endif

#endif /*ARM32*/

#ifdef ARM64
#define STACK_TMP_SIZE		2048
#define STACK_THREAD_SIZE	8192

#if TRACE_LEVEL > 0
#define STACK_ABT_SIZE		3072
#else
#define STACK_ABT_SIZE		1024
#endif
#endif /*ARM64*/

#define RPC_MAX_NUM_PARAMS	2

struct thread_ctx threads[CFG_NUM_THREADS];

static struct thread_core_local thread_core_local[CFG_TEE_CORE_NB_CORE];

#ifdef CFG_WITH_STACK_CANARIES
#ifdef ARM32
#define STACK_CANARY_SIZE	(4 * sizeof(uint32_t))
#endif
#ifdef ARM64
#define STACK_CANARY_SIZE	(8 * sizeof(uint32_t))
#endif
#define START_CANARY_VALUE	0xdededede
#define END_CANARY_VALUE	0xabababab
#define GET_START_CANARY(name, stack_num) name[stack_num][0]
#define GET_END_CANARY(name, stack_num) \
	name[stack_num][sizeof(name[stack_num]) / sizeof(uint32_t) - 1]
#else
#define STACK_CANARY_SIZE	0
#endif

#define DECLARE_STACK(name, num_stacks, stack_size, linkage) \
linkage uint32_t name[num_stacks] \
		[ROUNDUP(stack_size + STACK_CANARY_SIZE, STACK_ALIGNMENT) / \
		sizeof(uint32_t)] \
		__attribute__((section(".nozi.stack"), \
			       aligned(STACK_ALIGNMENT)))

#define STACK_SIZE(stack) (sizeof(stack) - STACK_CANARY_SIZE / 2)

#define GET_STACK(stack) \
	((vaddr_t)(stack) + STACK_SIZE(stack))

DECLARE_STACK(stack_tmp, CFG_TEE_CORE_NB_CORE, STACK_TMP_SIZE, /* global */);
DECLARE_STACK(stack_abt, CFG_TEE_CORE_NB_CORE, STACK_ABT_SIZE, static);
#if !defined(CFG_WITH_ARM_TRUSTED_FW)
DECLARE_STACK(stack_sm, CFG_TEE_CORE_NB_CORE, SM_STACK_SIZE, static);
#endif
#ifndef CFG_WITH_PAGER
DECLARE_STACK(stack_thread, CFG_NUM_THREADS, STACK_THREAD_SIZE, static);
#endif

const uint32_t stack_tmp_stride = STACK_SIZE(stack_tmp[0]);

KEEP_PAGER(stack_tmp);
KEEP_PAGER(stack_tmp_stride);

thread_smc_handler_t thread_std_smc_handler_ptr;
static thread_smc_handler_t thread_fast_smc_handler_ptr;
thread_fiq_handler_t thread_fiq_handler_ptr;
thread_pm_handler_t thread_cpu_on_handler_ptr;
thread_pm_handler_t thread_cpu_off_handler_ptr;
thread_pm_handler_t thread_cpu_suspend_handler_ptr;
thread_pm_handler_t thread_cpu_resume_handler_ptr;
thread_pm_handler_t thread_system_off_handler_ptr;
thread_pm_handler_t thread_system_reset_handler_ptr;


static unsigned int thread_global_lock = SPINLOCK_UNLOCK;
static bool thread_prealloc_rpc_cache;

static void init_canaries(void)
{
#ifdef CFG_WITH_STACK_CANARIES
	size_t n;
#define INIT_CANARY(name)						\
	for (n = 0; n < ARRAY_SIZE(name); n++) {			\
		uint32_t *start_canary = &GET_START_CANARY(name, n);	\
		uint32_t *end_canary = &GET_END_CANARY(name, n);	\
									\
		*start_canary = START_CANARY_VALUE;			\
		*end_canary = END_CANARY_VALUE;				\
		DMSG("#Stack canaries for %s[%zu] with top at %p\n",	\
			#name, n, (void *)(end_canary - 1));		\
		DMSG("watch *%p\n", (void *)end_canary);		\
	}

	INIT_CANARY(stack_tmp);
	INIT_CANARY(stack_abt);
#if !defined(CFG_WITH_ARM_TRUSTED_FW)
	INIT_CANARY(stack_sm);
#endif
#ifndef CFG_WITH_PAGER
	INIT_CANARY(stack_thread);
#endif
#endif/*CFG_WITH_STACK_CANARIES*/
}

void thread_check_canaries(void)
{
#ifdef CFG_WITH_STACK_CANARIES
	size_t n;

	for (n = 0; n < ARRAY_SIZE(stack_tmp); n++) {
		assert(GET_START_CANARY(stack_tmp, n) == START_CANARY_VALUE);
		assert(GET_END_CANARY(stack_tmp, n) == END_CANARY_VALUE);
	}

	for (n = 0; n < ARRAY_SIZE(stack_abt); n++) {
		assert(GET_START_CANARY(stack_abt, n) == START_CANARY_VALUE);
		assert(GET_END_CANARY(stack_abt, n) == END_CANARY_VALUE);
	}
#if !defined(CFG_WITH_ARM_TRUSTED_FW)
	for (n = 0; n < ARRAY_SIZE(stack_sm); n++) {
		assert(GET_START_CANARY(stack_sm, n) == START_CANARY_VALUE);
		assert(GET_END_CANARY(stack_sm, n) == END_CANARY_VALUE);
	}
#endif
#ifndef CFG_WITH_PAGER
	for (n = 0; n < ARRAY_SIZE(stack_thread); n++) {
		assert(GET_START_CANARY(stack_thread, n) == START_CANARY_VALUE);
		assert(GET_END_CANARY(stack_thread, n) == END_CANARY_VALUE);
	}
#endif
#endif/*CFG_WITH_STACK_CANARIES*/
}

static void lock_global(void)
{
	cpu_spin_lock(&thread_global_lock);
}

static void unlock_global(void)
{
	cpu_spin_unlock(&thread_global_lock);
}

#ifdef ARM32
uint32_t thread_get_exceptions(void)
{
	uint32_t cpsr = read_cpsr();

	return (cpsr >> CPSR_F_SHIFT) & THREAD_EXCP_ALL;
}

void thread_set_exceptions(uint32_t exceptions)
{
	uint32_t cpsr = read_cpsr();

	cpsr &= ~(THREAD_EXCP_ALL << CPSR_F_SHIFT);
	cpsr |= ((exceptions & THREAD_EXCP_ALL) << CPSR_F_SHIFT);
	write_cpsr(cpsr);
}
#endif /*ARM32*/

#ifdef ARM64
uint32_t thread_get_exceptions(void)
{
	uint32_t daif = read_daif();

	return (daif >> DAIF_F_SHIFT) & THREAD_EXCP_ALL;
}

void thread_set_exceptions(uint32_t exceptions)
{
	uint32_t daif = read_daif();

	daif &= ~(THREAD_EXCP_ALL << DAIF_F_SHIFT);
	daif |= ((exceptions & THREAD_EXCP_ALL) << DAIF_F_SHIFT);
	write_daif(daif);
}
#endif /*ARM64*/

uint32_t thread_mask_exceptions(uint32_t exceptions)
{
	uint32_t state = thread_get_exceptions();

	thread_set_exceptions(state | (exceptions & THREAD_EXCP_ALL));
	return state;
}

void thread_unmask_exceptions(uint32_t state)
{
	thread_set_exceptions(state & THREAD_EXCP_ALL);
}


struct thread_core_local *thread_get_core_local(void)
{
	uint32_t cpu_id = get_core_pos();

	/*
	 * IRQs must be disabled before playing with core_local since
	 * we otherwise may be rescheduled to a different core in the
	 * middle of this function.
	 */
	assert(thread_get_exceptions() & THREAD_EXCP_IRQ);

	assert(cpu_id < CFG_TEE_CORE_NB_CORE);
	return &thread_core_local[cpu_id];
}

static void thread_lazy_save_ns_vfp(void)
{
#ifdef CFG_WITH_VFP
	struct thread_ctx *thr = threads + thread_get_id();

	thr->vfp_state.ns_saved = false;
#if defined(ARM64) && defined(CFG_WITH_ARM_TRUSTED_FW)
	/*
	 * ARM TF saves and restores CPACR_EL1, so we must assume NS world
	 * uses VFP and always preserve the register file when secure world
	 * is about to use it
	 */
	thr->vfp_state.ns.force_save = true;
#endif
	vfp_lazy_save_state_init(&thr->vfp_state.ns);
#endif /*CFG_WITH_VFP*/
}

static void thread_lazy_restore_ns_vfp(void)
{
#ifdef CFG_WITH_VFP
	struct thread_ctx *thr = threads + thread_get_id();
	struct thread_user_vfp_state *tuv = thr->vfp_state.uvfp;

	assert(!thr->vfp_state.sec_lazy_saved && !thr->vfp_state.sec_saved);

	if (tuv && tuv->lazy_saved && !tuv->saved) {
		vfp_lazy_save_state_final(&tuv->vfp);
		tuv->saved = true;
	}

	vfp_lazy_restore_state(&thr->vfp_state.ns, thr->vfp_state.ns_saved);
	thr->vfp_state.ns_saved = false;
#endif /*CFG_WITH_VFP*/
}

#ifdef ARM32
static void init_regs(struct thread_ctx *thread,
		struct thread_smc_args *args)
{
	thread->regs.pc = (uint32_t)thread_std_smc_entry;

	/*
	 * Stdcalls starts in SVC mode with masked IRQ, masked Asynchronous
	 * abort and unmasked FIQ.
	  */
	thread->regs.cpsr = read_cpsr() & ARM32_CPSR_E;
	thread->regs.cpsr |= CPSR_MODE_SVC | CPSR_I | CPSR_A;
	/* Enable thumb mode if it's a thumb instruction */
	if (thread->regs.pc & 1)
		thread->regs.cpsr |= CPSR_T;
	/* Reinitialize stack pointer */
	thread->regs.svc_sp = thread->stack_va_end;

	/*
	 * Copy arguments into context. This will make the
	 * arguments appear in r0-r7 when thread is started.
	 */
	thread->regs.r0 = args->a0;
	thread->regs.r1 = args->a1;
	thread->regs.r2 = args->a2;
	thread->regs.r3 = args->a3;
	thread->regs.r4 = args->a4;
	thread->regs.r5 = args->a5;
	thread->regs.r6 = args->a6;
	thread->regs.r7 = args->a7;
}
#endif /*ARM32*/

#ifdef ARM64
static void init_regs(struct thread_ctx *thread,
		struct thread_smc_args *args)
{
	thread->regs.pc = (uint64_t)thread_std_smc_entry;

	/*
	 * Stdcalls starts in SVC mode with masked IRQ, masked Asynchronous
	 * abort and unmasked FIQ.
	  */
	thread->regs.cpsr = SPSR_64(SPSR_64_MODE_EL1, SPSR_64_MODE_SP_EL0,
				    DAIFBIT_IRQ | DAIFBIT_ABT);
	/* Reinitialize stack pointer */
	thread->regs.sp = thread->stack_va_end;

	/*
	 * Copy arguments into context. This will make the
	 * arguments appear in x0-x7 when thread is started.
	 */
	thread->regs.x[0] = args->a0;
	thread->regs.x[1] = args->a1;
	thread->regs.x[2] = args->a2;
	thread->regs.x[3] = args->a3;
	thread->regs.x[4] = args->a4;
	thread->regs.x[5] = args->a5;
	thread->regs.x[6] = args->a6;
	thread->regs.x[7] = args->a7;
}
#endif /*ARM64*/

void thread_init_boot_thread(void)
{
	struct thread_core_local *l = thread_get_core_local();
	size_t n;

	for (n = 0; n < CFG_NUM_THREADS; n++) {
		TAILQ_INIT(&threads[n].mutexes);
		TAILQ_INIT(&threads[n].tsd.sess_stack);
#ifdef CFG_SMALL_PAGE_USER_TA
		SLIST_INIT(&threads[n].tsd.pgt_cache);
#endif
	}

	for (n = 0; n < CFG_TEE_CORE_NB_CORE; n++)
		thread_core_local[n].curr_thread = -1;

	l->curr_thread = 0;
	threads[0].state = THREAD_STATE_ACTIVE;
}

void thread_clr_boot_thread(void)
{
	struct thread_core_local *l = thread_get_core_local();

	assert(l->curr_thread >= 0 && l->curr_thread < CFG_NUM_THREADS);
	assert(threads[l->curr_thread].state == THREAD_STATE_ACTIVE);
	assert(TAILQ_EMPTY(&threads[l->curr_thread].mutexes));
	threads[l->curr_thread].state = THREAD_STATE_FREE;
	l->curr_thread = -1;
}

static void thread_alloc_and_run(struct thread_smc_args *args)
{
	size_t n;
	struct thread_core_local *l = thread_get_core_local();
	bool found_thread = false;

	assert(l->curr_thread == -1);

	lock_global();

	for (n = 0; n < CFG_NUM_THREADS; n++) {
		if (threads[n].state == THREAD_STATE_FREE) {
			threads[n].state = THREAD_STATE_ACTIVE;
			found_thread = true;
			break;
		}
	}

	unlock_global();

	if (!found_thread) {
		args->a0 = OPTEE_SMC_RETURN_ETHREAD_LIMIT;
		return;
	}

	l->curr_thread = n;

	threads[n].flags = 0;
	init_regs(threads + n, args);

	/* Save Hypervisor Client ID */
	threads[n].hyp_clnt_id = args->a7;

	thread_lazy_save_ns_vfp();
	thread_resume(&threads[n].regs);
}

#ifdef ARM32
static void copy_a0_to_a5(struct thread_ctx_regs *regs,
		struct thread_smc_args *args)
{
	/*
	 * Update returned values from RPC, values will appear in
	 * r0-r3 when thread is resumed.
	 */
	regs->r0 = args->a0;
	regs->r1 = args->a1;
	regs->r2 = args->a2;
	regs->r3 = args->a3;
	regs->r4 = args->a4;
	regs->r5 = args->a5;
}
#endif /*ARM32*/

#ifdef ARM64
static void copy_a0_to_a5(struct thread_ctx_regs *regs,
		struct thread_smc_args *args)
{
	/*
	 * Update returned values from RPC, values will appear in
	 * x0-x3 when thread is resumed.
	 */
	regs->x[0] = args->a0;
	regs->x[1] = args->a1;
	regs->x[2] = args->a2;
	regs->x[3] = args->a3;
	regs->x[4] = args->a4;
	regs->x[5] = args->a5;
}
#endif /*ARM64*/

static void thread_resume_from_rpc(struct thread_smc_args *args)
{
	size_t n = args->a3; /* thread id */
	struct thread_core_local *l = thread_get_core_local();
	uint32_t rv = 0;

	assert(l->curr_thread == -1);

	lock_global();

	if (n < CFG_NUM_THREADS &&
	    threads[n].state == THREAD_STATE_SUSPENDED &&
	    args->a7 == threads[n].hyp_clnt_id)
		threads[n].state = THREAD_STATE_ACTIVE;
	else
		rv = OPTEE_SMC_RETURN_ERESUME;

	unlock_global();

	if (rv) {
		args->a0 = rv;
		return;
	}

	l->curr_thread = n;

	if (threads[n].have_user_map)
		core_mmu_set_user_map(&threads[n].user_map);

	/*
	 * Return from RPC to request service of an IRQ must not
	 * get parameters from non-secure world.
	 */
	if (threads[n].flags & THREAD_FLAGS_COPY_ARGS_ON_RETURN) {
		copy_a0_to_a5(&threads[n].regs, args);
		threads[n].flags &= ~THREAD_FLAGS_COPY_ARGS_ON_RETURN;
	}

	thread_lazy_save_ns_vfp();
	thread_resume(&threads[n].regs);
}

void thread_handle_fast_smc(struct thread_smc_args *args)
{
	thread_check_canaries();
	thread_fast_smc_handler_ptr(args);
	/* Fast handlers must not unmask any exceptions */
	assert(thread_get_exceptions() == THREAD_EXCP_ALL);
}

void thread_handle_std_smc(struct thread_smc_args *args)
{
	thread_check_canaries();

	if (args->a0 == OPTEE_SMC_CALL_RETURN_FROM_RPC)
		thread_resume_from_rpc(args);
	else
		thread_alloc_and_run(args);
}

/* Helper routine for the assembly function thread_std_smc_entry() */
void __thread_std_smc_entry(struct thread_smc_args *args)
{
	struct thread_ctx *thr = threads + thread_get_id();

	if (!thr->rpc_arg) {
		paddr_t parg;
		uint64_t carg;
		void *arg;

		thread_rpc_alloc_arg(
			OPTEE_MSG_GET_ARG_SIZE(RPC_MAX_NUM_PARAMS),
			&parg, &carg);
		if (!parg || !ALIGNMENT_IS_OK(parg, struct optee_msg_arg) ||
		    !(arg = phys_to_virt(parg, CORE_MEM_NSEC_SHM))) {
			thread_rpc_free_arg(carg);
			args->a0 = OPTEE_SMC_RETURN_ENOMEM;
			return;
		}

		thr->rpc_arg = arg;
		thr->rpc_carg = carg;
	}

	thread_std_smc_handler_ptr(args);

	if (!thread_prealloc_rpc_cache) {
		thread_rpc_free_arg(thr->rpc_carg);
		thr->rpc_carg = 0;
		thr->rpc_arg = 0;
	}
}

void *thread_get_tmp_sp(void)
{
	struct thread_core_local *l = thread_get_core_local();

	return (void *)l->tmp_stack_va_end;
}

#ifdef ARM64
vaddr_t thread_get_saved_thread_sp(void)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != -1);
	return threads[ct].kern_sp;
}
#endif /*ARM64*/

bool thread_addr_is_in_stack(vaddr_t va)
{
	struct thread_ctx *thr = threads + thread_get_id();

	return va < thr->stack_va_end &&
	       va >= (thr->stack_va_end - STACK_THREAD_SIZE);
}

void thread_state_free(void)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != -1);
	assert(TAILQ_EMPTY(&threads[ct].mutexes));

	thread_lazy_restore_ns_vfp();
	tee_pager_release_phys(
		(void *)(threads[ct].stack_va_end - STACK_THREAD_SIZE),
		STACK_THREAD_SIZE);

	lock_global();

	assert(threads[ct].state == THREAD_STATE_ACTIVE);
	threads[ct].state = THREAD_STATE_FREE;
	threads[ct].flags = 0;
	l->curr_thread = -1;

	unlock_global();
}

#ifdef ARM32
static bool is_from_user(uint32_t cpsr)
{
	return (cpsr & ARM32_CPSR_MODE_MASK) == ARM32_CPSR_MODE_USR;
}
#endif

#ifdef ARM64
static bool is_from_user(uint32_t cpsr)
{
	if (cpsr & (SPSR_MODE_RW_32 << SPSR_MODE_RW_SHIFT))
		return true;
	if (((cpsr >> SPSR_64_MODE_EL_SHIFT) & SPSR_64_MODE_EL_MASK) ==
	     SPSR_64_MODE_EL0)
		return true;
	return false;
}
#endif

#ifdef CFG_WITH_PAGER
static void release_unused_kernel_stack(struct thread_ctx *thr)
{
	vaddr_t sp = thr->regs.svc_sp;
	vaddr_t base = thr->stack_va_end - STACK_THREAD_SIZE;
	size_t len = sp - base;

	tee_pager_release_phys((void *)base, len);
}
#else
static void release_unused_kernel_stack(struct thread_ctx *thr __unused)
{
}
#endif

int thread_state_suspend(uint32_t flags, uint32_t cpsr, vaddr_t pc)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != -1);

	thread_check_canaries();

	release_unused_kernel_stack(threads + ct);

	if (is_from_user(cpsr))
		thread_user_save_vfp();
	thread_lazy_restore_ns_vfp();

	lock_global();

	assert(threads[ct].state == THREAD_STATE_ACTIVE);
	threads[ct].flags |= flags;
	threads[ct].regs.cpsr = cpsr;
	threads[ct].regs.pc = pc;
	threads[ct].state = THREAD_STATE_SUSPENDED;

	threads[ct].have_user_map = core_mmu_user_mapping_is_active();
	if (threads[ct].have_user_map) {
		core_mmu_get_user_map(&threads[ct].user_map);
		core_mmu_set_user_map(NULL);
	}

	l->curr_thread = -1;

	unlock_global();

	return ct;
}

#ifdef ARM32
static void set_tmp_stack(struct thread_core_local *l, vaddr_t sp)
{
	l->tmp_stack_va_end = sp;
	thread_set_irq_sp(sp);
	thread_set_fiq_sp(sp);
}

static void set_abt_stack(struct thread_core_local *l __unused, vaddr_t sp)
{
	thread_set_abt_sp(sp);
}
#endif /*ARM32*/

#ifdef ARM64
static void set_tmp_stack(struct thread_core_local *l, vaddr_t sp)
{
	/*
	 * We're already using the tmp stack when this function is called
	 * so there's no need to assign it to any stack pointer. However,
	 * we'll need to restore it at different times so store it here.
	 */
	l->tmp_stack_va_end = sp;
}

static void set_abt_stack(struct thread_core_local *l, vaddr_t sp)
{
	l->abt_stack_va_end = sp;
}
#endif /*ARM64*/

bool thread_init_stack(uint32_t thread_id, vaddr_t sp)
{
	if (thread_id >= CFG_NUM_THREADS)
		return false;
	threads[thread_id].stack_va_end = sp;
	return true;
}

int thread_get_id_may_fail(void)
{
	/* thread_get_core_local() requires IRQs to be disabled */
	uint32_t exceptions = thread_mask_exceptions(THREAD_EXCP_IRQ);
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	thread_unmask_exceptions(exceptions);
	return ct;
}

int thread_get_id(void)
{
	int ct = thread_get_id_may_fail();

	assert((ct >= 0) && (ct < CFG_NUM_THREADS));
	return ct;
}

static void init_handlers(const struct thread_handlers *handlers)
{
	thread_std_smc_handler_ptr = handlers->std_smc;
	thread_fast_smc_handler_ptr = handlers->fast_smc;
	thread_fiq_handler_ptr = handlers->fiq;
	thread_cpu_on_handler_ptr = handlers->cpu_on;
	thread_cpu_off_handler_ptr = handlers->cpu_off;
	thread_cpu_suspend_handler_ptr = handlers->cpu_suspend;
	thread_cpu_resume_handler_ptr = handlers->cpu_resume;
	thread_system_off_handler_ptr = handlers->system_off;
	thread_system_reset_handler_ptr = handlers->system_reset;
}

#ifdef CFG_WITH_PAGER
static void init_thread_stacks(void)
{
	size_t n;

	/*
	 * Allocate virtual memory for thread stacks.
	 */
	for (n = 0; n < CFG_NUM_THREADS; n++) {
		tee_mm_entry_t *mm;
		vaddr_t sp;

		/* Find vmem for thread stack and its protection gap */
		mm = tee_mm_alloc(&tee_mm_vcore,
				  SMALL_PAGE_SIZE + STACK_THREAD_SIZE);
		TEE_ASSERT(mm);

		/* Claim eventual physical page */
		tee_pager_add_pages(tee_mm_get_smem(mm), tee_mm_get_size(mm),
				    true);

		/* Add the area to the pager */
		tee_pager_add_core_area(tee_mm_get_smem(mm) + SMALL_PAGE_SIZE,
					tee_mm_get_bytes(mm) - SMALL_PAGE_SIZE,
					TEE_MATTR_PRW | TEE_MATTR_LOCKED,
					NULL, NULL);

		/* init effective stack */
		sp = tee_mm_get_smem(mm) + tee_mm_get_bytes(mm);
		if (!thread_init_stack(n, sp))
			panic();
	}
}
#else
static void init_thread_stacks(void)
{
	size_t n;

	/* Assign the thread stacks */
	for (n = 0; n < CFG_NUM_THREADS; n++) {
		if (!thread_init_stack(n, GET_STACK(stack_thread[n])))
			panic();
	}
}
#endif /*CFG_WITH_PAGER*/

void thread_init_primary(const struct thread_handlers *handlers)
{
	init_handlers(handlers);

	/* Initialize canaries around the stacks */
	init_canaries();

	init_thread_stacks();
	pgt_init();
}

static void init_sec_mon(size_t pos __maybe_unused)
{
#if !defined(CFG_WITH_ARM_TRUSTED_FW)
	/* Initialize secure monitor */
	sm_init(GET_STACK(stack_sm[pos]));
	sm_set_entry_vector(thread_vector_table);
#endif
}

void thread_init_per_cpu(void)
{
	size_t pos = get_core_pos();
	struct thread_core_local *l = thread_get_core_local();

	init_sec_mon(pos);

	set_tmp_stack(l, GET_STACK(stack_tmp[pos]));
	set_abt_stack(l, GET_STACK(stack_abt[pos]));

	thread_init_vbar();
}

struct thread_specific_data *thread_get_tsd(void)
{
	return &threads[thread_get_id()].tsd;
}

struct thread_ctx_regs *thread_get_ctx_regs(void)
{
	struct thread_core_local *l = thread_get_core_local();

	assert(l->curr_thread != -1);
	return &threads[l->curr_thread].regs;
}

void thread_set_irq(bool enable)
{
	/* thread_get_core_local() requires IRQs to be disabled */
	uint32_t exceptions = thread_mask_exceptions(THREAD_EXCP_IRQ);
	struct thread_core_local *l;

	l = thread_get_core_local();

	assert(l->curr_thread != -1);

	if (enable) {
		threads[l->curr_thread].flags |= THREAD_FLAGS_IRQ_ENABLE;
		thread_set_exceptions(exceptions & ~THREAD_EXCP_IRQ);
	} else {
		/*
		 * No need to disable IRQ here since it's already disabled
		 * above.
		 */
		threads[l->curr_thread].flags &= ~THREAD_FLAGS_IRQ_ENABLE;
	}
}

void thread_restore_irq(void)
{
	/* thread_get_core_local() requires IRQs to be disabled */
	uint32_t exceptions = thread_mask_exceptions(THREAD_EXCP_IRQ);
	struct thread_core_local *l;

	l = thread_get_core_local();

	assert(l->curr_thread != -1);

	if (threads[l->curr_thread].flags & THREAD_FLAGS_IRQ_ENABLE)
		thread_set_exceptions(exceptions & ~THREAD_EXCP_IRQ);
}

#ifdef CFG_WITH_VFP
uint32_t thread_kernel_enable_vfp(void)
{
	uint32_t exceptions = thread_mask_exceptions(THREAD_EXCP_IRQ);
	struct thread_ctx *thr = threads + thread_get_id();
	struct thread_user_vfp_state *tuv = thr->vfp_state.uvfp;

	assert(!vfp_is_enabled());

	if (!thr->vfp_state.ns_saved) {
		vfp_lazy_save_state_final(&thr->vfp_state.ns);
		thr->vfp_state.ns_saved = true;
	} else if (thr->vfp_state.sec_lazy_saved &&
		   !thr->vfp_state.sec_saved) {
		/*
		 * This happens when we're handling an abort while the
		 * thread was using the VFP state.
		 */
		vfp_lazy_save_state_final(&thr->vfp_state.sec);
		thr->vfp_state.sec_saved = true;
	} else if (tuv && tuv->lazy_saved && !tuv->saved) {
		/*
		 * This can happen either during syscall or abort
		 * processing (while processing a syscall).
		 */
		vfp_lazy_save_state_final(&tuv->vfp);
		tuv->saved = true;
	}

	vfp_enable();
	return exceptions;
}

void thread_kernel_disable_vfp(uint32_t state)
{
	uint32_t exceptions;

	assert(vfp_is_enabled());

	vfp_disable();
	exceptions = thread_get_exceptions();
	assert(exceptions & THREAD_EXCP_IRQ);
	exceptions &= ~THREAD_EXCP_IRQ;
	exceptions |= state & THREAD_EXCP_IRQ;
	thread_set_exceptions(exceptions);
}

void thread_kernel_save_vfp(void)
{
	struct thread_ctx *thr = threads + thread_get_id();

	assert(thread_get_exceptions() & THREAD_EXCP_IRQ);
	if (vfp_is_enabled()) {
		vfp_lazy_save_state_init(&thr->vfp_state.sec);
		thr->vfp_state.sec_lazy_saved = true;
	}
}

void thread_kernel_restore_vfp(void)
{
	struct thread_ctx *thr = threads + thread_get_id();

	assert(thread_get_exceptions() & THREAD_EXCP_IRQ);
	assert(!vfp_is_enabled());
	if (thr->vfp_state.sec_lazy_saved) {
		vfp_lazy_restore_state(&thr->vfp_state.sec,
				       thr->vfp_state.sec_saved);
		thr->vfp_state.sec_saved = false;
		thr->vfp_state.sec_lazy_saved = false;
	}
}

void thread_user_enable_vfp(struct thread_user_vfp_state *uvfp)
{
	struct thread_ctx *thr = threads + thread_get_id();
	struct thread_user_vfp_state *tuv = thr->vfp_state.uvfp;

	assert(thread_get_exceptions() & THREAD_EXCP_IRQ);
	assert(!vfp_is_enabled());

	if (!thr->vfp_state.ns_saved) {
		vfp_lazy_save_state_final(&thr->vfp_state.ns);
		thr->vfp_state.ns_saved = true;
	} else if (tuv && uvfp != tuv) {
		if (tuv->lazy_saved && !tuv->saved) {
			vfp_lazy_save_state_final(&tuv->vfp);
			tuv->saved = true;
		}
	}

	if (uvfp->lazy_saved)
		vfp_lazy_restore_state(&uvfp->vfp, uvfp->saved);
	uvfp->lazy_saved = false;
	uvfp->saved = false;

	thr->vfp_state.uvfp = uvfp;
	vfp_enable();
}

void thread_user_save_vfp(void)
{
	struct thread_ctx *thr = threads + thread_get_id();
	struct thread_user_vfp_state *tuv = thr->vfp_state.uvfp;

	assert(thread_get_exceptions() & THREAD_EXCP_IRQ);
	if (!vfp_is_enabled())
		return;

	assert(tuv && !tuv->lazy_saved && !tuv->saved);
	vfp_lazy_save_state_init(&tuv->vfp);
	tuv->lazy_saved = true;
}

void thread_user_clear_vfp(struct thread_user_vfp_state *uvfp)
{
	struct thread_ctx *thr = threads + thread_get_id();

	if (uvfp == thr->vfp_state.uvfp)
		thr->vfp_state.uvfp = NULL;
	uvfp->lazy_saved = false;
	uvfp->saved = false;
}
#endif /*CFG_WITH_VFP*/

#ifdef ARM32
static bool get_spsr(bool is_32bit, unsigned long entry_func, uint32_t *spsr)
{
	uint32_t s;

	if (!is_32bit)
		return false;

	s = read_spsr();
	s &= ~(CPSR_MODE_MASK | CPSR_T | CPSR_IT_MASK1 | CPSR_IT_MASK2);
	s |= CPSR_MODE_USR;
	if (entry_func & 1)
		s |= CPSR_T;
	*spsr = s;
	return true;
}
#endif

#ifdef ARM64
static bool get_spsr(bool is_32bit, unsigned long entry_func, uint32_t *spsr)
{
	uint32_t s;

	if (is_32bit) {
		s = read_daif() & (SPSR_32_AIF_MASK << SPSR_32_AIF_SHIFT);
		s |= SPSR_MODE_RW_32 << SPSR_MODE_RW_SHIFT;
		s |= (entry_func & SPSR_32_T_MASK) << SPSR_32_T_SHIFT;
	} else {
		s = read_daif() & (SPSR_64_DAIF_MASK << SPSR_64_DAIF_SHIFT);
	}

	*spsr = s;
	return true;
}
#endif

uint32_t thread_enter_user_mode(unsigned long a0, unsigned long a1,
		unsigned long a2, unsigned long a3, unsigned long user_sp,
		unsigned long entry_func, bool is_32bit,
		uint32_t *exit_status0, uint32_t *exit_status1)
{
	uint32_t spsr;

	if (!get_spsr(is_32bit, entry_func, &spsr)) {
		*exit_status0 = 1; /* panic */
		*exit_status1 = 0xbadbadba;
		return 0;
	}
	return __thread_enter_user_mode(a0, a1, a2, a3, user_sp, entry_func,
					spsr, exit_status0, exit_status1);
}

void thread_add_mutex(struct mutex *m)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != -1 && threads[ct].state == THREAD_STATE_ACTIVE);
	assert(m->owner_id == -1);
	m->owner_id = ct;
	TAILQ_INSERT_TAIL(&threads[ct].mutexes, m, link);
}

void thread_rem_mutex(struct mutex *m)
{
	struct thread_core_local *l = thread_get_core_local();
	int ct = l->curr_thread;

	assert(ct != -1 && threads[ct].state == THREAD_STATE_ACTIVE);
	assert(m->owner_id == ct);
	m->owner_id = -1;
	TAILQ_REMOVE(&threads[ct].mutexes, m, link);
}

bool thread_disable_prealloc_rpc_cache(uint64_t *cookie)
{
	bool rv;
	size_t n;
	uint32_t exceptions = thread_mask_exceptions(THREAD_EXCP_IRQ);

	lock_global();

	for (n = 0; n < CFG_NUM_THREADS; n++) {
		if (threads[n].state != THREAD_STATE_FREE) {
			rv = false;
			goto out;
		}
	}

	rv = true;
	for (n = 0; n < CFG_NUM_THREADS; n++) {
		if (threads[n].rpc_arg) {
			*cookie = threads[n].rpc_carg;
			threads[n].rpc_carg = 0;
			threads[n].rpc_arg = NULL;
			goto out;
		}
	}

	*cookie = 0;
	thread_prealloc_rpc_cache = false;
out:
	unlock_global();
	thread_unmask_exceptions(exceptions);
	return rv;
}

bool thread_enable_prealloc_rpc_cache(void)
{
	bool rv;
	size_t n;
	uint32_t exceptions = thread_mask_exceptions(THREAD_EXCP_IRQ);

	lock_global();

	for (n = 0; n < CFG_NUM_THREADS; n++) {
		if (threads[n].state != THREAD_STATE_FREE) {
			rv = false;
			goto out;
		}
	}

	rv = true;
	thread_prealloc_rpc_cache = true;
out:
	unlock_global();
	thread_unmask_exceptions(exceptions);
	return rv;
}

static uint32_t rpc_cmd_nolock(uint32_t cmd, size_t num_params,
		struct optee_msg_param *params)
{
	uint32_t rpc_args[THREAD_RPC_NUM_ARGS] = { OPTEE_SMC_RETURN_RPC_CMD };
	struct thread_ctx *thr = threads + thread_get_id();
	struct optee_msg_arg *arg = thr->rpc_arg;
	uint64_t carg = thr->rpc_carg;
	const size_t params_size = sizeof(struct optee_msg_param) * num_params;
	size_t n;

	TEE_ASSERT(arg && carg && num_params <= RPC_MAX_NUM_PARAMS);

	memset(arg, 0, OPTEE_MSG_GET_ARG_SIZE(RPC_MAX_NUM_PARAMS));
	arg->cmd = cmd;
	arg->ret = TEE_ERROR_GENERIC; /* in case value isn't updated */
	arg->num_params = num_params;
	memcpy(OPTEE_MSG_GET_PARAMS(arg), params, params_size);

	reg_pair_from_64(carg, rpc_args + 1, rpc_args + 2);
	thread_rpc(rpc_args);
	for (n = 0; n < num_params; n++) {
		switch (params[n].attr & OPTEE_MSG_ATTR_TYPE_MASK) {
		case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
		case OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_RMEM_INOUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
			memcpy(params + n, OPTEE_MSG_GET_PARAMS(arg) + n,
			       sizeof(struct optee_msg_param));
			break;
		default:
			break;
		}
	}
	return arg->ret;
}

uint32_t thread_rpc_cmd(uint32_t cmd, size_t num_params,
		struct optee_msg_param *params)
{
	uint32_t ret;

	ret = rpc_cmd_nolock(cmd, num_params, params);

	return ret;
}

void thread_rpc_alloc_arg(size_t size, paddr_t *arg, uint64_t *cookie)
{
	uint32_t rpc_args[THREAD_RPC_NUM_ARGS] = {
		OPTEE_SMC_RETURN_RPC_ALLOC, size};

	thread_rpc(rpc_args);
	*arg = reg_pair_to_64(rpc_args[1], rpc_args[2]);
	*cookie = reg_pair_to_64(rpc_args[4], rpc_args[5]);
}

/**
 * Allocates shared memory buffer via RPC
 *
 * @size:	size in bytes of shared memory buffer
 * @align:	required alignment of buffer
 * @bt:		buffer type OPTEE_MSG_RPC_SHM_TYPE_*
 * @payload:	returned physical pointer to buffer, 0 if allocation
 *		failed.
 * @cookie:	returned cookie used when freeing the buffer
 */
static void thread_rpc_alloc(size_t size, size_t align, unsigned bt,
			paddr_t *payload, uint64_t *cookie)
{
	uint32_t rpc_args[THREAD_RPC_NUM_ARGS] = { OPTEE_SMC_RETURN_RPC_CMD };
	struct thread_ctx *thr = threads + thread_get_id();
	struct optee_msg_arg *arg = thr->rpc_arg;
	uint64_t carg = thr->rpc_carg;
	struct optee_msg_param *params = OPTEE_MSG_GET_PARAMS(arg);

	memset(arg, 0, OPTEE_MSG_GET_ARG_SIZE(1));
	arg->cmd = OPTEE_MSG_RPC_CMD_SHM_ALLOC;
	arg->ret = TEE_ERROR_GENERIC; /* in case value isn't updated */
	arg->num_params = 1;

	params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = bt;
	params[0].u.value.b = size;
	params[0].u.value.c = align;

	reg_pair_from_64(carg, rpc_args + 1, rpc_args + 2);
	thread_rpc(rpc_args);
	if (arg->ret != TEE_SUCCESS)
		goto fail;

	if (arg->num_params != 1)
		goto fail;

	if (params[0].attr != OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT)
		goto fail;

	*payload = params[0].u.tmem.buf_ptr;
	*cookie = params[0].u.tmem.shm_ref;
	return;
fail:
	*payload = 0;
	*cookie = 0;
}

/**
 * Free physical memory previously allocated with thread_rpc_alloc()
 *
 * @cookie:	cookie received when allocating the buffer
 * @bt:		 must be the same as supplied when allocating
 */
static void thread_rpc_free(unsigned bt, uint64_t cookie)
{
	uint32_t rpc_args[THREAD_RPC_NUM_ARGS] = { OPTEE_SMC_RETURN_RPC_CMD };
	struct thread_ctx *thr = threads + thread_get_id();
	struct optee_msg_arg *arg = thr->rpc_arg;
	uint64_t carg = thr->rpc_carg;
	struct optee_msg_param *params = OPTEE_MSG_GET_PARAMS(arg);

	memset(arg, 0, OPTEE_MSG_GET_ARG_SIZE(1));
	arg->cmd = OPTEE_MSG_RPC_CMD_SHM_FREE;
	arg->ret = TEE_ERROR_GENERIC; /* in case value isn't updated */
	arg->num_params = 1;

	params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = bt;
	params[0].u.value.b = cookie;
	params[0].u.value.c = 0;

	reg_pair_from_64(carg, rpc_args + 1, rpc_args + 2);
	thread_rpc(rpc_args);
}


void thread_rpc_free_arg(uint64_t cookie)
{
	if (cookie) {
		uint32_t rpc_args[THREAD_RPC_NUM_ARGS] = {
			OPTEE_SMC_RETURN_RPC_FREE};

		reg_pair_from_64(cookie, rpc_args + 1, rpc_args + 2);
		thread_rpc(rpc_args);
	}
}

void thread_rpc_alloc_payload(size_t size, paddr_t *payload, uint64_t *cookie)
{
	thread_rpc_alloc(size, 8, OPTEE_MSG_RPC_SHM_TYPE_APPL, payload, cookie);
}

void thread_rpc_free_payload(uint64_t cookie)
{
	thread_rpc_free(OPTEE_MSG_RPC_SHM_TYPE_APPL, cookie);
}
