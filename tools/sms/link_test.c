/* Host test for the link state machine in app/sms_link.c.
 *
 * app/sms.c's protocol is covered by sms_test.c; this covers what happens once a
 * radio is involved - the timeout clock, the retries, and the turnaround. Those
 * are the parts that cost bench time to debug, so they are worth simulating.
 *
 * sms_link.c keeps its state in file-scope statics, so two stations cannot be
 * instantiated in one process. They do not need to be: the stub plays the far end.
 * When the link transmits, the stub decodes the frame, updates a peer's
 * reassembly state, and queues whatever the peer would send back - or drops it,
 * on a script. That exercises one station's state machine completely.
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/sms_link.h"
#include "driver/bk4819-hwfsk.h"

/* The stubs need the real signatures, or a mismatch would link cleanly and
 * misbehave. These headers are host-includable; tools/pocsag builds
 * driver/bk4819-hwfsk.c natively against the same ones. */
#include "driver/bk4819.h"
#include "radio.h"
#include "app/sms_crypto.h"

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

#define US    0x1111u
#define THEM  0x2222u

uint32_t gHostChipId[4] = { 0x12345678u, 0x9ABCDEF0u, 0x0F0F0F0Fu, 0xA5A5A5A5u };

uint32_t SMS_CryptoHostSeed(void) { return 0x5A5A5A5Au; }

// --- the simulated far end -------------------------------------------------

static struct {
    uint32_t    tx_count;           // frames the link under test has sent
    SMS_Frame_t last_tx;
    bool        last_tx_valid;

    uint8_t     reply[SMS_FRAME_BYTES];
    uint8_t     reply_pos;
    bool        reply_queued;

    SMS_Rx_t    peer;               // the far end's reassembly
    bool        peer_answers;       // false models a station that never replies
    bool      (*drop_reply)(uint32_t n);   // drop the reply to the nth frame
    uint32_t    replies_dropped;
    bool        peer_got_message;
    char        peer_text[SMS_MAX_CHARS + 1];
} air;

static bool DropNone(uint32_t n) { (void)n; return false; }

static void AirReset(void)
{
    memset(&air, 0, sizeof air);
    SMS_RxInit(&air.peer);
    air.peer_answers = true;
    air.drop_reply   = DropNone;
}

/* Queues a frame as if the far end had transmitted it, sealing it the way a real
 * peer would. Without this the link under test would open a plaintext payload and
 * reject it, so the stub has to be a full participant rather than a wire tap. */
static void AirQueue(const SMS_Frame_t *frame)
{
    SMS_Frame_t sealed = *frame;

    sealed.len = SMS_Crypto_Seal(sealed.payload, sealed.len,
                                 sealed.src, sealed.msg_id, sealed.frag);

    SMS_FrameEncode(&sealed, air.reply);
    air.reply_pos    = 0;
    air.reply_queued = true;
}

// --- chip stubs ------------------------------------------------------------

uint16_t BK4819_HwFskBaudWord(uint32_t baud) { return (uint16_t)baud; }

BK4819_HwFskStatus_t BK4819_HwFskCheck(const BK4819_HwFskConfig_t *cfg, uint32_t len)
{
    (void)cfg; (void)len;
    return BK4819_HWFSK_OK;
}

BK4819_HwFskStatus_t BK4819_HwFskSetup(const BK4819_HwFskConfig_t *cfg)
{
    (void)cfg;
    return BK4819_HWFSK_OK;
}

/* The link transmitting is where the far end gets to act. A DATA frame for the
 * peer is reassembled and acknowledged; an ACK is simply noted, since the peer has
 * nothing to say in reply to one. */
BK4819_HwFskStatus_t BK4819_HwFskSend(const BK4819_HwFskConfig_t *cfg,
                                      const uint8_t *payload, uint32_t len)
{
    (void)cfg;

    const uint32_t n = air.tx_count++;

    air.last_tx_valid = (len == SMS_FRAME_BYTES) &&
                        SMS_FrameDecode(payload, &air.last_tx) &&
                        SMS_Crypto_Open(air.last_tx.payload, &air.last_tx.len,
                                        air.last_tx.src, air.last_tx.msg_id,
                                        air.last_tx.frag);

    if (!air.last_tx_valid || !air.peer_answers)
        return BK4819_HWFSK_OK;

    if (air.last_tx.type != SMS_TYPE_DATA)
        return BK4819_HWFSK_OK;

    SMS_Frame_t ack;

    if (SMS_RxOnData(&air.peer, &air.last_tx, THEM, &ack)) {
        air.peer_got_message = true;
        memcpy(air.peer_text, air.peer.text, sizeof air.peer_text);
    }

    if (air.drop_reply(n))
        air.replies_dropped++;
    else
        AirQueue(&ack);

    return BK4819_HWFSK_OK;
}

void BK4819_HwFskStop(void) { }

BK4819_HwFskStatus_t BK4819_HwFskRxCheck(const BK4819_HwFskConfig_t *cfg, uint32_t n)
{
    (void)cfg; (void)n;
    return BK4819_HWFSK_OK;
}

BK4819_HwFskStatus_t BK4819_HwFskRxSetup(const BK4819_HwFskConfig_t *cfg, uint32_t n)
{
    (void)cfg; (void)n;
    return BK4819_HWFSK_OK;
}

BK4819_HwFskStatus_t BK4819_HwFskRxSetupMode(const BK4819_HwFskConfig_t *cfg,
                                             uint32_t n, uint16_t reg58)
{
    (void)cfg; (void)n; (void)reg58;
    return BK4819_HWFSK_OK;
}

void BK4819_HwFskRxRestart(const BK4819_HwFskConfig_t *cfg) { (void)cfg; }

// Hands over a queued frame a chunk at a time, raising finished on the last, which
// is how the real driver behaves for a frame that is a whole number of chunks.
uint8_t BK4819_HwFskRxPoll(uint8_t *dst, uint8_t max, BK4819_HwFskRxEvent_t *ev)
{
    memset(ev, 0, sizeof *ev);

    if (!air.reply_queued)
        return 0;

    uint8_t n = (uint8_t)(SMS_FRAME_BYTES - air.reply_pos);

    if (n > max)                        n = max;
    if (n > BK4819_HWFSK_RX_CHUNK)      n = BK4819_HWFSK_RX_CHUNK;

    memcpy(dst, air.reply + air.reply_pos, n);
    air.reply_pos = (uint8_t)(air.reply_pos + n);

    if (air.reply_pos >= SMS_FRAME_BYTES) {
        ev->finished     = true;
        air.reply_queued = false;
        air.reply_pos    = 0;
    }

    return n;
}

// Everything below is a no-op: the link only needs these to exist.
void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t pin, bool set) { (void)pin; (void)set; }
void BK4819_SetAF(BK4819_AF_Type_t af)                     { (void)af; }
void BK4819_WriteRegister(BK4819_REGISTER_t r, uint16_t v)  { (void)r; (void)v; }
uint16_t BK4819_ReadRegister(BK4819_REGISTER_t r)           { (void)r; return 0; }
void BK4819_ExitSubAu(void)                                 { }
void BK4819_SetFrequency(uint32_t f)                        { (void)f; }
void BK4819_PickRXFilterPathBasedOnFrequency(uint32_t f)     { (void)f; }
void BK4819_SetFilterBandwidth(const BK4819_FilterBandwidth_t bw, const bool w)
                                                            { (void)bw; (void)w; }
void BK4819_RX_TurnOn(void)                                 { }
void BK4819_PrepareTransmit(void)                           { }
void BK4819_SetupPowerAmplifier(const uint8_t b, const uint32_t f)
                                                            { (void)b; (void)f; }
void BK4819_EnterTxMute(void)                               { }
void BK4819_SetCompander(const unsigned int mode)            { (void)mode; }
void SYSTEM_DelayMs(uint32_t ms)                            { (void)ms; }
void RADIO_SetupRegisters(bool switchToForeground)           { (void)switchToForeground; }
int  TX_freq_check(const uint32_t f)                         { (void)f; return 0; }

VFO_Info_t *gCurrentVfo = NULL;

// --- helpers ---------------------------------------------------------------

// Runs the link for up to a bounded number of ticks, returning the first
// interesting event. Bounded so a stuck state machine fails rather than hangs.
static SMS_LinkEvent_t RunUntilEvent(uint32_t max_ticks, uint32_t *ticks_out)
{
    for (uint32_t t = 0; t < max_ticks; t++) {
        const SMS_LinkEvent_t ev = SMS_Link_Poll();

        if (ev != SMS_LINK_EVENT_NONE) {
            if (ticks_out != NULL)
                *ticks_out = t;

            return ev;
        }
    }

    if (ticks_out != NULL)
        *ticks_out = max_ticks;

    return SMS_LINK_EVENT_NONE;
}

static void Begin(void)
{
    AirReset();

    // Both ends share the passphrase, as two radios would.
    SMS_Crypto_SetKey("shared between the pair");

    SMS_Link_Start(14550000u, SMS_BAUD_DEFAULT, US);
}

// --- tests -----------------------------------------------------------------

static void TestAddressIsDerivedAndUsable(void)
{
    const uint16_t a = SMS_Link_DefaultAddress();

    CHECK(a != 0, "address is 0, which means unset");
    CHECK(a != SMS_ADDR_BROADCAST, "address is the broadcast address");

    // A different chip must give a different address, or every radio collides.
    uint32_t saved[4];

    memcpy(saved, gHostChipId, sizeof saved);
    gHostChipId[0] ^= 0xFFFFu;

    CHECK(SMS_Link_DefaultAddress() != a, "two chips derived the same address");

    memcpy(gHostChipId, saved, sizeof saved);

    // Unreadable registers must fall back rather than colliding silently.
    memset(gHostChipId, 0, sizeof gHostChipId);
    CHECK(SMS_Link_DefaultAddress() == 1, "no fallback for unreadable chip ID");

    memset(gHostChipId, 0xFF, sizeof gHostChipId);
    CHECK(SMS_Link_DefaultAddress() == 1, "no fallback for all-ones chip ID");

    memcpy(gHostChipId, saved, sizeof saved);
}

static void TestSingleFragmentDelivered(void)
{
    Begin();

    CHECK(SMS_Link_Send(THEM, "short one"), "send refused");
    CHECK(SMS_Link_Busy(), "not busy straight after sending");

    uint32_t ticks = 0;

    CHECK(RunUntilEvent(200, &ticks) == SMS_LINK_EVENT_SENT,
          "no SENT event within 200 ticks");
    CHECK(!SMS_Link_Busy(), "still busy after delivery");
    CHECK(air.tx_count == 1, "%u transmissions for one fragment, want 1",
          air.tx_count);
    CHECK(air.peer_got_message, "the far end did not get it");
    CHECK(strcmp(air.peer_text, "short one") == 0, "far end has \"%s\"",
          air.peer_text);
}

static void TestThreeFragmentsDelivered(void)
{
    char text[SMS_MAX_CHARS + 1];

    for (uint32_t i = 0; i < SMS_MAX_CHARS; i++)
        text[i] = (char)('a' + (i % 26));

    text[SMS_MAX_CHARS] = '\0';

    Begin();

    CHECK(SMS_Link_Send(THEM, text), "send refused");
    CHECK(RunUntilEvent(400, NULL) == SMS_LINK_EVENT_SENT, "no SENT event");
    CHECK(air.tx_count == SMS_MAX_FRAGS, "%u transmissions, want %u",
          air.tx_count, SMS_MAX_FRAGS);
    CHECK(strcmp(air.peer_text, text) == 0, "reassembly differs at the far end");
}

static bool DropFirstReply(uint32_t n) { return n == 0; }

static void TestLostAckIsRetried(void)
{
    /* The case that deadlocks a naive implementation, now with the timeout clock
     * in the loop: the sender must wait SMS_ACK_TICKS, resend, and the far end
     * must answer a duplicate rather than ignoring it. */
    Begin();
    air.drop_reply = DropFirstReply;

    CHECK(SMS_Link_Send(THEM, "the ack gets lost"), "send refused");

    uint32_t ticks = 0;

    CHECK(RunUntilEvent(400, &ticks) == SMS_LINK_EVENT_SENT,
          "a lost ACK was not recovered from");
    CHECK(air.tx_count == 2, "%u transmissions, want 2", air.tx_count);
    CHECK(ticks >= SMS_ACK_TICKS, "recovered in %u ticks, before the %u tick "
          "timeout could have expired", ticks, SMS_ACK_TICKS);
}

static void TestSilentPeerFails(void)
{
    // A link that never works must say so rather than retrying forever.
    Begin();
    air.peer_answers = false;

    CHECK(SMS_Link_Send(THEM, "into the void"), "send refused");

    uint32_t ticks = 0;
    const SMS_LinkEvent_t ev = RunUntilEvent(SMS_ACK_TICKS * (SMS_MAX_ATTEMPTS + 2),
                                             &ticks);

    CHECK(ev == SMS_LINK_EVENT_FAILED, "no FAILED event from a silent peer");
    CHECK(!SMS_Link_Busy(), "still busy after failing");
    CHECK(air.tx_count == SMS_MAX_ATTEMPTS, "%u transmissions, want %u",
          air.tx_count, SMS_MAX_ATTEMPTS);
}

static void TestIncomingMessageIsAnswered(void)
{
    Begin();

    SMS_Frame_t data;

    memset(&data, 0, sizeof data);
    data.type = SMS_TYPE_DATA;  data.frags = 1;  data.src = THEM;  data.dst = US;
    data.msg_id = 7;  data.len = 8;
    memcpy(data.payload, "incoming", 8);

    AirQueue(&data);

    CHECK(RunUntilEvent(200, NULL) == SMS_LINK_EVENT_MESSAGE, "no MESSAGE event");

    const SMS_Rx_t *in = SMS_Link_Inbox();

    CHECK(in != NULL, "inbox empty after a MESSAGE event");
    CHECK(in != NULL && strcmp(in->text, "incoming") == 0,
          "inbox has \"%s\"", (in != NULL) ? in->text : "");
    CHECK(in != NULL && in->src == THEM, "wrong sender recorded");

    // And it must have been acknowledged, or the sender will keep resending.
    CHECK(air.tx_count == 1, "sent %u frames in reply, want 1", air.tx_count);
    CHECK(air.last_tx_valid && air.last_tx.type == SMS_TYPE_ACK,
          "the reply was not an ACK");
    CHECK(air.last_tx.dst == THEM, "the ACK went to the wrong station");
}

static void TestFrameForAnotherStationIgnored(void)
{
    Begin();

    SMS_Frame_t data;

    memset(&data, 0, sizeof data);
    data.type = SMS_TYPE_DATA;  data.frags = 1;  data.src = THEM;
    data.dst  = 0x9999u;        // somebody else
    data.len  = 3;
    memcpy(data.payload, "not", 3);

    AirQueue(&data);

    CHECK(RunUntilEvent(50, NULL) == SMS_LINK_EVENT_NONE,
          "reported a message addressed to another station");
    CHECK(air.tx_count == 0, "answered a frame addressed elsewhere");
}

static void TestAnsweringDoesNotTimeOutOurOwnMessage(void)
{
    /* The interleaving case, and the reason the ACK window is restarted rather
     * than left running. Two stations messaging each other at once: answering an
     * incoming frame takes the radio away from our own message in flight, and if
     * that counted as a timeout the two would time each other out.
     *
     * Here our message's acknowledgement is delayed until after an incoming frame
     * has been dealt with, which is exactly the collision. */
    Begin();
    air.drop_reply = DropFirstReply;   // our first fragment goes unanswered

    CHECK(SMS_Link_Send(THEM, "ours"), "send refused");

    // Halfway through our timeout, a message arrives from the other station.
    for (uint32_t t = 0; t < SMS_ACK_TICKS / 2; t++)
        CHECK(SMS_Link_Poll() == SMS_LINK_EVENT_NONE, "unexpected early event");

    SMS_Frame_t theirs;

    memset(&theirs, 0, sizeof theirs);
    theirs.type = SMS_TYPE_DATA;  theirs.frags = 1;  theirs.src = THEM;
    theirs.dst  = US;  theirs.msg_id = 99;  theirs.len = 5;
    memcpy(theirs.payload, "yours", 5);

    AirQueue(&theirs);

    CHECK(RunUntilEvent(100, NULL) == SMS_LINK_EVENT_MESSAGE,
          "their message was not received while ours was in flight");

    // Ours must still complete, and must not have been abandoned.
    CHECK(SMS_Link_Busy(), "our message was dropped when theirs arrived");
    CHECK(RunUntilEvent(400, NULL) == SMS_LINK_EVENT_SENT,
          "our message never completed after answering theirs");
}

static void TestSendRefusedWhileBusy(void)
{
    Begin();
    air.peer_answers = false;

    CHECK(SMS_Link_Send(THEM, "first"), "first send refused");
    CHECK(!SMS_Link_Send(THEM, "second"), "accepted a second message while busy");
}

static void TestProgressReported(void)
{
    char text[SMS_MAX_CHARS + 1];

    for (uint32_t i = 0; i < SMS_MAX_CHARS; i++)
        text[i] = 'x';

    text[SMS_MAX_CHARS] = '\0';

    Begin();

    CHECK(SMS_Link_Send(THEM, text), "send refused");

    uint8_t done = 9, total = 9;

    SMS_Link_Progress(&done, &total, NULL);
    CHECK(total == SMS_MAX_FRAGS, "total %u want %u", total, SMS_MAX_FRAGS);

    CHECK(RunUntilEvent(400, NULL) == SMS_LINK_EVENT_SENT, "no SENT event");

    SMS_Link_Progress(&done, &total, NULL);
    CHECK(done == SMS_MAX_FRAGS, "done %u of %u after delivery", done, total);
}

int main(void)
{
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "address derived and usable",     TestAddressIsDerivedAndUsable },
        { "single fragment delivered",      TestSingleFragmentDelivered },
        { "three fragments delivered",      TestThreeFragmentsDelivered },
        { "lost ACK is retried",            TestLostAckIsRetried },
        { "silent peer fails",              TestSilentPeerFails },
        { "incoming message is answered",   TestIncomingMessageIsAnswered },
        { "frame for another station",      TestFrameForAnotherStationIgnored },
        { "answering does not time us out", TestAnsweringDoesNotTimeOutOurOwnMessage },
        { "send refused while busy",        TestSendRefusedWhileBusy },
        { "progress reported",              TestProgressReported },
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = failures;

        tests[i].fn();
        printf("%-36s %s\n", tests[i].name, (failures == before) ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
