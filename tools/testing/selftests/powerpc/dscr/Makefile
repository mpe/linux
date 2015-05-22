PROGS := dscr_default_test dscr_explicit_test dscr_user_test	\
	 dscr_inherit_test dscr_inherit_exec_test		\
	 dscr_sysfs_test dscr_sysfs_thread_test

CFLAGS := $(CFLAGS) -lpthread

all: $(PROGS)

$(PROGS): ../harness.c

run_tests: all
	@-for PROG in $(PROGS); do \
		./$$PROG; \
	done;

clean:
	rm -f $(PROGS) *.o

.PHONY: all run_tests clean
