CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
SANITIZERS ?= -fsanitize=address,undefined

.PHONY: test test-core test-hook clean

test: test-core test-hook

test-core:
	$(CC) -std=c11 $(CFLAGS) $(SANITIZERS) -I. tests/test_rtc_s3_sim.c -o test_rtc_s3_sim
	./test_rtc_s3_sim

test-hook:
	$(CC) -std=gnu11 $(CFLAGS) $(SANITIZERS) -I. -Ishellcode_hv -Itests/support tests/test_rtc_s3_hook_sim.c -o test_rtc_s3_hook_sim
	./test_rtc_s3_hook_sim

clean:
	rm -f test_rtc_s3_sim test_rtc_s3_hook_sim
