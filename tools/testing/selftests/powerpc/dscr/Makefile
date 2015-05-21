PROGS := dscr_default_test dscr_explicit_test dscr_user_test

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
