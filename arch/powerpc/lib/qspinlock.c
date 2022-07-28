// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <asm/qspinlock.h>

#define MAX_NODES	4

struct qnode {
	struct qnode	*next;
	struct qspinlock *lock;
	u8		locked; /* 1 if lock acquired */
};

struct qnodes {
	int		count;
	struct qnode nodes[MAX_NODES];
};

static DEFINE_PER_CPU_ALIGNED(struct qnodes, qnodes);

static inline u32 encode_tail_cpu(void)
{
	return (smp_processor_id() + 1) << _Q_TAIL_CPU_OFFSET;
}

static inline int get_tail_cpu(u32 val)
{
	return (val >> _Q_TAIL_CPU_OFFSET) - 1;
}

/* Take the lock by setting the bit, no other CPUs may concurrently lock it. */
/* Take the lock by setting the lock bit, no other CPUs will touch it. */
static __always_inline void lock_set_locked(struct qspinlock *lock)
{
	u32 new = _Q_LOCKED_VAL;
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
	u32 new = _Q_LOCKED_VAL;
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

static inline void queued_spin_lock_mcs_queue(struct qspinlock *lock)
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
	node->locked = 0;

	tail = encode_tail_cpu();

	old = publish_tail_cpu(lock, tail);

	/*
	 * If there was a previous node; link it and wait until reaching the
	 * head of the waitqueue.
	 */
	if (old & _Q_TAIL_CPU_MASK) {
		struct qnode *prev = get_tail_qnode(lock, old);

		/* Link @node into the waitqueue. */
		WRITE_ONCE(prev->next, node);

		/* Wait for mcs node lock to be released */
		while (!node->locked)
			cpu_relax();

		smp_rmb(); /* acquire barrier for the mcs lock */
	}

	/* We're at the head of the waitqueue, wait for the lock. */
	while ((val = READ_ONCE(lock->val)) & _Q_LOCKED_VAL)
		cpu_relax();

	/* If we're the last queued, must clean up the tail. */
	if ((val & _Q_TAIL_CPU_MASK) == tail) {
		if (trylock_clear_tail_cpu(lock, val))
			goto release;
		/* Another waiter must have enqueued */
	}

	/* We must be the owner, just set the lock bit and acquire */
	lock_set_locked(lock);

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
	WRITE_ONCE(next->locked, 1);

release:
	qnodesp->count--; /* release the node */
}

void queued_spin_lock_slowpath(struct qspinlock *lock)
{
	queued_spin_lock_mcs_queue(lock);
}
EXPORT_SYMBOL(queued_spin_lock_slowpath);

#ifdef CONFIG_PARAVIRT_SPINLOCKS
void pv_spinlocks_init(void)
{
}
#endif

