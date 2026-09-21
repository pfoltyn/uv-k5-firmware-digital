# Host-side build for the SMS protocol layer. Nothing here is part of the
# firmware build; it compiles app/sms.c natively so the protocol that runs on the
# radio and the one under test are the same source file.

TOP     := ../..
CC      ?= cc
# -MMD because these objects depend on headers in the firmware tree: without it a
# change to a struct leaves a stale object behind and the test aborts on an ABI
# mismatch rather than failing a check.
CFLAGS  += -std=c11 -O2 -Wall -Wextra -Wpedantic -MMD -I $(TOP)

# Built with crypto on, because that is the shipping configuration and it changes
# SMS_TEXT_PER_FRAG: testing the other one would test a build nobody runs.
CFLAGS  += -DENABLE_SMS_CRYPTO -DSMS_CRYPTO_HOST_TEST

BIN     := sms_test
OBJS    := sms_test.o sms.o sms_crypto.o

# The link state machine, against a stub that plays the far end. sms_link.c keeps
# its state in file-scope statics so two stations cannot be instantiated in one
# process; the stub does not need them to be.
LINKBIN  := link_test
LINKOBJS := link_test.o sms_link.o sms.o sms_crypto.o
LINKFLAGS := -DSMS_LINK_HOST_TEST

# ChaCha20 against LibreSSL and RFC 8439, SipHash against its published vectors.
CRYPTOBIN  := crypto_test
CRYPTOOBJS := crypto_test.o sms_crypto.o
CRYPTOFLAGS := -DSMS_CRYPTO_HOST_TEST

.PHONY: all test clean

all: $(BIN) $(LINKBIN) $(CRYPTOBIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(LINKBIN): $(LINKOBJS)
	$(CC) $(CFLAGS) -o $@ $(LINKOBJS)

$(CRYPTOBIN): $(CRYPTOOBJS)
	$(CC) $(CFLAGS) -o $@ $(CRYPTOOBJS)

crypto_test.o: crypto_test.c
	$(CC) $(CFLAGS) $(CRYPTOFLAGS) -c $< -o $@

sms_crypto.o: $(TOP)/app/sms_crypto.c
	$(CC) $(CFLAGS) $(CRYPTOFLAGS) -c $< -o $@

link_test.o: link_test.c
	$(CC) $(CFLAGS) $(LINKFLAGS) -c $< -o $@

sms_link.o: $(TOP)/app/sms_link.c
	$(CC) $(CFLAGS) $(LINKFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

sms.o: $(TOP)/app/sms.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BIN) $(LINKBIN) $(CRYPTOBIN)
	@echo "== protocol =="
	./$(BIN)
	@echo
	@echo "== link state machine =="
	./$(LINKBIN)
	@echo
	@echo "== crypto =="
	./$(CRYPTOBIN)

clean:
	rm -f $(BIN) $(OBJS) $(LINKBIN) $(LINKOBJS) $(CRYPTOBIN) $(CRYPTOOBJS) *.d

-include $(OBJS:.o=.d) $(LINKOBJS:.o=.d) $(CRYPTOOBJS:.o=.d)
