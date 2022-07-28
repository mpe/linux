// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/atomic.h>
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

static inline int encode_tail_cpu(void)
{
	return (smp_processor_id() + 1) << _Q_TAIL_CPU_OFFSET;
}

static inline int get_tail_cpu(int val)
{
	return (val >> _Q_TAIL_CPU_OFFSET) - 1;
}

/* Take the lock by setting the bit, no other CPUs may concurrently lock it. */
static __always_inline void lock_set_locked(struct qspinlock *lock)
{
	atomic_or(_Q_LOCKED_VAL, &lock->val);
	__atomic_acquire_fence();
}

/* Take lock, clearing tail, cmpxchg with val (which must not be locked) */
static __always_inline int trylock_clear_tail_cpu(struct qspinlock *lock, int val)
{
	int newval = _Q_LOCKED_VAL;

	if (atomic_cmpxchg_acquire(&lock->val, val, newval) == val)
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
static __always_inline int publish_tail_cpu(struct qspinlock *lock, int tail)
{
	for (;;) {
		int val = atomic_read(&lock->val);
		int newval = (val & ~_Q_TAIL_CPU_MASK) | tail;
		int old;

		old = atomic_cmpxchg(&lock->val, val, newval);
		if (old == val)
			return old;
	}
}

static struct qnode *get_tail_qnode(struct qspinlock *lock, int val)
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
	int val, old, tail;
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
	while ((val = atomic_read(&lock->val)) & _Q_LOCKED_VAL)
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

