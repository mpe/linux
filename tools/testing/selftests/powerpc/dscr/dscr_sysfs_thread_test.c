/*
 * POWER Data Stream Control Register (DSCR) sysfs thread test
 *
 * This test updates the system wide DSCR default value through
 * sysfs interface which should then update all the CPU specific
 * DSCR default values which must also be then visible to threads
 * executing on individual CPUs on the system.
 *
 * Copyright (C) 2015 Anshuman Khandual <khandual@linux.vnet.ibm.com>, IBM
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#define _GNU_SOURCE
#include "dscr.h"

static pthread_mutex_t lock;	/* Pthread lock */
static cpu_set_t cpuset;	/* Thread cpu set */
static int target;		/* Thread target cpu */
static unsigned long *result;	/* Thread exit status array */

#define NR_ONLN sysconf(_SC_NPROCESSORS_ONLN)

static void *test_thread_dscr(void *in)
{
	unsigned long cur_dscr, cur_dscr_usr;
	unsigned long val = (unsigned long)in;

	pthread_mutex_lock(&lock);
	target++;
	if (target == NR_ONLN)
		target =  0;
	CPU_ZERO(&cpuset);
	CPU_SET(target, &cpuset);

	if (sched_setaffinity(0, sizeof(cpuset), &cpuset)) {
		perror("sched_setaffinity() failed");
		pthread_mutex_unlock(&lock);
		result[target] = 1;
		pthread_exit(&result[target]);
	}
	pthread_mutex_unlock(&lock);

	cur_dscr = get_dscr();
	cur_dscr_usr = get_dscr_usr();

	if (val != cur_dscr) {
		printf("[cpu %d] Kernel DSCR should be %ld but is %ld\n",
					sched_getcpu(), val, cur_dscr);
		result[target] = 1;
		pthread_exit(&result[target]);
	}

	if (val != cur_dscr_usr) {
		printf("[cpu %d] User DSCR should be %ld but is %ld\n",
					sched_getcpu(), val, cur_dscr_usr);
		result[target] = 1;
		pthread_exit(&result[target]);
	}
	result[target] = 0;
	pthread_exit(&result[target]);
}

static int check_cpu_dscr_thread(unsigned long val)
{
	pthread_t threads[NR_ONLN];
	unsigned long *status[NR_ONLN];
	unsigned long i;

	for (i = 0; i < NR_ONLN; i++) {
		if (pthread_create(&threads[i], NULL,
				test_thread_dscr, (void *)val)) {
			perror("pthread_create() failed");
			return 1;
		}
	}

	for (i = 0; i < NR_ONLN; i++) {
		if (pthread_join(threads[i], (void **)&(status[i]))) {
			perror("pthread_join() failed");
			return 1;
		}

		if (*status[i]) {
			printf(" %ldth thread join failed with %ld\n",
							i, *status[i]);
			return 1;
		}
	}
	return 0;
}

int dscr_sysfs_thread(void)
{
	unsigned long orig_dscr_default;
	int i, j;

	result = malloc(sizeof(unsigned long) * NR_ONLN);
	pthread_mutex_init(&lock, NULL);
	target = 0;
	orig_dscr_default = get_default_dscr();
	for (i = 0; i < COUNT; i++) {
		for (j = 0; j < DSCR_MAX; j++) {
			set_default_dscr(j);
			if (check_cpu_dscr_thread(j))
				goto fail;
		}
	}
	free(result);
	set_default_dscr(orig_dscr_default);
	return 0;
fail:
	free(result);
	set_default_dscr(orig_dscr_default);
	return 1;
}

int main(int argc, char *argv[])
{
	return test_harness(dscr_sysfs_thread, "dscr_sysfs_thread_test");
}
