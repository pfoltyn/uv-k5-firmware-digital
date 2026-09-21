# Host-side build for the AX.25 and APRS harness. Nothing here is part of the
# firmware build; it compiles app/ax25.c and app/aprs.c natively so the encoder
# that runs on the radio and the one under test are the same source file.

TOP     := ../..
CC      ?= cc
# -MMD because these objects depend on headers in the firmware tree: without it
# a change to a struct leaves a stale object behind and the test aborts on an
# ABI mismatch rather than failing a check.
CFLAGS  += -std=c11 -O2 -Wall -Wextra -Wpedantic -MMD -I $(TOP)
LDLIBS  += -lm

WAVBIN  := aprs_wav
WAVOBJS := aprs_wav.o ax25.o aprs.o

TESTBIN := ax25_test
TESTOBJS := ax25_test.o ax25.o aprs.o

AFSKBIN  := afsk_test
AFSKOBJS := afsk_test.o bk4819-afsk-stub.o
# the stub swaps SysTick and the chip for a simulated clock, so the bit timing
# can be tested without the radio
AFSKFLAGS := -DBK4819_AFSK_HOST_TEST

# direwolf provides atest and gen_packets. brew install direwolf
ATEST       ?= atest
GEN_PACKETS ?= gen_packets

CALL  ?= SP9ABC-7
GRID  ?= JO90xa
MSG   ?= UV-K5 APRS test

.PHONY: all test tones demo clean

all: $(WAVBIN) $(TESTBIN) $(AFSKBIN)

$(WAVBIN): $(WAVOBJS)
	$(CC) $(CFLAGS) -o $@ $(WAVOBJS) $(LDLIBS)

$(TESTBIN): $(TESTOBJS)
	$(CC) $(CFLAGS) -o $@ $(TESTOBJS) $(LDLIBS)

$(AFSKBIN): $(AFSKOBJS)
	$(CC) $(CFLAGS) -o $@ $(AFSKOBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ax25.o: $(TOP)/app/ax25.c
	$(CC) $(CFLAGS) -c $< -o $@

aprs.o: $(TOP)/app/aprs.c
	$(CC) $(CFLAGS) -c $< -o $@

afsk_test.o: afsk_test.c
	$(CC) $(CFLAGS) $(AFSKFLAGS) -c $< -o $@

bk4819-afsk-stub.o: $(TOP)/driver/bk4819-afsk.c
	$(CC) $(CFLAGS) $(AFSKFLAGS) -c $< -o $@

test: $(WAVBIN) $(TESTBIN) $(AFSKBIN)
	@echo "== AX.25 and APRS unit tests =="
	./$(TESTBIN)
	@echo
	@echo "== Bell 202 bit clock =="
	./$(AFSKBIN)
	@echo
	@echo "== our frames through direwolf =="
	ATEST=$(ATEST) GEN_PACKETS=$(GEN_PACKETS) ./roundtrip.sh
	@echo
	@echo "== does a 2400Hz space still decode? =="
	ATEST=$(ATEST) ./tones.sh

# The Phase 1 gate on its own: whether the BK4819's FFSK tone pair is acceptable
# to a standard TNC, answered without a radio.
tones: $(WAVBIN)
	ATEST=$(ATEST) ./tones.sh

demo: $(WAVBIN)
	./$(WAVBIN) -o aprs.wav "$(CALL)>APZK5F,WIDE1-1,WIDE2-1:>$(MSG)"
	$(ATEST) aprs.wav

clean:
	rm -f $(WAVBIN) $(WAVOBJS) $(TESTBIN) $(TESTOBJS) $(AFSKBIN) $(AFSKOBJS) *.wav *.d

-include $(WAVOBJS:.o=.d) $(TESTOBJS:.o=.d) $(AFSKOBJS:.o=.d)
