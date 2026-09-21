/* Host tests for app/sms.c.
 *
 * The framing is straightforward; the retransmission logic is not, and most of
 * these exercise the cases where it goes subtly wrong - a stale acknowledgement
 * marking the wrong message delivered, a lost ACK deadlocking both ends, a frame
 * that passes the CRC by chance corrupting the reassembly state.
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/sms.h"

static int checks;
static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __func__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

// Linked in because sms.h takes SMS_CRYPTO_OVERHEAD from the crypto header; this
// suite does not seal anything itself, but the seed symbol has to exist.
uint32_t SMS_CryptoHostSeed(void) { return 0x2A2A2A2Au; }

#define ALICE 0x1234u
#define BOB   0x5678u

// One frame from sender to receiver, through the wire format, optionally with a
// bit flipped on the way. Returns false if the receiver rejected it.
static bool Deliver(const SMS_Frame_t *sent, SMS_Frame_t *got, int flip_bit)
{
    uint8_t wire[SMS_FRAME_BYTES];

    SMS_FrameEncode(sent, wire);

    if (flip_bit >= 0)
        wire[flip_bit >> 3] ^= (uint8_t)(1u << (flip_bit & 7));

    return SMS_FrameDecode(wire, got);
}

static void TestCrcCheckValue(void)
{
    // The documented check value for CRC-16/CCITT-FALSE over the digits 1 to 9.
    const uint8_t data[] = "123456789";

    CHECK(SMS_Crc(data, 9) == 0x29B1, "got %04x want 29b1", SMS_Crc(data, 9));
}

static void TestFrameRoundTrip(void)
{
    SMS_Frame_t in, out;

    memset(&in, 0, sizeof in);

    in.type   = SMS_TYPE_DATA;
    in.frag   = 1;
    in.frags  = 3;
    in.src    = ALICE;
    in.dst    = BOB;
    in.msg_id = 0xA5;
    in.len    = 5;
    memcpy(in.payload, "hello", 5);

    CHECK(Deliver(&in, &out, -1), "did not decode");
    CHECK(out.type == in.type && out.frag == 1 && out.frags == 3, "flags wrong");
    CHECK(out.src == ALICE && out.dst == BOB, "addresses wrong");
    CHECK(out.msg_id == 0xA5 && out.len == 5, "id or length wrong");
    CHECK(memcmp(out.payload, "hello", 5) == 0, "payload wrong");
}

static void TestEverySingleBitErrorIsCaught(void)
{
    // No forward error correction here, so every corruption must be refused: a
    // frame accepted with a flipped bit is a message shown wrong.
    SMS_Frame_t in, out;

    memset(&in, 0, sizeof in);

    in.type = SMS_TYPE_DATA;  in.frags = 1;  in.src = ALICE;  in.dst = BOB;
    in.len  = 20;
    memcpy(in.payload, "twenty bytes exactly", 20);

    uint32_t accepted = 0;

    for (int bit = 0; bit < SMS_FRAME_BYTES * 8; bit++)
        if (Deliver(&in, &out, bit))
            accepted++;

    CHECK(accepted == 0, "%u of %d single bit errors were accepted",
          accepted, SMS_FRAME_BYTES * 8);
}

static void TestNoLongRunsOnAir(void)
{
    /* The regression guard for a bug that looked like a radio fault and was not.
     *
     * Zero filled padding gave a 13 character message 234 consecutive identical bits,
     * which no clock recovery survives: the sync word matched, the data clocked
     * through, and then a quarter of a second with no transition destroyed the timing
     * and every CRC failed. Anything that reintroduces a constant fill will trip this
     * before it reaches a radio. */
    static const uint8_t lengths[] = { 1, 5, 13, 30, SMS_PAYLOAD_BYTES };

    for (size_t c = 0; c < sizeof lengths / sizeof lengths[0]; c++) {
        SMS_Frame_t in;
        uint8_t     wire[SMS_FRAME_BYTES];

        memset(&in, 0, sizeof in);
        in.type = SMS_TYPE_DATA; in.frags = 1; in.src = ALICE; in.dst = BOB;
        in.len  = lengths[c];

        /* A payload with no long runs of its own, so what is measured is the
         * encoder's contribution rather than the caller's. What a caller hands over
         * is its business - real payloads are ciphertext or text, neither of which
         * has long runs - and the fault here was the padding the encoder adds. */
        for (uint8_t i = 0; i < in.len; i++)
            in.payload[i] = (uint8_t)('A' + (i % 26));

        SMS_FrameEncode(&in, wire);

        // The padding itself must alternate, which is the actual fix.
        for (uint32_t i = SMS_HEADER_BYTES + in.len;
             i < SMS_FRAME_BYTES - SMS_CRC_BYTES; i++)
            CHECK(wire[i] == 0xAAu, "padding byte %u is %02x, not alternating",
                  i, wire[i]);

        uint32_t run = 1, worst = 1;
        uint8_t  prev = (uint8_t)((wire[0] >> 7) & 1u);

        for (uint32_t b = 1; b < (uint32_t)SMS_FRAME_BYTES * 8u; b++) {
            const uint8_t bit = (uint8_t)((wire[b >> 3] >> (7 - (b & 7))) & 1u);

            run = (bit == prev) ? run + 1 : 1;

            if (run > worst)
                worst = run;

            prev = bit;
        }

        // 32 is generous: random ciphertext gives about ten, and the failure this
        // guards against was 234.
        CHECK(worst <= 32, "len %u gave a run of %u identical bits", lengths[c], worst);
    }
}

static void TestOutOfRangeFieldsRejected(void)
{
    /* A frame can pass a 16 bit CRC by chance about once in 65536. If it then
     * claims four fragments or a 200 byte payload it corrupts the reassembly
     * state rather than merely being wrong, so the fields are range checked and
     * the CRC recomputed to make each frame otherwise valid. */
    uint8_t     wire[SMS_FRAME_BYTES];
    SMS_Frame_t in, out;

    memset(&in, 0, sizeof in);
    in.type = SMS_TYPE_DATA; in.frags = 1; in.src = ALICE; in.dst = BOB; in.len = 4;

    static const struct { const char *what; uint32_t off; uint8_t val; } bad[] = {
        { "length past the payload", 6, SMS_PAYLOAD_BYTES + 1 },
        { "length of 255",           6, 255                   },
        { "zero fragments",          0, 0x00                   },
    };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        SMS_FrameEncode(&in, wire);
        wire[bad[i].off] = bad[i].val;

        const uint16_t crc = SMS_Crc(wire, SMS_FRAME_BYTES - 2);

        wire[SMS_FRAME_BYTES - 2] = (uint8_t)(crc & 0xFFu);
        wire[SMS_FRAME_BYTES - 1] = (uint8_t)(crc >> 8);

        CHECK(!SMS_FrameDecode(wire, &out), "accepted %s", bad[i].what);
    }

    // And a fragment index outside the fragment count.
    SMS_FrameEncode(&in, wire);
    wire[0] = (uint8_t)((SMS_TYPE_DATA << 6) | (1u << 4) | (2u << 2));  // frag 2 of 1

    const uint16_t crc = SMS_Crc(wire, SMS_FRAME_BYTES - 2);

    wire[SMS_FRAME_BYTES - 2] = (uint8_t)(crc & 0xFFu);
    wire[SMS_FRAME_BYTES - 1] = (uint8_t)(crc >> 8);

    CHECK(!SMS_FrameDecode(wire, &out), "accepted a fragment index past the count");
}

// Drives a whole exchange, dropping the frames named by a callback, and reports
// how many transmissions it took. Returns false if the message never arrived.
static bool Exchange(const char *text, bool (*drop)(int n), int *sent_out,
                     char *received, uint32_t received_max)
{
    SMS_Tx_t    tx;
    SMS_Rx_t    rx;
    SMS_Frame_t data, ack, got;
    int         sent = 0;

    SMS_RxInit(&rx);

    if (!SMS_TxBegin(&tx, BOB, 0x11, text))
        return false;

    for (int round = 0; round < SMS_MAX_ATTEMPTS + 2 && !SMS_TxComplete(&tx); round++) {
        while (SMS_TxNext(&tx, ALICE, &data)) {
            const int n = sent++;

            // Mark this fragment sent for this round by clearing it locally, so
            // TxNext moves on; a missing ACK puts it back below.
            const uint8_t before = tx.pending;

            if (!drop(n) && Deliver(&data, &got, -1)) {
                if (SMS_RxOnData(&rx, &got, BOB, &ack)) {
                    // complete; the ACK still has to get back
                }

                SMS_Frame_t ackgot;

                if (!drop(sent++) && Deliver(&ack, &ackgot, -1))
                    SMS_TxOnAck(&tx, &ackgot);
            }

            if (tx.pending == before) {
                // Nothing was acknowledged for this fragment, so stop walking the
                // same one forever and let the next round retry it.
                break;
            }
        }

        SMS_TxRetry(&tx);

        if (SMS_TxFailed(&tx))
            break;
    }

    if (sent_out != NULL)
        *sent_out = sent;

    if (received != NULL && received_max > 0) {
        uint32_t n = 0;

        while (rx.text[n] != '\0' && n + 1 < received_max) {
            received[n] = rx.text[n];
            n++;
        }

        received[n] = '\0';
    }

    return SMS_TxComplete(&tx);
}

static bool DropNothing(int n) { (void)n; return false; }
static bool DropFirst(int n)   { return n == 0; }
static bool DropAck(int n)     { return n == 1; }   // the first ACK

static void TestCleanExchange(void)
{
    char text[SMS_MAX_CHARS + 1];
    int  sent = 0;

    CHECK(Exchange("hello over the air", DropNothing, &sent, text, sizeof text),
          "clean exchange did not complete");
    CHECK(strcmp(text, "hello over the air") == 0, "received \"%s\"", text);
    CHECK(sent == 2, "took %d transmissions for one fragment, want 2", sent);
}

static void TestLostFragmentIsRepaired(void)
{
    char text[SMS_MAX_CHARS + 1];

    CHECK(Exchange("dropped on the way", DropFirst, NULL, text, sizeof text),
          "a lost fragment was not repaired");
    CHECK(strcmp(text, "dropped on the way") == 0, "received \"%s\"", text);
}

static void TestLostAckIsRepaired(void)
{
    /* The case that deadlocks a naive implementation. From the sender's side a
     * lost ACK is indistinguishable from a lost fragment, so it resends; the
     * receiver already has that fragment and must acknowledge it again rather
     * than ignoring it as a duplicate. */
    char text[SMS_MAX_CHARS + 1];

    CHECK(Exchange("the ack went missing", DropAck, NULL, text, sizeof text),
          "a lost ACK was not recovered from");
    CHECK(strcmp(text, "the ack went missing") == 0, "received \"%s\"", text);
}

static void TestMultipleFragments(void)
{
    char long_text[SMS_MAX_CHARS + 1];
    char got[SMS_MAX_CHARS + 1];

    for (uint32_t i = 0; i < SMS_MAX_CHARS; i++)
        long_text[i] = (char)('a' + (i % 26));

    long_text[SMS_MAX_CHARS] = '\0';

    int sent = 0;

    CHECK(Exchange(long_text, DropNothing, &sent, got, sizeof got),
          "the longest message did not complete");
    CHECK(strcmp(got, long_text) == 0, "reassembly differs");
    CHECK(sent == SMS_MAX_FRAGS * 2, "took %d transmissions, want %d",
          sent, SMS_MAX_FRAGS * 2);
}

static void TestStaleAckIgnored(void)
{
    /* An acknowledgement for the previous message arriving late must not mark the
     * current one delivered - that would report a message sent that never was. */
    SMS_Tx_t    tx;
    SMS_Frame_t stale;

    CHECK(SMS_TxBegin(&tx, BOB, 0x20, "current message"), "begin failed");

    memset(&stale, 0, sizeof stale);

    stale.type       = SMS_TYPE_ACK;
    stale.frags      = 1;
    stale.src        = BOB;
    stale.dst        = ALICE;
    stale.msg_id     = 0x1F;         // the one before
    stale.len        = 1;
    stale.payload[0] = 0x07;         // claims everything

    SMS_TxOnAck(&tx, &stale);
    CHECK(!SMS_TxComplete(&tx), "a stale ACK completed the message");

    // And one from the wrong station, with the right message id.
    stale.msg_id = 0x20;
    stale.src    = 0x9999;

    SMS_TxOnAck(&tx, &stale);
    CHECK(!SMS_TxComplete(&tx), "an ACK from the wrong station completed it");

    // The genuine one works.
    stale.src = BOB;
    SMS_TxOnAck(&tx, &stale);
    CHECK(SMS_TxComplete(&tx), "the real ACK did not complete it");
}

static void TestGivesUpRatherThanLoops(void)
{
    // A link that never works must report failure, not retry forever.
    SMS_Tx_t tx;

    CHECK(SMS_TxBegin(&tx, BOB, 1, "into the void"), "begin failed");

    for (int i = 0; i < SMS_MAX_ATTEMPTS; i++) {
        CHECK(!SMS_TxFailed(&tx), "gave up after %d of %d attempts",
              i, SMS_MAX_ATTEMPTS);
        SMS_TxRetry(&tx);
    }

    CHECK(SMS_TxFailed(&tx), "never gave up");
    CHECK(!SMS_TxComplete(&tx), "reported complete having delivered nothing");
}

static void TestAddressing(void)
{
    SMS_Rx_t    rx;
    SMS_Frame_t data, ack;

    SMS_RxInit(&rx);
    memset(&data, 0, sizeof data);

    data.type = SMS_TYPE_DATA; data.frags = 1; data.src = ALICE;
    data.len = 3; memcpy(data.payload, "hey", 3);

    data.dst = 0x9999;   // somebody else
    CHECK(!SMS_RxOnData(&rx, &data, BOB, &ack), "accepted a frame for another station");

    data.dst = BOB;
    CHECK(SMS_RxOnData(&rx, &data, BOB, &ack), "rejected a frame addressed to us");
    CHECK(ack.dst == ALICE && ack.src == BOB, "ACK addressed wrongly");
    CHECK(ack.payload[0] == 1u, "ACK bitmap %02x want 01", ack.payload[0]);

    SMS_RxInit(&rx);
    data.dst = SMS_ADDR_BROADCAST;
    CHECK(SMS_RxOnData(&rx, &data, BOB, &ack), "rejected a broadcast");
}

static void TestLengthLimits(void)
{
    SMS_Tx_t tx;
    char     toolong[SMS_MAX_CHARS + 2];

    memset(toolong, 'x', sizeof toolong - 1);
    toolong[sizeof toolong - 1] = '\0';

    CHECK(!SMS_TxBegin(&tx, BOB, 1, ""), "accepted an empty message");
    CHECK(!SMS_TxBegin(&tx, BOB, 1, toolong), "accepted an over-long message");

    toolong[SMS_MAX_CHARS] = '\0';
    CHECK(SMS_TxBegin(&tx, BOB, 1, toolong), "rejected a maximum length message");
    CHECK(tx.frags == SMS_MAX_FRAGS, "%u fragments for the maximum, want %u",
          tx.frags, SMS_MAX_FRAGS);
}

static void TestNoiseDecodesNothing(void)
{
    uint32_t seed = 99u, accepted = 0;
    const uint32_t trials = 20000;

    for (uint32_t t = 0; t < trials; t++) {
        uint8_t     wire[SMS_FRAME_BYTES];
        SMS_Frame_t out;

        for (uint32_t i = 0; i < sizeof wire; i++) {
            seed    = seed * 1103515245u + 12345u;
            wire[i] = (uint8_t)(seed >> 16);
        }

        if (SMS_FrameDecode(wire, &out))
            accepted++;
    }

    /* A 16 bit CRC lets one random frame in 65536 through, and the range checks
     * throw out most of those, so zero is expected over this many trials but a
     * handful would not be alarming. Anything systematic would show as dozens. */
    CHECK(accepted <= 2, "%u of %u noise frames accepted", accepted, trials);
}

int main(void)
{
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "CRC check value",              TestCrcCheckValue },
        { "frame round trip",             TestFrameRoundTrip },
        { "no long runs on air",          TestNoLongRunsOnAir },
        { "every single bit error caught", TestEverySingleBitErrorIsCaught },
        { "out of range fields rejected", TestOutOfRangeFieldsRejected },
        { "clean exchange",               TestCleanExchange },
        { "lost fragment repaired",       TestLostFragmentIsRepaired },
        { "lost ACK repaired",            TestLostAckIsRepaired },
        { "multiple fragments",           TestMultipleFragments },
        { "stale ACK ignored",            TestStaleAckIgnored },
        { "gives up rather than loops",   TestGivesUpRatherThanLoops },
        { "addressing",                   TestAddressing },
        { "length limits",                TestLengthLimits },
        { "noise decodes nothing",        TestNoiseDecodesNothing },
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = failures;

        tests[i].fn();
        printf("%-34s %s\n", tests[i].name, (failures == before) ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
