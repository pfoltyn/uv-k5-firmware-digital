/* The radio side of SMS. See sms_link.h. */

#include <string.h>

#include "app/sms_link.h"

#ifdef ENABLE_SMS_CRYPTO
    #include "app/sms_crypto.h"
#endif

#ifdef SMS_LINK_HOST_TEST
    /* The host test cannot read the chip's ID registers, and syscon.h dereferences
     * absolute addresses, so the values come from the test instead. This is the
     * only thing in the file that needs a seam: everything else the test stubs at
     * the function level. */
    extern uint32_t gHostChipId[4];
    #define SYSCON_CHIP_ID0 gHostChipId[0]
    #define SYSCON_CHIP_ID1 gHostChipId[1]
    #define SYSCON_CHIP_ID2 gHostChipId[2]
    #define SYSCON_CHIP_ID3 gHostChipId[3]

    /* AUDIO_AudioPathOff is a static inline in audio.h that writes GPIO registers
     * directly, so it cannot be replaced by a stub at link time - the inline wins
     * and dereferences an absolute address. It has to be skipped here instead. */
    static void AUDIO_AudioPathOff(void) { }
#else
    #include "bsp/dp32g030/syscon.h"
    #include "audio.h"
#endif
#include "driver/bk4819-hwfsk.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "frequencies.h"
#include "radio.h"

// POCSAG's measured values; that path is proven on this hardware.
#define TX_DEVIATION      0x4D0
#define TX_GAIN           96
#define PREAMBLE_BYTES    8

enum state_t {
    ST_STOPPED = 0,
    ST_LISTEN,        // idle, receiver armed for incoming messages
    ST_WAIT_ACK       // a fragment has gone out, waiting to be acknowledged
};

static BK4819_HwFskConfig_t gCfg;
static uint32_t             gFrequency;
static uint16_t             gSelf;
static uint8_t              gState;
static uint16_t             gTicks;        // in ST_WAIT_ACK
static uint8_t              gMsgId;

static SMS_Tx_t             gTx;
static bool                 gSending;
static SMS_Rx_t             gRx;
static bool                 gHaveInbox;

static uint8_t              gWire[SMS_FRAME_BYTES];
static SMS_LinkStats_t      gStats;

uint16_t SMS_Link_DefaultAddress(void)
{
    const uint32_t id[4] = {
        SYSCON_CHIP_ID0, SYSCON_CHIP_ID1, SYSCON_CHIP_ID2, SYSCON_CHIP_ID3
    };

    // All zeroes or all ones means the registers are not what we think they are,
    // and every radio would answer to the same address. The same check POCSAG
    // makes for the same reason.
    bool useful = false;

    for (uint8_t i = 0; i < 4; i++)
        if (id[i] != 0 && id[i] != 0xFFFFFFFFu)
            useful = true;

    if (!useful)
        return 0x0001u;

    // FNV-1a, folded to 16 bits.
    uint32_t h = 2166136261u;

    for (uint8_t i = 0; i < 4; i++)
        for (uint8_t b = 0; b < 4; b++) {
            h ^= (id[i] >> (b * 8)) & 0xFFu;
            h *= 16777619u;
        }

    uint16_t addr = (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);

    // 0 is unset and 0xFFFF is the broadcast address; neither may be a station.
    if (addr == 0 || addr == SMS_ADDR_BROADCAST)
        addr = 0x0001u;

    return addr;
}

static void Config(uint32_t baud)
{
    gCfg.baud           = baud;
    gCfg.preamble_bytes = PREAMBLE_BYTES;
    gCfg.sync_bytes     = 4;
    gCfg.sync01         = (uint16_t)(SMS_SYNC_WORD >> 16);
    gCfg.sync23         = (uint16_t)(SMS_SYNC_WORD & 0xFFFFu);
    gCfg.deviation      = TX_DEVIATION;
    gCfg.gain           = TX_GAIN;
    gCfg.crc            = false;   // the frame carries its own
    gCfg.scramble       = false;

    /* Left false here and set per direction below. See the note on Invert().
     *
     * This chip hands back inverted data on receive, which the POCSAG work
     * established and the direct FSK bring-up confirmed unmissably: 64 bytes of 0x01
     * came back as 64 bytes of 0xFE. The correction belongs on receive only. */
    gCfg.invert         = false;
}

/* The driver has one invert flag and applies it to REG_59<9> for transmit and
 * REG_59<10> for receive, depending on which setup function is called. Setting it
 * once in the config therefore inverts BOTH directions, which is not what the chip
 * needs: it inverts on receive by itself, so only the receive correction is wanted.
 *
 * With it set for both, a transmission went out inverted, the chip inverted it again
 * on the way in, and the receive correction inverted it a third time - so every
 * frame arrived inverted, every CRC failed, and neither direction worked. Setting it
 * per call is the whole fix.
 */
static void Invert(bool on)
{
    gCfg.invert = on;
}

/* Turns the receiver on. SetFrequency writes REG_38 and REG_39 but the
 * synthesiser only takes them on a REG_30 re-trigger, which is what RX_TurnOn
 * does, so the order matters - this cost a bench session on the POCSAG path. The
 * audio stays muted because the modem taps the discriminator ahead of it. */
static void RxOn(void)
{
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

    BK4819_SetAF(BK4819_AF_MUTE);
    AUDIO_AudioPathOff();

    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_ExitSubAu();

    BK4819_SetFrequency(gFrequency);
    BK4819_PickRXFilterPathBasedOnFrequency(gFrequency);

    /* Narrow, which the APRS probe measured as dramatically better than wide for
     * direct FSK.
     *
     * Wide was tried here on the strength of a Carson's rule calculation - POCSAG's
     * 4.5kHz deviation at 2400 baud occupies about 13.8kHz against narrow's 12.5 - and
     * measured worse: none of four frames decoded where narrow had managed one. The
     * calculation was not the problem the frames had; 234 bits of zero padding was.
     * Reverted, and the arithmetic left here as a warning rather than a reason. */
    BK4819_SetFilterBandwidth(BK4819_FILTER_BW_NARROW, false);

    BK4819_RX_TurnOn();

    Invert(true);
    BK4819_HwFskRxSetup(&gCfg, SMS_FRAME_BYTES);
}

/* Keying, from the POCSAG transmit path. Two details there were found the hard
 * way: clearing RX_ENABLE is what routes the PA to the antenna, and REG_70 must
 * be programmed after keying rather than before, because keying clears it. */
static void TxOn(uint8_t pa_bias)
{
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_ExitSubAu();
    BK4819_SetCompander(0);

    BK4819_SetFrequency(gFrequency);

    BK4819_PrepareTransmit();
    SYSTEM_DelayMs(10);

    BK4819_PickRXFilterPathBasedOnFrequency(gFrequency);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    SYSTEM_DelayMs(5);

    BK4819_SetupPowerAmplifier(pa_bias, gFrequency);
    SYSTEM_DelayMs(10);
}

static void TxOff(void)
{
    BK4819_HwFskStop();
    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
}

/* Sends one frame and returns to listening.
 *
 * Blocking, for the 253ms a 64 byte frame takes at 2400 baud. The POCSAG transmit
 * path blocks the same way; the alternative is a state machine around the modem's
 * TX-finished interrupt, which buys nothing while there is nothing else for the
 * radio to do meanwhile.
 */
static void SendFrame(const SMS_Frame_t *frame)
{
    const uint8_t bias = (gCurrentVfo != NULL) ? gCurrentVfo->TXP_CalculatedSetting : 0;

    SMS_Frame_t sealed = *frame;

#ifdef ENABLE_SMS_CRYPTO
    /* Sealed here rather than in the protocol layer because this is the boundary
     * where a frame becomes radio. Applied to acknowledgements as well as data: an
     * unauthenticated ACK would let anyone forge a delivery confirmation. */
    sealed.len = SMS_Crypto_Seal(sealed.payload, sealed.len,
                                 sealed.src, sealed.msg_id, sealed.frag);
#endif

    SMS_FrameEncode(&sealed, gWire);

    TxOn(bias);

    Invert(false);

    if (BK4819_HwFskSetup(&gCfg) == BK4819_HWFSK_OK) {
        BK4819_HwFskSend(&gCfg, gWire, SMS_FRAME_BYTES);
        gStats.sent++;
    }

    TxOff();
    RxOn();
}

void SMS_Link_Start(uint32_t frequency, uint32_t baud, uint16_t self)
{
    gFrequency = frequency;
    gSelf      = self;
    gMsgId     = 0;
    gSending   = false;
    gHaveInbox = false;
    gTicks     = 0;

    Config(baud);
    SMS_RxInit(&gRx);
    memset(&gStats, 0, sizeof gStats);

    RxOn();

    gState = ST_LISTEN;
}

void SMS_Link_GetStats(SMS_LinkStats_t *stats)
{
    if (stats != NULL)
        *stats = gStats;
}

void SMS_Link_Stop(void)
{
    if (gState == ST_STOPPED)
        return;

    BK4819_HwFskStop();
    gState = ST_STOPPED;

    RADIO_SetupRegisters(true);
}

bool SMS_Link_Send(uint16_t dst, const char *text)
{
    if (gState == ST_STOPPED || gSending)
        return false;

    if (TX_freq_check(gFrequency) != 0)
        return false;

    if (!SMS_TxBegin(&gTx, dst, gMsgId, text))
        return false;

    gMsgId++;
    gSending = true;
    gTicks   = 0;

    SMS_Frame_t frame;

    if (!SMS_TxNext(&gTx, gSelf, &frame)) {
        gSending = false;
        return false;
    }

    SendFrame(&frame);
    gState = ST_WAIT_ACK;

    return true;
}

bool SMS_Link_Busy(void)
{
    return gSending;
}

const SMS_Rx_t *SMS_Link_Inbox(void)
{
    return gHaveInbox ? &gRx : NULL;
}

void SMS_Link_Progress(uint8_t *done, uint8_t *total, uint8_t *attempt)
{
    uint8_t acked = 0;

    for (uint8_t f = 0; f < gTx.frags; f++)
        if ((gTx.pending & (1u << f)) == 0)
            acked++;

    if (done    != NULL) *done    = acked;
    if (total   != NULL) *total   = gTx.frags;
    if (attempt != NULL) *attempt = gTx.attempt;
}

// Reads one frame out of the FIFO if the chip has finished one. The frame is a
// whole number of FIFO chunks, so RX-finished arrives with everything delivered
// and there is no stranded tail to work around.
static bool TakeFrame(SMS_Frame_t *frame)
{
    BK4819_HwFskRxEvent_t ev;
    uint8_t               chunk[BK4819_HWFSK_RX_CHUNK];
    static uint8_t        buf[SMS_FRAME_BYTES];
    static uint8_t        len;

    const uint8_t n = BK4819_HwFskRxPoll(chunk, sizeof chunk, &ev);

    if (n > 0 && len + n <= sizeof buf) {
        memcpy(buf + len, chunk, n);
        len = (uint8_t)(len + n);
    }

    if (!ev.finished && len < sizeof buf)
        return false;

    bool ok = (len >= sizeof buf) && SMS_FrameDecode(buf, frame);

    if (len >= sizeof buf && !ok)
        gStats.crc_bad++;

#ifdef ENABLE_SMS_CRYPTO
    /* A frame whose tag fails is discarded silently rather than reported. The CRC
     * passing means it arrived intact, so a bad tag means somebody constructed it,
     * and there is nothing useful to show the operator. */
    if (ok) {
        ok = SMS_Crypto_Open(frame->payload, &frame->len,
                             frame->src, frame->msg_id, frame->frag);

        if (!ok)
            gStats.tag_bad++;
    }
#endif

    if (ok) {
        gStats.rx_ok++;
        gStats.last_dst = frame->dst;
        gStats.last_src = frame->src;
    }

    len = 0;
    BK4819_HwFskRxRestart(&gCfg);

    return ok;
}

SMS_LinkEvent_t SMS_Link_Poll(void)
{
    SMS_Frame_t frame;

    if (gState == ST_STOPPED)
        return SMS_LINK_EVENT_NONE;

    if (TakeFrame(&frame)) {
        if (frame.type == SMS_TYPE_ACK) {
            if (gSending) {
                gStats.acks++;
                SMS_TxOnAck(&gTx, &frame);

                if (SMS_TxComplete(&gTx)) {
                    gSending = false;
                    gState   = ST_LISTEN;

                    return SMS_LINK_EVENT_SENT;
                }

                // More to go: send the next outstanding fragment straight away.
                if (SMS_TxNext(&gTx, gSelf, &frame)) {
                    SendFrame(&frame);
                    gTicks = 0;
                    gState = ST_WAIT_ACK;
                }
            }

            return SMS_LINK_EVENT_NONE;
        }

        // DATA. Reassemble and answer, complete or not: a partial acknowledgement
        // is what tells the sender which fragments to repeat.
        SMS_Frame_t ack;

        const bool done = SMS_RxOnData(&gRx, &frame, gSelf, &ack);

        if (frame.dst != gSelf && frame.dst != SMS_ADDR_BROADCAST) {
            /* Perfectly good frame for somebody else. Counted rather than ignored,
             * because a sender's TO not matching this radio's OWN looks identical to
             * a dead link from both ends: the sender sees nothing come back and the
             * receiver sees frames arriving and does nothing with them. */
            gStats.addr_bad++;
        } else {
            SendFrame(&ack);

            /* Answering took the radio away from whatever we were doing. If a
             * message of our own is still in flight its ACK window has been eaten,
             * so restart it rather than counting that as a timeout. */
            if (gSending)
                gTicks = 0;
        }

        if (done) {
            gHaveInbox = true;
            return SMS_LINK_EVENT_MESSAGE;
        }

        return SMS_LINK_EVENT_NONE;
    }

    if (gState != ST_WAIT_ACK || !gSending)
        return SMS_LINK_EVENT_NONE;

    if (++gTicks < SMS_ACK_TICKS)
        return SMS_LINK_EVENT_NONE;

    // The acknowledgement did not come. Count the round and try again.
    gTicks = 0;
    SMS_TxRetry(&gTx);

    if (SMS_TxFailed(&gTx)) {
        gSending = false;
        gState   = ST_LISTEN;

        return SMS_LINK_EVENT_FAILED;
    }

    if (SMS_TxNext(&gTx, gSelf, &frame))
        SendFrame(&frame);

    return SMS_LINK_EVENT_NONE;
}
