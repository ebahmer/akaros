/*
 * Copyright (c) 2009 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 */

#ifdef __SHARC__
#pragma nosharc
#endif

#include <arch/arch.h>
#include <arch/bitmask.h>
#include <process.h>
#include <atomic.h>
#include <smp.h>
#include <pmap.h>
#include <trap.h>
#include <schedule.h>
#include <manager.h>
#include <stdio.h>
#include <assert.h>
#include <timing.h>
#include <hashtable.h>
#include <slab.h>
#include <sys/queue.h>
#include <frontend.h>

/* Process Lists */
struct proc_list proc_runnablelist = TAILQ_HEAD_INITIALIZER(proc_runnablelist);
spinlock_t runnablelist_lock = SPINLOCK_INITIALIZER;
struct kmem_cache *proc_cache;

/* Tracks which cores are idle, similar to the vcoremap.  Each value is the
 * physical coreid of an unallocated core. */
spinlock_t idle_lock = SPINLOCK_INITIALIZER;
uint32_t LCKD(&idle_lock) (RO idlecoremap)[MAX_NUM_CPUS];
uint32_t LCKD(&idle_lock) num_idlecores = 0;

/* Helper function to return a core to the idlemap.  It causes some more lock
 * acquisitions (like in a for loop), but it's a little easier.  Plus, one day
 * we might be able to do this without locks (for the putting). */
static void put_idle_core(uint32_t coreid)
{
	spin_lock(&idle_lock);
	idlecoremap[num_idlecores++] = coreid;
	spin_unlock(&idle_lock);
}

/* Other helpers, implemented later. */
static uint32_t get_free_vcoreid(struct proc *SAFE p, uint32_t prev);
static uint32_t get_busy_vcoreid(struct proc *SAFE p, uint32_t prev);
static bool is_mapped_vcore(struct proc *p, uint32_t pcoreid);
static uint32_t get_vcoreid(struct proc *SAFE p, uint32_t pcoreid);
static inline void __wait_for_ipi(const char *fnname);

/* PID management. */
#define PID_MAX 32767 // goes from 0 to 32767, with 0 reserved
static DECL_BITMASK(pid_bmask, PID_MAX + 1);
spinlock_t pid_bmask_lock = SPINLOCK_INITIALIZER;
struct hashtable *pid_hash;
spinlock_t pid_hash_lock; // initialized in proc_init

/* Finds the next free entry (zero) entry in the pid_bitmask.  Set means busy.
 * PID 0 is reserved (in proc_init).  A return value of 0 is a failure (and
 * you'll also see a warning, for now).  Consider doing this with atomics. */
static pid_t get_free_pid(void)
{
	static pid_t next_free_pid = 1;
	pid_t my_pid = 0;

	spin_lock(&pid_bmask_lock);
	// atomically (can lock for now, then change to atomic_and_return
	FOR_CIRC_BUFFER(next_free_pid, PID_MAX + 1, i) {
		// always points to the next to test
		next_free_pid = (next_free_pid + 1) % (PID_MAX + 1);
		if (!GET_BITMASK_BIT(pid_bmask, i)) {
			SET_BITMASK_BIT(pid_bmask, i);
			my_pid = i;
			break;
		}
	}
	spin_unlock(&pid_bmask_lock);
	if (!my_pid)
		warn("Shazbot!  Unable to find a PID!  You need to deal with this!\n");
	return my_pid;
}

/* Return a pid to the pid bitmask */
static void put_free_pid(pid_t pid)
{
	spin_lock(&pid_bmask_lock);
	CLR_BITMASK_BIT(pid_bmask, pid);
	spin_unlock(&pid_bmask_lock);
}

/* While this could be done with just an assignment, this gives us the
 * opportunity to check for bad transitions.  Might compile these out later, so
 * we shouldn't rely on them for sanity checking from userspace.  */
int __proc_set_state(struct proc *p, uint32_t state)
{
	uint32_t curstate = p->state;
	/* Valid transitions:
	 * C   -> RBS
	 * RBS -> RGS
	 * RGS -> RBS
	 * RGS -> W
	 * W   -> RBS
	 * RGS -> RBM
	 * RBM -> RGM
	 * RGM -> RBM
	 * RGM -> RBS
	 * RGS -> D
	 * RGM -> D
	 *
	 * These ought to be implemented later (allowed, not thought through yet).
	 * RBS -> D
	 * RBM -> D
	 *
	 * This isn't allowed yet, should be later.  Is definitely causable.
	 * C   -> D
	 */
	#if 1 // some sort of correctness flag
	switch (curstate) {
		case PROC_CREATED:
			if (state != PROC_RUNNABLE_S)
				panic("Invalid State Transition! PROC_CREATED to %d", state);
			break;
		case PROC_RUNNABLE_S:
			if (!(state & (PROC_RUNNING_S | PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNABLE_S to %d", state);
			break;
		case PROC_RUNNING_S:
			if (!(state & (PROC_RUNNABLE_S | PROC_RUNNABLE_M | PROC_WAITING |
			               PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNING_S to %d", state);
			break;
		case PROC_WAITING:
			if (state != PROC_RUNNABLE_S)
				panic("Invalid State Transition! PROC_WAITING to %d", state);
			break;
		case PROC_DYING:
			if (state != PROC_CREATED) // when it is reused (TODO)
				panic("Invalid State Transition! PROC_DYING to %d", state);
			break;
		case PROC_RUNNABLE_M:
			if (!(state & (PROC_RUNNING_M | PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNABLE_M to %d", state);
			break;
		case PROC_RUNNING_M:
			if (!(state & (PROC_RUNNABLE_S | PROC_RUNNABLE_M | PROC_DYING)))
				panic("Invalid State Transition! PROC_RUNNING_M to %d", state);
			break;
	}
	#endif
	p->state = state;
	return 0;
}

/* Returns a pointer to the proc with the given pid, or 0 if there is none */
struct proc *pid2proc(pid_t pid)
{
	spin_lock(&pid_hash_lock);
	struct proc *p = hashtable_search(pid_hash, (void*)pid);
	spin_unlock(&pid_hash_lock);
	/* if the refcnt was 0, decref and return 0 (we failed). (TODO) */
	if (p)
		proc_incref(p, 1); // TODO:(REF) to do this all atomically and not panic
	return p;
}

/* Performs any initialization related to processes, such as create the proc
 * cache, prep the scheduler, etc.  When this returns, we should be ready to use
 * any process related function. */
void proc_init(void)
{
	proc_cache = kmem_cache_create("proc", sizeof(struct proc),
	             MAX(HW_CACHE_ALIGN, __alignof__(struct proc)), 0, 0, 0);
	/* Init PID mask and hash.  pid 0 is reserved. */
	SET_BITMASK_BIT(pid_bmask, 0);
	spinlock_init(&pid_hash_lock);
	spin_lock(&pid_hash_lock);
	pid_hash = create_hashtable(100, __generic_hash, __generic_eq);
	spin_unlock(&pid_hash_lock);
	schedule_init();
	/* Init idle cores. Core 0 is the management core, and core 1 is
     * dedicated to the NIC currently */
	spin_lock(&idle_lock);
	#ifdef __CONFIG_NETWORKING__
	assert(num_cpus >= 2);
	int reserved_cores = 2;
	#else
	int reserved_cores = 1;
	#endif
	num_idlecores = num_cpus - reserved_cores;
	for (int i = 0; i < num_idlecores; i++)
		idlecoremap[i] = i + reserved_cores;
	spin_unlock(&idle_lock);
	atomic_init(&num_envs, 0);
}

void
proc_init_procinfo(struct proc* p)
{
	memset(&p->procinfo->vcoremap, 0, sizeof(p->procinfo->vcoremap));
	memset(&p->procinfo->pcoremap, 0, sizeof(p->procinfo->pcoremap));
	p->procinfo->num_vcores = 0;
	// TODO: change these too
	p->procinfo->pid = p->pid;
	p->procinfo->ppid = p->ppid;
	p->procinfo->tsc_freq = system_timing.tsc_freq;
	// TODO: maybe do something smarter here
	p->procinfo->max_harts = MAX(1,num_cpus-1);
}

/* Allocates and initializes a process, with the given parent.  Currently
 * writes the *p into **pp, and returns 0 on success, < 0 for an error.
 * Errors include:
 *  - ENOFREEPID if it can't get a PID
 *  - ENOMEM on memory exhaustion */
static error_t proc_alloc(struct proc *SAFE*SAFE pp, pid_t parent_id)
{
	error_t r;
	struct proc *p;

	if (!(p = kmem_cache_alloc(proc_cache, 0)))
		return -ENOMEM;

	{ INITSTRUCT(*p)

	// Setup the default map of where to get cache colors from
	p->cache_colors_map = global_cache_colors_map;
	p->next_cache_color = 0;

	/* Initialize the address space */
	if ((r = env_setup_vm(p)) < 0) {
		kmem_cache_free(proc_cache, p);
		return r;
	}

	/* Get a pid, then store a reference in the pid_hash */
	if (!(p->pid = get_free_pid())) {
		kmem_cache_free(proc_cache, p);
		return -ENOFREEPID;
	}
	spin_lock(&pid_hash_lock);
	hashtable_insert(pid_hash, (void*)p->pid, p);
	spin_unlock(&pid_hash_lock);

	/* Set the basic status variables. */
	spinlock_init(&p->proc_lock);
	p->exitcode = 0;
	p->ppid = parent_id;
	p->state = PROC_CREATED; // shouldn't go through state machine for init
	p->env_refcnt = 2; // one for the object, one for the ref we pass back
	p->env_flags = 0;
	p->env_entry = 0; // cheating.  this really gets set in load_icode
	p->procinfo->heap_bottom = (void*)UTEXT;
	p->heap_top = (void*)UTEXT;
	memset(&p->resources, 0, sizeof(p->resources));
	memset(&p->env_ancillary_state, 0, sizeof(p->env_ancillary_state));
	memset(&p->env_tf, 0, sizeof(p->env_tf));

	/* Initialize the contents of the e->procinfo structure */
	proc_init_procinfo(p);
	/* Initialize the contents of the e->procdata structure */

	/* Initialize the generic syscall ring buffer */
	SHARED_RING_INIT(&p->procdata->syscallring);
	/* Initialize the backend of the syscall ring buffer */
	BACK_RING_INIT(&p->syscallbackring,
	               &p->procdata->syscallring,
	               SYSCALLRINGSIZE);

	/* Initialize the generic sysevent ring buffer */
	SHARED_RING_INIT(&p->procdata->syseventring);
	/* Initialize the frontend of the sysevent ring buffer */
	FRONT_RING_INIT(&p->syseventfrontring,
	                &p->procdata->syseventring,
	                SYSEVENTRINGSIZE);
	*pp = p;
	atomic_inc(&num_envs);

	frontend_proc_init(p);

	printd("[%08x] new process %08x\n", current ? current->pid : 0, p->pid);
	} // INIT_STRUCT
	return 0;
}

/* Creates a process from the specified binary, which is of size size.
 * Currently, the binary must be a contiguous block of memory, which needs to
 * change.  On any failure, it just panics, which ought to be sorted. */
struct proc *proc_create(uint8_t *binary, size_t size)
{
	struct proc *p;
	error_t r;
	pid_t curid;

	curid = (current ? current->pid : 0);
	if ((r = proc_alloc(&p, curid)) < 0)
		panic("proc_create: %e", r); // one of 3 quaint usages of %e.
	if(binary != NULL)
		env_load_icode(p, NULL, binary, size);
	return p;
}

/* This is called by proc_decref, once the last reference to the process is
 * gone.  Don't call this otherwise (it will panic).  It will clean up the
 * address space and deallocate any other used memory. */
static void __proc_free(struct proc *p)
{
	physaddr_t pa;

	printd("[PID %d] freeing proc: %d\n", current ? current->pid : 0, p->pid);
	// All parts of the kernel should have decref'd before __proc_free is called
	assert(p->env_refcnt == 0);

	frontend_proc_free(p);

	// Free any colors allocated to this process
	if(p->cache_colors_map != global_cache_colors_map) {
		for(int i=0; i<llc_cache->num_colors; i++)
			cache_color_free(llc_cache, p->cache_colors_map);
		cache_colors_map_free(p->cache_colors_map);
	}

	// Flush all mapped pages in the user portion of the address space
	env_user_mem_free(p, 0, UVPT);
	/* These need to be free again, since they were allocated with a refcnt. */
	free_cont_pages(p->procinfo, LOG2_UP(PROCINFO_NUM_PAGES));
	free_cont_pages(p->procdata, LOG2_UP(PROCDATA_NUM_PAGES));

	env_pagetable_free(p);
	p->env_pgdir = 0;
	p->env_cr3 = 0;

	/* Remove self from the pid hash, return PID.  Note the reversed order. */
	spin_lock(&pid_hash_lock);
	if (!hashtable_remove(pid_hash, (void*)p->pid))
		panic("Proc not in the pid table in %s", __FUNCTION__);
	spin_unlock(&pid_hash_lock);
	put_free_pid(p->pid);
	atomic_dec(&num_envs);

	/* Dealloc the struct proc */
	kmem_cache_free(proc_cache, p);
}

/* Whether or not actor can control target.  Note we currently don't need
 * locking for this. TODO: think about that, esp wrt proc's dying. */
bool proc_controls(struct proc *actor, struct proc *target)
{
	return ((actor == target) || (target->ppid == actor->pid));
}

/* Dispatches a process to run, either on the current core in the case of a
 * RUNNABLE_S, or on its partition in the case of a RUNNABLE_M.  This should
 * never be called to "restart" a core.  This expects that the "instructions"
 * for which core(s) to run this on will be in the vcoremap, which needs to be
 * set externally.
 *
 * When a process goes from RUNNABLE_M to RUNNING_M, its vcoremap will be
 * "packed" (no holes in the vcore->pcore mapping), vcore0 will continue to run
 * it's old core0 context, and the other cores will come in at the entry point.
 * Including in the case of preemption.
 *
 * This won't return if the current core is going to be one of the processes
 * cores (either for _S mode or for _M if it's in the vcoremap).  proc_run will
 * eat your reference if it does not return. */
void proc_run(struct proc *p)
{
	bool self_ipi_pending = FALSE;
	spin_lock_irqsave(&p->proc_lock);
	switch (p->state) {
		case (PROC_DYING):
			spin_unlock_irqsave(&p->proc_lock);
			printk("Process %d not starting due to async death\n", p->pid);
			// if we're a worker core, smp_idle, o/w return
			if (!management_core())
				smp_idle(); // this never returns
			return;
		case (PROC_RUNNABLE_S):
			__proc_set_state(p, PROC_RUNNING_S);
			/* We will want to know where this process is running, even if it is
			 * only in RUNNING_S.  can use the vcoremap, which makes death easy.
			 * Also, this is the signal used in trap.c to know to save the tf in
			 * env_tf. */
			// TODO: (VSEQ) signal these vcore changes
			p->procinfo->num_vcores = 0;
			__map_vcore(p, 0, core_id()); // sort of.  this needs work.
			spin_unlock_irqsave(&p->proc_lock);
			/* Transferring our reference to startcore, where p will become
			 * current.  If it already is, decref in advance.  This is similar
			 * to __startcore(), in that it sorts out the refcnt accounting.  */
			if (current == p)
				proc_decref(p, 1);
			proc_startcore(p, &p->env_tf);
			break;
		case (PROC_RUNNABLE_M):
			/* vcoremap[i] holds the coreid of the physical core allocated to
			 * this process.  It is set outside proc_run.  For the active
			 * message, a0 = struct proc*, a1 = struct trapframe*.   */
			if (p->procinfo->num_vcores) {
				__proc_set_state(p, PROC_RUNNING_M);
				int i = 0;
				/* Up the refcnt, since num_vcores are going to start using this
				 * process and have it loaded in their 'current'. */
				p->env_refcnt += p->procinfo->num_vcores; // TODO: (REF) use incref
				/* If the core we are running on is in the vcoremap, we will get
				 * an IPI (once we reenable interrupts) and never return. */
				if (is_mapped_vcore(p, core_id()))
					self_ipi_pending = TRUE;
				// TODO: handle silly state (HSS)
				// set virtual core 0 to run the main context on transition
				if (p->env_flags & PROC_TRANSITION_TO_M) {
					p->env_flags &= !PROC_TRANSITION_TO_M;
#ifdef __IVY__
					send_active_message(p->procinfo->vcoremap[0].pcoreid,
					                    __startcore, p,
					                    &p->env_tf, (void *SNT)0);
#else
					send_active_message(p->procinfo->vcoremap[0].pcoreid,
					                    (void *)__startcore, (void *)p,
										(void *)&p->env_tf, 0);
#endif
					i = 1; // start at vcore1 in the loop below
				}
				/* handle the others. */
				for (/* i set above */; i < p->procinfo->num_vcores; i++)
#ifdef __IVY__
					send_active_message(p->procinfo->vcoremap[i].pcoreid,
					                    __startcore, p,
										(trapframe_t *CT(1))NULL, (void *SNT)i);
#else
					send_active_message(p->procinfo->vcoremap[i].pcoreid,
					                    (void *)__startcore, (void *)p,
										(void *)0, (void *)i);
#endif
			} else {
				warn("Tried to proc_run() an _M with no vcores!");
			}
			/* Unlock and decref/wait for the IPI if one is pending.  This will
			 * eat the reference if we aren't returning. 
			 *
			 * There a subtle race avoidance here.  proc_startcore can handle a
			 * death message, but we can't have the startcore come after the
			 * death message.  Otherwise, it would look like a new process.  So
			 * we hold the lock til after we send our message, which prevents a
			 * possible death message.
			 * - Likewise, we need interrupts to be disabled, in case one of the
			 *   messages was for us, and reenable them after letting go of the
			 *   lock.  This is done by spin_lock_irqsave, so be careful if you
			 *   change this.
			 * - Note there is no guarantee this core's interrupts were on, so
			 *   it may not get the message for a while... */
			__proc_unlock_ipi_pending(p, self_ipi_pending);
			break;
		default:
			spin_unlock_irqsave(&p->proc_lock);
			panic("Invalid process state %p in proc_run()!!", p->state);
	}
}

/* Runs the given context (trapframe) of process p on the core this code
 * executes on.
 *
 * Given we are RUNNING_*, an IPI for death or preemption could come in:
 * 1. death attempt (IPI to kill whatever is on your core):
 * 		we don't need to worry about protecting the stack, since we're
 * 		abandoning ship - just need to get a good cr3 and decref current, which
 * 		the death handler will do.
 * 		If a death IPI comes in, we immediately stop this function and will
 * 		never come back.
 * 2. preempt attempt (IPI to package state and maybe run something else):
 * 		- if a preempt attempt comes in while we're in the kernel, it'll
 * 		just set a flag.  we could attempt to bundle the kernel state
 * 		and rerun it later, but it's really messy (and possibly given
 * 		back to userspace).  we'll disable ints, check this flag, and if
 * 		so, handle the preemption using the same funcs as the normal
 * 		preemption handler.  nonblocking kernel calls will just slow
 * 		down the preemption while they work.  blocking kernel calls will
 * 		need to package their state properly anyway.
 *
 * TODO: in general, think about when we no longer need the stack, in case we
 * are preempted and expected to run again from somewhere else.  we can't
 * expect to have the kernel stack around anymore.  the nice thing about being
 * at this point is that we are just about ready to give up the stack anyways.
 *
 * I think we need to make it such that the kernel in "process context" never
 * gets removed from the core (displaced from its stack) without going through
 * some "bundling" code.
 *
 * A note on refcnting: this function will not return, and your proc reference
 * will end up stored in current.  This will make no changes to p's refcnt, so
 * do your accounting such that there is only the +1 for current.  This means if
 * it is already in current (like in the trap return path), don't up it.  If
 * it's already in current and you have another reference (like pid2proc or from
 * an IPI), then down it (which is what happens in __startcore()).  If it's not
 * in current and you have one reference, like proc_run(non_current_p), then
 * also do nothing.  The refcnt for your *p will count for the reference stored
 * in current. */
void proc_startcore(struct proc *p, trapframe_t *tf) {
	// it's possible to be DYING, but it's a rare race.
	//if (p->state & (PROC_RUNNING_S | PROC_RUNNING_M))
	//	printk("dying before (re)startcore on core %d\n", core_id());
	// sucks to have ints disabled when doing env_decref and possibly freeing
	disable_irq();
	if (per_cpu_info[core_id()].preempt_pending) {
		// TODO: handle preemption
		// the functions will need to consider deal with current like down below
		panic("Preemption not supported!");
	}
	/* If the process wasn't here, then we need to load its address space. */
	if (p != current) {
		/* Do not incref here.  We were given the reference to current,
		 * pre-upped. */
		lcr3(p->env_cr3);
		/* This is "leaving the process context" of the previous proc.  The
		 * previous lcr3 unloaded the previous proc's context.  This should
		 * rarely happen, since we usually proactively leave process context,
		 * but is the fallback. */
		if (current)
			proc_decref(current, 1);
		set_current_proc(p);
	}
	/* need to load our silly state, preferably somewhere other than here so we
	 * can avoid the case where the context was just running here.  it's not
	 * sufficient to do it in the "new process" if-block above (could be things
	 * like page faults that cause us to keep the same process, but want a
	 * different context.
	 * for now, we load this silly state here. (TODO) (HSS)
	 * We also need this to be per trapframe, and not per process...
	 */
	env_pop_ancillary_state(p);
	env_pop_tf(tf);
}

/*
 * Destroys the given process.  This may be called from another process, a light
 * kernel thread (no real process context), asynchronously/cross-core, or from
 * the process on its own core.
 *
 * Here's the way process death works:
 * 0. grab the lock (protects state transition and core map)
 * 1. set state to dying.  that keeps the kernel from doing anything for the
 * process (like proc_running it).
 * 2. figure out where the process is running (cross-core/async or RUNNING_M)
 * 3. IPI to clean up those cores (decref, etc).
 * 4. Unlock
 * 5. Clean up your core, if applicable
 * (Last core/kernel thread to decref cleans up and deallocates resources.)
 *
 * Note that some cores can be processing async calls, but will eventually
 * decref.  Should think about this more, like some sort of callback/revocation.
 *
 * This will eat your reference if it won't return.  Note that this function
 * needs to change anyways when we make __death more like __preempt.  (TODO) */
void proc_destroy(struct proc *p)
{
	bool self_ipi_pending = FALSE;
	spin_lock_irqsave(&p->proc_lock);

	/* TODO: (DEATH) look at this again when we sort the __death IPI */
	if (current == p)
		self_ipi_pending = TRUE;

	switch (p->state) {
		case PROC_DYING: // someone else killed this already.
			__proc_unlock_ipi_pending(p, self_ipi_pending);
			return;
		case PROC_RUNNABLE_M:
			/* Need to reclaim any cores this proc might have, even though it's
			 * not running yet. */
			__proc_take_allcores(p, NULL, NULL, NULL, NULL);
			// fallthrough
		case PROC_RUNNABLE_S:
			// Think about other lists, like WAITING, or better ways to do this
			deschedule_proc(p);
			break;
		case PROC_RUNNING_S:
			#if 0
			// here's how to do it manually
			if (current == p) {
				lcr3(boot_cr3);
				proc_decref(p, 1); // this decref is for the cr3
				current = NULL;
			}
			#endif
			send_active_message(p->procinfo->vcoremap[0].pcoreid, __death,
			                   (void *SNT)0, (void *SNT)0, (void *SNT)0);
			// TODO: (VSEQ) signal these vcore changes
			__unmap_vcore(p, 0);
			#if 0
			/* right now, RUNNING_S only runs on a mgmt core (0), not cores
			 * managed by the idlecoremap.  so don't do this yet. */
			put_idle_core(p->procinfo->vcoremap[0].pcoreid);
			#endif
			break;
		case PROC_RUNNING_M:
			/* Send the DEATH message to every core running this process, and
			 * deallocate the cores.
			 * The rule is that the vcoremap is set before proc_run, and reset
			 * within proc_destroy */
			__proc_take_allcores(p, __death, (void *SNT)0, (void *SNT)0,
			                     (void *SNT)0);
			break;
		default:
			panic("Weird state(0x%08x) in proc_destroy", p->state);
	}
	__proc_set_state(p, PROC_DYING);
	/* this decref is for the process in general */
	p->env_refcnt--; // TODO (REF)
	//proc_decref(p, 1);

	/* Unlock and possible decref and wait.  A death IPI should be on its way,
	 * either from the RUNNING_S one, or from proc_take_cores with a __death.
	 * in general, interrupts should be on when you call proc_destroy locally,
	 * but currently aren't for all things (like traphandlers). */
	__proc_unlock_ipi_pending(p, self_ipi_pending);
	return;
}

/* Helper function.  Starting from prev, it will find the next free vcoreid,
 * which is the next vcore that is not valid.
 * You better hold the lock before calling this. */
static uint32_t get_free_vcoreid(struct proc *SAFE p, uint32_t prev)
{
	uint32_t i;
	for (i = prev; i < MAX_NUM_CPUS; i++)
		if (!p->procinfo->vcoremap[i].valid)
			break;
	if (i + 1 >= MAX_NUM_CPUS)
		warn("At the end of the vcorelist.  Might want to check that out.");
	return i;
}

/* Helper function.  Starting from prev, it will find the next busy vcoreid,
 * which is the next vcore that is valid.
 * You better hold the lock before calling this. */
static uint32_t get_busy_vcoreid(struct proc *SAFE p, uint32_t prev)
{
	uint32_t i;
	for (i = prev; i < MAX_NUM_CPUS; i++)
		if (p->procinfo->vcoremap[i].valid)
			break;
	if (i + 1 >= MAX_NUM_CPUS)
		warn("At the end of the vcorelist.  Might want to check that out.");
	return i;
}

/* Helper function.  Is the given pcore a mapped vcore?  Hold the lock before
 * calling. */
static bool is_mapped_vcore(struct proc *p, uint32_t pcoreid)
{
	return p->procinfo->pcoremap[pcoreid].valid;
}

/* Helper function.  Find the vcoreid for a given physical core id for proc p.
 * You better hold the lock before calling this.  Panics on failure. */
static uint32_t get_vcoreid(struct proc *SAFE p, uint32_t pcoreid)
{
	assert(is_mapped_vcore(p, pcoreid));
	return p->procinfo->pcoremap[pcoreid].vcoreid;
}

/* Use this when you are waiting for an IPI that you sent yourself.  In most
 * cases, interrupts should already be on (like after a spin_unlock_irqsave from
 * process context), but aren't always, like in proc_destroy().  We might be
 * able to remove the enable_irq in the future.  Think about this (TODO).
 *
 * Note this means all non-proc management interrupt handlers must return (which
 * they need to do anyway), so that we get back to this point.  */
static inline void __wait_for_ipi(const char *fnname)
{
	enable_irq();
	udelay(1000000);
	panic("Waiting too long on core %d for an IPI in %s()!", core_id(), fnname);
}

/* Yields the calling core.  Must be called locally (not async) for now.
 * - If RUNNING_S, you just give up your time slice and will eventually return.
 * - If RUNNING_M, you give up the current vcore (which never returns), and
 *   adjust the amount of cores wanted/granted.
 * - If you have only one vcore, you switch to RUNNABLE_M.  When you run again,
 *   you'll have one guaranteed core, starting from the entry point.
 *
 * - RES_CORES amt_wanted will be the amount running after taking away the
 *   yielder, unless there are none left, in which case it will be 1.
 *
 * This does not return (abandon_core()), so it will eat your reference.  */
void proc_yield(struct proc *SAFE p)
{
	spin_lock_irqsave(&p->proc_lock);
	switch (p->state) {
		case (PROC_RUNNING_S):
			p->env_tf= *current_tf;
			env_push_ancillary_state(p);
			__proc_set_state(p, PROC_RUNNABLE_S);
			schedule_proc(p);
			break;
		case (PROC_RUNNING_M):
			// TODO: (VSEQ) signal these vcore changes
			// give up core
			__unmap_vcore(p, get_vcoreid(p, core_id()));
			p->resources[RES_CORES].amt_granted = --(p->procinfo->num_vcores);
			p->resources[RES_CORES].amt_wanted = p->procinfo->num_vcores;
			// add to idle list
			put_idle_core(core_id());
			// last vcore?  then we really want 1, and to yield the gang
			if (p->procinfo->num_vcores == 0) {
				// might replace this with m_yield, if we have it directly
				p->resources[RES_CORES].amt_wanted = 1;
				__proc_set_state(p, PROC_RUNNABLE_M);
				schedule_proc(p);
			}
			break;
		default:
			// there are races that can lead to this (async death, preempt, etc)
			panic("Weird state(0x%08x) in proc_yield", p->state);
	}
	spin_unlock_irqsave(&p->proc_lock);
	proc_decref(p, 1);
	/* Clean up the core and idle.  For mgmt cores, they will ultimately call
	 * manager, which will call schedule() and will repick the yielding proc. */
	abandon_core();
}

/* Gives process p the additional num cores listed in pcorelist.  You must be
 * RUNNABLE_M or RUNNING_M before calling this.  If you're RUNNING_M, this will
 * startup your new cores at the entry point with their virtual IDs.  If you're
 * RUNNABLE_M, you should call proc_run after this so that the process can start
 * to use its cores.
 *
 * If you're *_S, make sure your core0's TF is set (which is done when coming in
 * via arch/trap.c and we are RUNNING_S), change your state, then call this.
 * Then call proc_run().
 *
 * The reason I didn't bring the _S cases from core_request over here is so we
 * can keep this family of calls dealing with only *_Ms, to avoiding caring if
 * this is called from another core, and to avoid the need_to_idle business.
 * The other way would be to have this function have the side effect of changing
 * state, and finding another way to do the need_to_idle.
 *
 * The returned bool signals whether or not a stack-crushing IPI will come in
 * once you unlock after this function.
 *
 * WARNING: You must hold the proc_lock before calling this! */
bool __proc_give_cores(struct proc *SAFE p, uint32_t *pcorelist, size_t num)
{ TRUSTEDBLOCK
	bool self_ipi_pending = FALSE;
	uint32_t free_vcoreid = 0;
	switch (p->state) {
		case (PROC_RUNNABLE_S):
		case (PROC_RUNNING_S):
			panic("Don't give cores to a process in a *_S state!\n");
			break;
		case (PROC_DYING):
			panic("Attempted to give cores to a DYING process.\n");
			break;
		case (PROC_RUNNABLE_M):
			// set up vcoremap.  list should be empty, but could be called
			// multiple times before proc_running (someone changed their mind?)
			if (p->procinfo->num_vcores) {
				printk("[kernel] Yaaaaaarrrrr!  Giving extra cores, are we?\n");
				// debugging: if we aren't packed, then there's a problem
				// somewhere, like someone forgot to take vcores after
				// preempting.
				for (int i = 0; i < p->procinfo->num_vcores; i++)
					assert(p->procinfo->vcoremap[i].valid);
			}
			// TODO: (VSEQ) signal these vcore changes
			// add new items to the vcoremap
			for (int i = 0; i < num; i++) {
				// find the next free slot, which should be the next one
				free_vcoreid = get_free_vcoreid(p, free_vcoreid);
				printd("setting vcore %d to pcore %d\n", free_vcoreid,
				       pcorelist[i]);
				__map_vcore(p, free_vcoreid, pcorelist[i]);
				p->procinfo->num_vcores++;
			}
			break;
		case (PROC_RUNNING_M):
			/* Up the refcnt, since num cores are going to start using this
			 * process and have it loaded in their 'current'. */
			// TODO: (REF) use proc_incref once we have atomics
			p->env_refcnt += num;
			// TODO: (VSEQ) signal these vcore changes
			for (int i = 0; i < num; i++) {
				free_vcoreid = get_free_vcoreid(p, free_vcoreid);
				//todo
				printd("setting vcore %d to pcore %d\n", free_vcoreid,
				       pcorelist[i]);
				__map_vcore(p, free_vcoreid, pcorelist[i]);
				p->procinfo->num_vcores++;
				send_active_message(pcorelist[i], __startcore, p,
				                    (struct trapframe *)0,
				                    (void*SNT)free_vcoreid);
				if (pcorelist[i] == core_id())
					self_ipi_pending = TRUE;
			}
			break;
		default:
			panic("Weird proc state %d in proc_give_cores()!\n", p->state);
	}
	return self_ipi_pending;
}

/* Makes process p's coremap look like pcorelist (add, remove, etc).  Caller
 * needs to know what cores are free after this call (removed, failed, etc).
 * This info will be returned via corelist and *num.  This will send message to
 * any cores that are getting removed.
 *
 * Before implementing this, we should probably think about when this will be
 * used.  Implies preempting for the message.  The more that I think about this,
 * the less I like it.  For now, don't use this, and think hard before
 * implementing it.
 *
 * WARNING: You must hold the proc_lock before calling this! */
bool __proc_set_allcores(struct proc *SAFE p, uint32_t *pcorelist,
                         size_t *num, amr_t message,TV(a0t) arg0,
                         TV(a1t) arg1, TV(a2t) arg2)
{
	panic("Set all cores not implemented.\n");
}

/* Takes from process p the num cores listed in pcorelist, using the given
 * message for the active message (__death, __preempt, etc).  Like the others
 * in this function group, bool signals whether or not an IPI is pending.
 *
 * WARNING: You must hold the proc_lock before calling this! */
bool __proc_take_cores(struct proc *SAFE p, uint32_t *pcorelist,
                       size_t num, amr_t message, TV(a0t) arg0,
                       TV(a1t) arg1, TV(a2t) arg2)
{ TRUSTEDBLOCK
	uint32_t vcoreid, pcoreid;
	bool self_ipi_pending = FALSE;
	switch (p->state) {
		case (PROC_RUNNABLE_M):
			assert(!message);
			break;
		case (PROC_RUNNING_M):
			assert(message);
			break;
		default:
			panic("Weird state %d in proc_take_cores()!\n", p->state);
	}
	spin_lock(&idle_lock);
	assert((num <= p->procinfo->num_vcores) &&
	       (num_idlecores + num <= num_cpus));
	spin_unlock(&idle_lock);
	// TODO: (VSEQ) signal these vcore changes
	for (int i = 0; i < num; i++) {
		vcoreid = get_vcoreid(p, pcorelist[i]);
		pcoreid = p->procinfo->vcoremap[vcoreid].pcoreid;
		assert(pcoreid == pcorelist[i]);
		if (message) {
			if (pcoreid == core_id())
				self_ipi_pending = TRUE;
			send_active_message(pcoreid, message, arg0, arg1, arg2);
		}
		// give the pcore back to the idlecoremap
		__unmap_vcore(p, vcoreid);
		put_idle_core(pcoreid);
	}
	p->procinfo->num_vcores -= num;
	p->resources[RES_CORES].amt_granted -= num;
	return self_ipi_pending;
}

/* Takes all cores from a process, which must be in an _M state.  Cores are
 * placed back in the idlecoremap.  If there's a message, such as __death or
 * __preempt, it will be sent to the cores.  The bool signals whether or not an
 * IPI is coming in once you unlock.
 *
 * WARNING: You must hold the proc_lock before calling this! */
bool __proc_take_allcores(struct proc *SAFE p, amr_t message,
                          TV(a0t) arg0, TV(a1t) arg1, TV(a2t) arg2)
{
	uint32_t active_vcoreid = 0, pcoreid;
	bool self_ipi_pending = FALSE;
	switch (p->state) {
		case (PROC_RUNNABLE_M):
			assert(!message);
			break;
		case (PROC_RUNNING_M):
			assert(message);
			break;
		default:
			panic("Weird state %d in proc_take_allcores()!\n", p->state);
	}
	spin_lock(&idle_lock);
	assert(num_idlecores + p->procinfo->num_vcores <= num_cpus); // sanity
	spin_unlock(&idle_lock);
	// TODO: (VSEQ) signal these vcore changes
	for (int i = 0; i < p->procinfo->num_vcores; i++) {
		// find next active vcore
		active_vcoreid = get_busy_vcoreid(p, active_vcoreid);
		pcoreid = p->procinfo->vcoremap[active_vcoreid].pcoreid;
		if (message) {
			if (pcoreid == core_id())
				self_ipi_pending = TRUE;
			send_active_message(pcoreid, message, arg0, arg1, arg2);
		}
		// give the pcore back to the idlecoremap
		__unmap_vcore(p, active_vcoreid);
		put_idle_core(pcoreid);
	}
	p->procinfo->num_vcores = 0;
	p->resources[RES_CORES].amt_granted = 0;
	return self_ipi_pending;
}

/* Helper, to be used when unlocking after calling the above functions that
 * might cause an IPI to be sent.  TODO inline this, so the __FUNCTION__ works.
 * Will require an overhaul of core_request (break it up, etc) */
void __proc_unlock_ipi_pending(struct proc *p, bool ipi_pending)
{
	if (ipi_pending) {
		p->env_refcnt--; // TODO: (REF) (atomics)
		spin_unlock_irqsave(&p->proc_lock);
		__wait_for_ipi(__FUNCTION__);
	} else {
		spin_unlock_irqsave(&p->proc_lock);
	}
}

/* Helper to do the vcore->pcore and inverse mapping.  Hold the lock when
 * calling. */
void __map_vcore(struct proc *p, uint32_t vcoreid, uint32_t pcoreid)
{
	p->procinfo->vcoremap[vcoreid].pcoreid = pcoreid;
	p->procinfo->vcoremap[vcoreid].valid = TRUE;
	p->procinfo->pcoremap[pcoreid].vcoreid = vcoreid;
	p->procinfo->pcoremap[pcoreid].valid = TRUE;
}

/* Helper to unmap the vcore->pcore and inverse mapping.  Hold the lock when
 * calling. */
void __unmap_vcore(struct proc *p, uint32_t vcoreid)
{
	p->procinfo->vcoremap[vcoreid].valid = FALSE;
	p->procinfo->pcoremap[p->procinfo->vcoremap[vcoreid].pcoreid].valid = FALSE;
}

/* This takes a referenced process and ups the refcnt by count.  If the refcnt
 * was already 0, then someone has a bug, so panic.  Check out the Documentation
 * for brutal details about refcnting.
 *
 * Implementation aside, the important thing is that we atomically increment
 * only if it wasn't already 0.  If it was 0, panic.
 *
 * TODO: (REF) change to use CAS / atomics. */
void proc_incref(struct proc *p, size_t count)
{
	spin_lock_irqsave(&p->proc_lock);
	if (p->env_refcnt)
		p->env_refcnt += count;
	else
		panic("Tried to incref a proc with no existing refernces!");
	spin_unlock_irqsave(&p->proc_lock);
}

/* When the kernel is done with a process, it decrements its reference count.
 * When the count hits 0, no one is using it and it should be freed.  "Last one
 * out" actually finalizes the death of the process.  This is tightly coupled
 * with the previous function (incref)
 *
 * TODO: (REF) change to use CAS.  Note that when we do so, we may be holding
 * the process lock when calling __proc_free(). */
void proc_decref(struct proc *p, size_t count)
{
	spin_lock_irqsave(&p->proc_lock);
	p->env_refcnt -= count;
	size_t refcnt = p->env_refcnt; // need to copy this in so it's not reloaded
	spin_unlock_irqsave(&p->proc_lock);
	// if we hit 0, no one else will increment and we can check outside the lock
	if (!refcnt)
		__proc_free(p);
	if (refcnt < 0)
		panic("Too many decrefs!");
}

/* Active message handler to start a process's context on this core.  Tightly
 * coupled with proc_run() */
#ifdef __IVY__
void __startcore(trapframe_t *tf, uint32_t srcid, struct proc *CT(1) a0,
                 trapframe_t *CT(1) a1, void *SNT a2)
#else
void __startcore(trapframe_t *tf, uint32_t srcid, void * a0, void * a1,
                 void * a2)
#endif
{
	uint32_t coreid = core_id();
	uint32_t vcoreid = (uint32_t)a2;
	struct proc *p_to_run = (struct proc *CT(1))a0;
	trapframe_t local_tf;
	trapframe_t *tf_to_pop = (trapframe_t *CT(1))a1;

	printd("[kernel] Startcore on physical core %d for Process %d\n",
	       coreid, p_to_run->pid);
	assert(p_to_run);
	// TODO: handle silly state (HSS)
	if (!tf_to_pop) {
		tf_to_pop = &local_tf;
		memset(tf_to_pop, 0, sizeof(*tf_to_pop));
		proc_init_trapframe(tf_to_pop, vcoreid, p_to_run->env_entry,
		                    p_to_run->procdata->stack_pointers[vcoreid]);
	}
	/* the sender of the amsg increfed, thinking we weren't running current. */
	if (p_to_run == current)
		proc_decref(p_to_run, 1);
	proc_startcore(p_to_run, tf_to_pop);
}

/* Stop running whatever context is on this core, load a known-good cr3, and
 * 'idle'.  Note this leaves no trace of what was running. This "leaves the
 * process's context. */
void abandon_core(void)
{
	if (current)
		__abandon_core();
	smp_idle();
}

/* Active message handler to clean up the core when a process is dying.
 * Note this leaves no trace of what was running.
 * It's okay if death comes to a core that's already idling and has no current.
 * It could happen if a process decref'd before proc_startcore could incref. */
void __death(trapframe_t *tf, uint32_t srcid, void *SNT a0, void *SNT a1,
             void *SNT a2)
{
	abandon_core();
}

void print_idlecoremap(void)
{
	spin_lock(&idle_lock);
	printk("There are %d idle cores.\n", num_idlecores);
	for (int i = 0; i < num_idlecores; i++)
		printk("idlecoremap[%d] = %d\n", i, idlecoremap[i]);
	spin_unlock(&idle_lock);
}

void print_allpids(void)
{
	spin_lock(&pid_hash_lock);
	if (hashtable_count(pid_hash)) {
		hashtable_itr_t *phtable_i = hashtable_iterator(pid_hash);
		printk("PID      STATE    \n");
		printk("------------------\n");
		do {
			struct proc *p = hashtable_iterator_value(phtable_i);
			printk("%8d %s\n", hashtable_iterator_key(phtable_i),
			       p ? procstate2str(p->state) : "(null)");
		} while (hashtable_iterator_advance(phtable_i));
	}
	spin_unlock(&pid_hash_lock);
}

void print_proc_info(pid_t pid)
{
	int j = 0;
	struct proc *p = pid2proc(pid);
	// not concerned with a race on the state...
	if (!p) {
		printk("Bad PID.\n");
		return;
	}
	spinlock_debug(&p->proc_lock);
	spin_lock_irqsave(&p->proc_lock);
	printk("struct proc: %p\n", p);
	printk("PID: %d\n", p->pid);
	printk("PPID: %d\n", p->ppid);
	printk("State: 0x%08x\n", p->state);
	printk("Refcnt: %d\n", p->env_refcnt - 1); // don't report our ref
	printk("Flags: 0x%08x\n", p->env_flags);
	printk("CR3(phys): 0x%08x\n", p->env_cr3);
	printk("Num Vcores: %d\n", p->procinfo->num_vcores);
	printk("Vcoremap:\n");
	for (int i = 0; i < p->procinfo->num_vcores; i++) {
		j = get_busy_vcoreid(p, j);
		printk("\tVcore %d: Pcore %d\n", j, p->procinfo->vcoremap[j].pcoreid);
		j++;
	}
	printk("Resources:\n");
	for (int i = 0; i < MAX_NUM_RESOURCES; i++)
		printk("\tRes type: %02d, amt wanted: %08d, amt granted: %08d\n", i,
		       p->resources[i].amt_wanted, p->resources[i].amt_granted);
	printk("Vcore 0's Last Trapframe:\n");
	print_trapframe(&p->env_tf);
	spin_unlock_irqsave(&p->proc_lock);
	proc_decref(p, 1); /* decref for the pid2proc reference */
}
