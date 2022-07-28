// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <asm/qspinlock.h>
#include <asm/paravirt.h>

#define MAX_NODES	4

struct qnode {
	struct qnode	*next;
	struct qspinlock *lock;
	int		cpu;
	int		yield_cpu;
	u8		locked; /* 1 if lock acquired */
};

struct qnodes {
	int		count;
	struct qnode nodes[MAX_NODES];
};

/* Tuning parameters */
static int STEAL_SPINS __read_mostly = (1<<5);
#if _Q_SPIN_TRY_LOCK_STEAL == 1
static const bool MAYBE_STEALERS = true;
#else
static bool MAYBE_STEALERS __read_mostly = true;
#endif
static int HEAD_SPINS __read_mostly = (1<<8);

static bool pv_yield_owner __read_mostly = true;
static bool pv_yield_allow_steal __read_mostly = false;
static bool pv_yield_prev __read_mostly = true;
static bool pv_yield_propagate_owner __read_mostly = true;
static bool pv_prod_head __read_mostly = false;

static DEFINE_PER_CPU_ALIGNED(struct qnodes, qnodes);

static __always_inline int get_steal_spins(bool paravirt)
{
	return STEAL_SPINS;
}

static __always_inline int get_head_spins(bool paravirt)
{
	return HEAD_SPINS;
}

static inline u32 encode_tail_cpu(void)
{
	return (smp_processor_id() + 1) << _Q_TAIL_CPU_OFFSET;
}

static inline int get_tail_cpu(u32 val)
{
	return (val >> _Q_TAIL_CPU_OFFSET) - 1;
}

static inline int get_owner_cpu(u32 val)
{
	return (val & _Q_OWNER_CPU_MASK) >> _Q_OWNER_CPU_OFFSET;
}

/* Take the lock by setting the lock bit, no other CPUs will touch it. */
static __always_inline void lock_set_locked(struct qspinlock *lock)
{
	u32 new = queued_spin_get_locked_val();
	u32 prev;

	asm volatile(
"1:	lwarx	%0,0,%1,%3	# lock_set_locked			\n"
"	or	%0,%0,%2						\n"
"	stwcx.	%0,0,%1							\n"
"	bne-	1b							\n"
"\t"	PPC_ACQUIRE_BARRIER "						\n"
	: "=&r" (prev)
	: "r" (&lock->val), "r" (new),
	  "i" (IS_ENABLED(CONFIG_PPC64) ? 1 : 0)
	: "cr0", "memory");
}

/* Take lock, clearing tail, cmpxchg with old (which must not be locked) */
static __always_inline int trylock_clear_tail_cpu(struct qspinlock *lock, u32 old)
{
	u32 new = queued_spin_get_locked_val();
	u32 prev;

	BUG_ON(old & _Q_LOCKED_VAL);

	asm volatile(
"1:	lwarx	%0,0,%1,%4	# trylock_clear_tail_cpu		\n"
"	cmpw	0,%0,%2							\n"
"	bne-	2f							\n"
"	stwcx.	%3,0,%1							\n"
"	bne-	1b							\n"
"\t"	PPC_ACQUIRE_BARRIER "						\n"
"2:									\n"
	: "=&r" (prev)
	: "r" (&lock->val), "r"(old), "r" (new),
	  "i" (IS_ENABLED(CONFIG_PPC64) ? 1 : 0)
	: "cr0", "memory");

	if (likely(prev == old))
		return 1;
	return 0;
}

static __always_inline u32 __trylock_cmpxchg(struct qspinlock *lock, u32 old, u32 new)
{
	u32 prev;

	BUG_ON(old & _Q_LOCKED_VAL);

	asm volatile(
"1:	lwarx	%0,0,%1,%4	# queued_spin_trylock_cmpxchg		\n"
"	cmpw	0,%0,%2							\n"
"	bne-	2f							\n"
"	stwcx.	%3,0,%1							\n"
"	bne-	1b							\n"
"\t"	PPC_ACQUIRE_BARRIER "						\n"
"2:									\n"
	: "=&r" (prev)
	: "r" (&lock->val), "r"(old), "r" (new),
	  "i" (IS_ENABLED(CONFIG_PPC64) ? 1 : 0)
	: "cr0", "memory");

	return prev;
}

/* Take lock, preserving tail, cmpxchg with val (which must not be locked) */
static __always_inline int trylock_with_tail_cpu(struct qspinlock *lock, u32 val)
{
	u32 newval = queued_spin_get_locked_val() | (val & _Q_TAIL_CPU_MASK);

	if (__trylock_cmpxchg(lock, val, newval) == val)
		return 1;
	else
		return 0;
}

/*
 * Publish our tail, replacing previous tail. Return previous value.
 *
 * This provides a release barrier for publishing node, and an acquire barrier
 * for getting the old node.
 */
static __always_inline u32 publish_tail_cpu(struct qspinlock *lock, u32 tail)
{
	u32 prev, tmp;

	asm volatile(
"\t"	PPC_RELEASE_BARRIER "						\n"
"1:	lwarx	%0,0,%2		# publish_tail_cpu			\n"
"	andc	%1,%0,%4						\n"
"	or	%1,%1,%3						\n"
"	stwcx.	%1,0,%2							\n"
"	bne-	1b							\n"
	: "=&r" (prev), "=&r"(tmp)
	: "r" (&lock->val), "r" (tail), "r"(_Q_TAIL_CPU_MASK)
	: "cr0", "memory");

	return prev;
}

static __always_inline u32 lock_set_mustq(struct qspinlock *lock)
{
	u32 new = _Q_MUST_Q_VAL;
	u32 prev;

	asm volatile(
"1:	lwarx	%0,0,%1		# lock_set_mustq			\n"
"	or	%0,%0,%2						\n"
"	stwcx.	%0,0,%1							\n"
"	bne-	1b							\n"
	: "=&r" (prev)
	: "r" (&lock->val), "r" (new)
	: "cr0", "memory");

	return prev;
}

static __always_inline u32 lock_clear_mustq(struct qspinlock *lock)
{
	u32 new = _Q_MUST_Q_VAL;
	u32 prev;

	asm volatile(
"1:	lwarx	%0,0,%1		# lock_clear_mustq			\n"
"	andc	%0,%0,%2						\n"
"	stwcx.	%0,0,%1							\n"
"	bne-	1b							\n"
	: "=&r" (prev)
	: "r" (&lock->val), "r" (new)
	: "cr0", "memory");

	return prev;
}

static struct qnode *get_tail_qnode(struct qspinlock *lock, u32 val)
{
	int cpu = get_tail_cpu(val);
	struct qnodes *qnodesp = per_cpu_ptr(&qnodes, cpu);
	int idx;

	for (idx = 0; idx < MAX_NODES; idx++) {
		struct qnode *qnode = &qnodesp->nodes[idx];
		if (qnode->lock == lock)
			return qnode;
	}

	BUG();
}

static __always_inline void __yield_to_locked_owner(struct qspinlock *lock, u32 val, bool paravirt, bool clear_mustq)
{
	int owner;
	u32 yield_count;

	BUG_ON(!(val & _Q_LOCKED_VAL));

	if (!paravirt)
		goto relax;

	if (!pv_yield_owner)
		goto relax;

	owner = get_owner_cpu(val);
	yield_count = yield_count_of(owner);

	if ((yield_count & 1) == 0)
		goto relax; /* owner vcpu is running */

	/*
	 * Read the lock word after sampling the yield count. On the other side
	 * there may a wmb because the yield count update is done by the
	 * hypervisor preemption and the value update by the OS, however this
	 * ordering might reduce the chance of out of order accesses and
	 * improve the heuristic.
	 */
	smp_rmb();

	if (READ_ONCE(lock->val) == val) {
		if (clear_mustq)
			lock_clear_mustq(lock);
		yield_to_preempted(owner, yield_count);
		if (clear_mustq)
			lock_set_mustq(lock);
		/* Don't relax if we yielded. Maybe we should? */
		return;
	}
relax:
	cpu_relax();
}

static __always_inline void yield_to_locked_owner(struct qspinlock *lock, u32 val, bool paravirt)
{
	__yield_to_locked_owner(lock, val, paravirt, false);
}

static __always_inline void yield_head_to_locked_owner(struct qspinlock *lock, u32 val, bool paravirt, bool clear_mustq)
{
	__yield_to_locked_owner(lock, val, paravirt, clear_mustq);
}

static __always_inline void propagate_yield_cpu(struct qnode *node, u32 val, int *set_yield_cpu, bool paravirt)
{
	struct qnode *next;
	int owner;

	if (!paravirt)
		return;
	if (!pv_yield_propagate_owner)
		return;

	owner = get_owner_cpu(val);
	if (*set_yield_cpu == owner)
		return;

	next = READ_ONCE(node->next);
	if (!next)
		return;

	if (vcpu_is_preempted(owner)) {
		next->yield_cpu = owner;
		*set_yield_cpu = owner;
	} else if (*set_yield_cpu != -1) {
		next->yield_cpu = owner;
		*set_yield_cpu = owner;
	}
}

static __always_inline void yield_to_prev(struct qspinlock *lock, struct qnode *node, int prev_cpu, bool paravirt)
{
	u32 yield_count;
	int yield_cpu;

	if (!paravirt)
		goto relax;

	if (!pv_yield_propagate_owner)
		goto yield_prev;

	yield_cpu = READ_ONCE(node->yield_cpu);
	if (yield_cpu == -1) {
		/* Propagate back the -1 CPU */
		if (node->next && node->next->yield_cpu != -1)
			node->next->yield_cpu = yield_cpu;
		goto yield_prev;
	}

	yield_count = yield_count_of(yield_cpu);
	if ((yield_count & 1) == 0)
		goto yield_prev; /* owner vcpu is running */

	smp_rmb();

	if (yield_cpu == node->yield_cpu) {
		if (node->next && node->next->yield_cpu != yield_cpu)
			node->next->yield_cpu = yield_cpu;
		yield_to_preempted(yield_cpu, yield_count);
		return;
	}

yield_prev:
	if (!pv_yield_prev)
		goto relax;

	yield_count = yield_count_of(prev_cpu);
	if ((yield_count & 1) == 0)
		goto relax; /* owner vcpu is running */

	smp_rmb(); /* See yield_to_locked_owner comment */

	if (!node->locked) {
		yield_to_preempted(prev_cpu, yield_count);
		return;
	}

relax:
	cpu_relax();
}


static __always_inline bool try_to_steal_lock(struct qspinlock *lock, bool paravirt)
{
	int iters;

	/* Attempt to steal the lock */
	for (;;) {
		u32 val = READ_ONCE(lock->val);

		if (val & _Q_MUST_Q_VAL)
			break;

		if (unlikely(!(val & _Q_LOCKED_VAL))) {
			if (trylock_with_tail_cpu(lock, val))
				return true;
			continue;
		}

		yield_to_locked_owner(lock, val, paravirt);

		iters++;

		if (iters >= get_steal_spins(paravirt))
			break;
	}

	return false;
}

static __always_inline void queued_spin_lock_mcs_queue(struct qspinlock *lock, bool paravirt)
{
	struct qnodes *qnodesp;
	struct qnode *next, *node;
	u32 val, old, tail;
	int idx;

	BUILD_BUG_ON(CONFIG_NR_CPUS >= (1U << _Q_TAIL_CPU_BITS));

	qnodesp = this_cpu_ptr(&qnodes);
	if (unlikely(qnodesp->count == MAX_NODES)) {
		while (!queued_spin_trylock(lock))
			cpu_relax();
		return;
	}

	idx = qnodesp->count++;
	/*
	 * Ensure that we increment the head node->count before initialising
	 * the actual node. If the compiler is kind enough to reorder these
	 * stores, then an IRQ could overwrite our assignments.
	 */
	barrier();
	node = &qnodesp->nodes[idx];
	node->next = NULL;
	node->lock = lock;
	node->cpu = smp_processor_id();
	node->yield_cpu = -1;
	node->locked = 0;

	tail = encode_tail_cpu();

	old = publish_tail_cpu(lock, tail);

	/*
	 * If there was a previous node; link it and wait until reaching the
	 * head of the waitqueue.
	 */
	if (old & _Q_TAIL_CPU_MASK) {
		struct qnode *prev = get_tail_qnode(lock, old);
		int prev_cpu = get_tail_cpu(old);

		/* Link @node into the waitqueue. */
		WRITE_ONCE(prev->next, node);

		/* Wait for mcs node lock to be released */
		while (!node->locked)
			yield_to_prev(lock, node, prev_cpu, paravirt);

		/* Clear out stale propagated yield_cpu */
		if (paravirt && pv_yield_propagate_owner && node->yield_cpu != -1)
			node->yield_cpu = -1;

		smp_rmb(); /* acquire barrier for the mcs lock */
	}

	if (!MAYBE_STEALERS) {
		int set_yield_cpu = -1;

		/* We're at the head of the waitqueue, wait for the lock. */
		while ((val = READ_ONCE(lock->val)) & _Q_LOCKED_VAL) {
			propagate_yield_cpu(node, val, &set_yield_cpu, paravirt);
			yield_head_to_locked_owner(lock, val, paravirt, false);
		}

		/* If we're the last queued, must clean up the tail. */
		if ((val & _Q_TAIL_CPU_MASK) == tail) {
			if (trylock_clear_tail_cpu(lock, val))
				goto release;
			/* Another waiter must have enqueued. */
		}

		/* We must be the owner, just set the lock bit and acquire */
		lock_set_locked(lock);
	} else {
		int set_yield_cpu = -1;
		int iters = 0;
		bool set_mustq = false;

again:
		/* We're at the head of the waitqueue, wait for the lock. */
		while ((val = READ_ONCE(lock->val)) & _Q_LOCKED_VAL) {
			propagate_yield_cpu(node, val, &set_yield_cpu, paravirt);
			yield_head_to_locked_owner(lock, val, paravirt,
					pv_yield_allow_steal && set_mustq);

			iters++;
			if (!set_mustq && iters >= get_head_spins(paravirt)) {
				set_mustq = true;
				lock_set_mustq(lock);
				val |= _Q_MUST_Q_VAL;
			}
		}

		/* If we're the last queued, must clean up the tail. */
		if ((val & _Q_TAIL_CPU_MASK) == tail) {
			if (trylock_clear_tail_cpu(lock, val))
				goto release;
			/* Another waiter must have enqueued, or lock stolen. */
		} else {
			if (trylock_with_tail_cpu(lock, val))
				goto unlock_next;
		}
		goto again;
	}

unlock_next:
	/* contended path; must wait for next != NULL (MCS protocol) */
	while (!(next = READ_ONCE(node->next)))
		cpu_relax();

	/*
	 * Unlock the next mcs waiter node. Release barrier is not required
	 * here because the acquirer is only accessing the lock word, and
	 * the acquire barrier we took the lock with orders that update vs
	 * this store to locked. The corresponding barrier is the smp_rmb()
	 * acquire barrier for mcs lock, above.
	 */
	if (paravirt && pv_prod_head) {
		int next_cpu = next->cpu;
		WRITE_ONCE(next->locked, 1);
		if (vcpu_is_preempted(next_cpu))
			prod_cpu(next_cpu);
	} else {
		WRITE_ONCE(next->locked, 1);
	}

release:
	qnodesp->count--; /* release the node */
}

void queued_spin_lock_slowpath(struct qspinlock *lock)
{
	if (IS_ENABLED(CONFIG_PARAVIRT_SPINLOCKS) && is_shared_processor()) {
		if (try_to_steal_lock(lock, true))
			return;
		queued_spin_lock_mcs_queue(lock, true);
	} else {
		if (try_to_steal_lock(lock, false))
			return;
		queued_spin_lock_mcs_queue(lock, false);
	}
}
EXPORT_SYMBOL(queued_spin_lock_slowpath);

#ifdef CONFIG_PARAVIRT_SPINLOCKS
void pv_spinlocks_init(void)
{
}
#endif

#include <linux/debugfs.h>
static int steal_spins_set(void *data, u64 val)
{
#if _Q_SPIN_TRY_LOCK_STEAL == 1
	/* MAYBE_STEAL remains true */
	STEAL_SPINS = val;
#else
	static DEFINE_MUTEX(lock);

	mutex_lock(&lock);
	if (val && !STEAL_SPINS) {
		MAYBE_STEALERS = true;
		/* wait for waiter to go away */
		synchronize_rcu();
		STEAL_SPINS = val;
	} else if (!val && STEAL_SPINS) {
		STEAL_SPINS = val;
		/* wait for all possible stealers to go away */
		synchronize_rcu();
		MAYBE_STEALERS = false;
	} else {
		STEAL_SPINS = val;
	}
	mutex_unlock(&lock);
#endif

	return 0;
}

static int steal_spins_get(void *data, u64 *val)
{
	*val = STEAL_SPINS;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_steal_spins, steal_spins_get, steal_spins_set, "%llu\n");

static int head_spins_set(void *data, u64 val)
{
	HEAD_SPINS = val;

	return 0;
}

static int head_spins_get(void *data, u64 *val)
{
	*val = HEAD_SPINS;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_head_spins, head_spins_get, head_spins_set, "%llu\n");

static int pv_yield_owner_set(void *data, u64 val)
{
	pv_yield_owner = !!val;

	return 0;
}

static int pv_yield_owner_get(void *data, u64 *val)
{
	*val = pv_yield_owner;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_pv_yield_owner, pv_yield_owner_get, pv_yield_owner_set, "%llu\n");

static int pv_yield_allow_steal_set(void *data, u64 val)
{
	pv_yield_allow_steal = !!val;

	return 0;
}

static int pv_yield_allow_steal_get(void *data, u64 *val)
{
	*val = pv_yield_allow_steal;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_pv_yield_allow_steal, pv_yield_allow_steal_get, pv_yield_allow_steal_set, "%llu\n");

static int pv_yield_prev_set(void *data, u64 val)
{
	pv_yield_prev = !!val;

	return 0;
}

static int pv_yield_prev_get(void *data, u64 *val)
{
	*val = pv_yield_prev;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_pv_yield_prev, pv_yield_prev_get, pv_yield_prev_set, "%llu\n");

static int pv_yield_propagate_owner_set(void *data, u64 val)
{
	pv_yield_propagate_owner = !!val;

	return 0;
}

static int pv_yield_propagate_owner_get(void *data, u64 *val)
{
	*val = pv_yield_propagate_owner;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_pv_yield_propagate_owner, pv_yield_propagate_owner_get, pv_yield_propagate_owner_set, "%llu\n");

static int pv_prod_head_set(void *data, u64 val)
{
	pv_prod_head = !!val;

	return 0;
}

static int pv_prod_head_get(void *data, u64 *val)
{
	*val = pv_prod_head;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(fops_pv_prod_head, pv_prod_head_get, pv_prod_head_set, "%llu\n");

static __init int spinlock_debugfs_init(void)
{
	debugfs_create_file("qspl_steal_spins", 0600, arch_debugfs_dir, NULL, &fops_steal_spins);
	debugfs_create_file("qspl_head_spins", 0600, arch_debugfs_dir, NULL, &fops_head_spins);
	if (is_shared_processor()) {
		debugfs_create_file("qspl_pv_yield_owner", 0600, arch_debugfs_dir, NULL, &fops_pv_yield_owner);
		debugfs_create_file("qspl_pv_yield_allow_steal", 0600, arch_debugfs_dir, NULL, &fops_pv_yield_allow_steal);
		debugfs_create_file("qspl_pv_yield_prev", 0600, arch_debugfs_dir, NULL, &fops_pv_yield_prev);
		debugfs_create_file("qspl_pv_yield_propagate_owner", 0600, arch_debugfs_dir, NULL, &fops_pv_yield_propagate_owner);
		debugfs_create_file("qspl_pv_prod_head", 0600, arch_debugfs_dir, NULL, &fops_pv_prod_head);
	}

	return 0;
}
device_initcall(spinlock_debugfs_init);

