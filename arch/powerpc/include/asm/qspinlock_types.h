/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ASM_POWERPC_QSPINLOCK_TYPES_H
#define _ASM_POWERPC_QSPINLOCK_TYPES_H

#include <linux/types.h>
#include <asm/byteorder.h>

typedef struct qspinlock {
	union {
		u32 val;

#ifdef __LITTLE_ENDIAN
		struct {
			u16	locked;
			u8	reserved[2];
		};
#else
		struct {
			u8	reserved[2];
			u16	locked;
		};
#endif
	};
} arch_spinlock_t;

#define	__ARCH_SPIN_LOCK_UNLOCKED	{ { .val = 0 } }

/*
 * Bitfields in the lock word:
 *
 *     0: locked bit
 * 16-31: tail cpu (+1)
 */
#define	_Q_SET_MASK(type)	(((1U << _Q_ ## type ## _BITS) - 1)\
				      << _Q_ ## type ## _OFFSET)
#define _Q_LOCKED_OFFSET	0
#define _Q_LOCKED_BITS		1
#define _Q_LOCKED_MASK		_Q_SET_MASK(LOCKED)
#define _Q_LOCKED_VAL		(1U << _Q_LOCKED_OFFSET)

#define _Q_TAIL_CPU_OFFSET	16
#define _Q_TAIL_CPU_BITS	(32 - _Q_TAIL_CPU_OFFSET)
#define _Q_TAIL_CPU_MASK	_Q_SET_MASK(TAIL_CPU)

#if CONFIG_NR_CPUS >= (1U << _Q_TAIL_CPU_BITS)
#error "qspinlock does not support such large CONFIG_NR_CPUS"
#endif

#endif /* _ASM_POWERPC_QSPINLOCK_TYPES_H */
