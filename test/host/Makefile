# Host-side regression test for the Somfy RTS waveform.
#
# Compiles the real components/somfy_rts/somfy_rts.c against stub ESP-IDF
# headers, captures the RMT symbol stream it would hand to the peripheral, and
# compares it pulse-by-pulse with the Arduino sketch's bit-banged output.
#
#   make && ./rtstest
#
# No hardware or ESP-IDF installation needed.

ROOT    := ../..
CFLAGS  := -std=gnu17 -Wall -Wextra -Wno-unused-parameter -g
INCLUDE := -I. -I$(ROOT)/components/somfy_rts/include \
           -I$(ROOT)/components/config_manager/include

rtstest: test.c $(ROOT)/components/somfy_rts/somfy_rts.c
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $^

.PHONY: check clean
check: rtstest
	./rtstest

clean:
	rm -f rtstest
