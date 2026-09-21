/* APRS receive. See aprs_rx.h. */

#include <string.h>

#include "app/aprs_rx.h"

#include "audio.h"
#include "driver/bk4819-hwfsk.h"
#include "driver/bk4819.h"
#include "radio.h"

/* Long enough for the longest frame AX.25 allows, since anything shorter would
 * truncate a real station's transmission and take the FCS with it.
 *
 * Derived from AX25_MAX_AIR_BYTES and rounded up to a multiple of
 * BK4819_HWFSK_RX_CHUNK, which BK4819_HwFskRxCheck insists on because the
 * almost-full interrupt is the only measure of how much the FIFO holds.
 *
 * It is not free. At 1200 baud a byte is 6.7ms, so this is about three seconds
 * during which a capture armed by noise leaves the receiver deaf. In practice
 * captures end early on the carrier drop or the stall timer rather than running to
 * the programmed length, which is what already happens with every frame.
 */
#define RX_DATA_BYTES        (((AX25_MAX_AIR_BYTES + BK4819_HWFSK_RX_CHUNK - 1u)   \
                               / BK4819_HWFSK_RX_CHUNK) * BK4819_HWFSK_RX_CHUNK)

// Polls with nothing arriving before a part-received capture is given up on. At
// a 10ms timeslice this is about half a second, where a whole frame is 0.5s.
#define RX_STALL_POLLS       60u

/* Much shorter while probing, so a step that hears nothing moves on quickly. A
 * 32 byte capture takes 213ms at 1200 baud, so six steps come to under a second
 * either way, which one transmission from the far end covers. */
#define PROBE_STALL_POLLS    12u

// Raw REG_67 steps, which are 0.5dB each, so this is 10dB below the level at
// which sync was found. A sender that stops mid capture would otherwise leave
// the chip clocking receiver noise until the programmed length was reached.
#define RX_CARRIER_DROP      20u

static BK4819_HwFskConfig_t gCfg;
static uint32_t             gFrequency;
static uint16_t             gReg58;

static uint8_t              gAir[RX_DATA_BYTES];
static uint16_t             gLen;

static uint16_t             gStall;
static uint16_t             gSyncRssi;      // armed reference, 0 when not armed
static bool                 gInSync;
static bool                 gRunning;

static AX25_Decoded_t       gFrame;
static bool                 gHaveFrame;
static APRS_RxStats_t       gStats;

#define MODES 5u

static const uint16_t gModeReg58[MODES] = {
    BK4819_HWFSK_RX58_FFSK1800,
    BK4819_HWFSK_RX58_FFSK2400,
    BK4819_HWFSK_RX58_FSK12K,
    /* The wider front end, REG_58<3:1>=100, which the register list gives as the
     * bandwidth "for FSK 2.4K". Both of these ran on the 1.2K setting at first: 2400
     * worked anyway and 4800 would not lock at all, which is what sent us back to
     * the field. */
    BK4819_HWFSK_RX58_FSK24K,   // FSK2400
    BK4819_HWFSK_RX58_FSK24K    // FSK4800
};

// REG_72 follows the mode, because direct FSK at 2400 is the whole point of the
// fourth one.
static const uint16_t gModeBaud[MODES] = { 1200, 1200, 1200, 2400, 4800 };

/* Capture length per mode. The AFSK modes need room for the longest frame AX.25
 * allows; the direct FSK test needs only its 64 byte payload, and wants nothing
 * more, because every byte is 0.4ms at 2400 baud during which a capture armed by
 * noise leaves the receiver deaf. The first run showed exactly that: the byte count
 * climbing steadily with nothing decoding. */
static const uint16_t gModeData[MODES] = {
    RX_DATA_BYTES, RX_DATA_BYTES, RX_DATA_BYTES, 64u, 64u
};

/* REG_59<10>, invert the received data.
 *
 * This chip hands back inverted data, which the POCSAG receive work established and
 * had to correct in software. It never mattered for the AFSK modes here because NRZI
 * codes transitions, so inverting the whole stream cancels out and only the first
 * bit's reference level changes.
 *
 * Direct FSK has no NRZI, so it shows through unmissably: 64 bytes of 0x01 came back
 * as 64 bytes of 0xFE, every bit correct and every bit flipped. Corrected in the chip
 * rather than in software, since the register exists for it. */
static const bool gModeInvert[MODES] = { false, false, false, true, true };

/* Which IF filter bandwidth each demodulator wants, measured rather than reasoned.
 *
 * The probe swept all six combinations against a known flag stream sent at
 * 1200/2400 and scored how many of 32 bytes came back as 0x01. By name, not by
 * position, because the enum order has since been changed to put the winner first:
 *
 *     wide   FFSK2400 9    FFSK1800 0    FSK12K 0
 *     narrow FFSK2400 3    FFSK1800 32   FSK12K 0
 *
 * So FFSK1800 with the narrow filter is perfect and everything else close to
 * useless - including FFSK2400, which is the one that ought to have matched a
 * 2400Hz space tone and which three rounds of reasoning had settled on. No
 * mechanism is offered here because none was measured; the numbers are the
 * finding. */
static const bool gModeNarrow[MODES] = { true, false, false, true, true };

static bool    gProbe;
static uint8_t gProbeStep;
static uint8_t gMode;      // which of the four demodulator configurations

const char *APRS_RX_ModeName(APRS_RxMode_t mode)
{
    switch (mode) {
        case APRS_RX_FFSK2400: return "FFSK2400";
        case APRS_RX_FFSK1800: return "FFSK1800";
        case APRS_RX_FSK12K:   return "FSK 1.2K";
        case APRS_RX_FSK2400:  return "FSK 2400";
        case APRS_RX_FSK4800:  return "FSK 4800";
        case APRS_RX_PROBE:    return "PROBE";
        default:               return "?";
    }
}

static void Config(void)
{
    // Follows the mode, because direct FSK at 2400 is the point of the fourth one.
    gCfg.baud           = gModeBaud[gMode];

    // The minimum the chip allows. Every extra byte demanded is another byte of
    // alternating bits the far end has to send before it will listen, and
    // nothing in APRS sends any.
    gCfg.preamble_bytes = 1;

    if (gMode >= APRS_RX_FSK2400) {
        /* Direct FSK needs a strong sync word, and 0x0101 is a very weak one: two
         * ones in sixteen bits, and with no NRZI constraint the demodulator slices
         * raw noise into bits that match it often. The first run at 2400 baud
         * syncing repeatedly on an idle channel is what that looks like.
         *
         * POCSAG's sync codeword is borrowed for its autocorrelation, not its
         * meaning: 32 bits, balanced, and designed to be found in noise. */
        gCfg.sync_bytes = 4;
        gCfg.sync01     = 0x7CD2;
        gCfg.sync23     = 0x15D8;
    } else {
        /* Two flags, as NRZI puts them on air. See ax25.h.
         *
         * Two bytes rather than four, deliberately. A 32 bit sync word needs all 32
         * bits right, and an early bench run showed one match after several
         * transmissions, which is what a demodulator making occasional bit errors
         * looks like. Sixteen bits is four times less likely to be spoiled by a
         * single error, and here the FCS throws out what gets through. */
        gCfg.sync_bytes = 2;
        gCfg.sync01     = 0x0101;
        gCfg.sync23     = 0x0101;
    }

    gCfg.deviation      = 0;       // transmit only
    gCfg.gain           = 0;
    gCfg.crc            = false;   // AX.25 brings its own FCS
    gCfg.scramble       = false;

    /* For the AFSK modes this is a no-op either way: the chip matches the sync word
     * in both senses and says which it found, and NRZI codes transitions, so
     * inverting the whole stream changes only the very first decoded bit, which is
     * ahead of the flags the decoder searches for. TestInversionDoesNotMatter holds
     * it. Direct FSK has no such protection and needs the correction. */
    gCfg.invert         = gModeInvert[gMode];
}

/* SetFrequency writes REG_38 and REG_39 but the synthesiser only takes them on a
 * REG_30 re-trigger, which is what RX_TurnOn does, so the order matters here.
 * The audio is muted because the modem taps the discriminator ahead of the audio
 * path, so the speaker has nothing to contribute but the sound of 1200 baud
 * AFSK. Both points are from the POCSAG receive work. */
static void RxOn(void)
{
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

    BK4819_SetAF(BK4819_AF_MUTE);
    AUDIO_AudioPathOff();

    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_ExitSubAu();

    BK4819_SetFrequency(gFrequency);
    BK4819_PickRXFilterPathBasedOnFrequency(gFrequency);

    /* Wide, though APRS at 2.6kHz deviation with a 2200Hz tone needs only about
     * 10kHz by Carson's rule. Narrow would be the better choice for sensitivity
     * once this works at all; wide is more forgiving of frequency error between
     * two radios, and there are enough unknowns here already. */
    BK4819_SetFilterBandwidth(BK4819_FILTER_BW_WIDE, false);

    BK4819_RX_TurnOn();
}

/* Applies the current sweep step: demodulator and filter bandwidth together.
 *
 * The bandwidth is in here rather than fixed because it is a real candidate. Wide
 * is forgiving of frequency error between two radios; narrow passes less noise but
 * could also be distorting a 2200 or 2400Hz tone, and nothing so far rules either
 * way. */
static void ApplyStep(void)
{
    // The probe sweeps only the three 1200 baud demodulators; the fourth mode is a
    // different bit rate and is selected explicitly rather than swept.
    const uint8_t mode = gProbe ? (uint8_t)(gProbeStep % 3u) : gMode;

    gReg58 = gModeReg58[mode];

    /* While probing the step chooses; otherwise the table does, which is where the
     * probe's answer lives. */
    const bool narrow = gProbe ? (gProbeStep >= 3u) : gModeNarrow[mode];

    BK4819_SetFilterBandwidth(narrow ? BK4819_FILTER_BW_NARROW
                                     : BK4819_FILTER_BW_WIDE, false);

    BK4819_HwFskRxSetupMode(&gCfg, gProbe ? APRS_RX_PROBE_BYTES : gModeData[gMode],
                            gReg58);
}

static void Rearm(void)
{
    BK4819_HwFskRxRestart(&gCfg);

    gLen      = 0;
    gStall    = 0;
    gSyncRssi = 0;
    gInSync   = false;
}

void APRS_RX_Start(uint32_t frequency, APRS_RxMode_t mode)
{
    gFrequency = frequency;
    gProbe     = (mode == APRS_RX_PROBE);
    gProbeStep = gProbe ? 0u : (uint8_t)((mode < MODES) ? mode : 0u);
    gMode      = (uint8_t)(gProbe ? 0u : gProbeStep);

    Config();

    memset(&gStats, 0, sizeof gStats);
    memset(&gFrame, 0, sizeof gFrame);

    gHaveFrame = false;
    gRunning   = false;

    Rearm();
    RxOn();

    ApplyStep();

    gRunning = true;
}

void APRS_RX_Stop(void)
{
    if (!gRunning)
        return;

    BK4819_HwFskStop();
    gRunning = false;

    RADIO_SetupRegisters(true);
}

bool APRS_RX_InSync(void)
{
    return gInSync;
}

const AX25_Decoded_t *APRS_RX_Frame(void)
{
    return gHaveFrame ? &gFrame : NULL;
}

void APRS_RX_GetStats(APRS_RxStats_t *stats)
{
    if (stats != NULL)
        *stats = gStats;
}

/* Examines whatever was captured and starts listening again.
 *
 * The bytes out of the FIFO are the air stream: the chip does no NRZI decoding
 * and no de-stuffing, so they go to AX25_FromAir exactly as they arrived. Sync
 * landed on a flag boundary, so the NRZI level at the first bit is known, but
 * AX25_FromAir searches for flags itself and would find the frame regardless.
 */
static APRS_RxEvent_t Flush(void)
{
    APRS_RxEvent_t ev = APRS_RX_EVENT_NONE;

    if (gProbe) {
        uint8_t score = 0;

        for (uint16_t i = 0; i < gLen; i++)
            if (gAir[i] == 0x01u)
                score++;

        if (score > gStats.probe[gProbeStep])
            gStats.probe[gProbeStep] = score;

        if (gLen > 0) {
            gStats.flushes++;
            memcpy(gStats.first, gAir,
                   (gLen < sizeof gStats.first) ? gLen : sizeof gStats.first);
        }

        // Advance whether or not anything arrived, so a step that hears nothing
        // cannot stall the sweep.
        gProbeStep       = (uint8_t)((gProbeStep + 1u) % APRS_RX_PROBE_STEPS);
        gStats.probe_step = gProbeStep;

        gLen      = 0;
        gStall    = 0;
        gSyncRssi = 0;
        gInSync   = false;

        ApplyStep();

        return APRS_RX_EVENT_NONE;
    }

    if (gLen >= 18) {              // shortest possible frame, in bytes
        gStats.flushes++;

        /* Score the lead flags before anything else. They are the only part of
         * the stream whose value is known in advance, so they are the only
         * measurement of the demodulator that does not depend on the frame
         * decoding, which is exactly the situation to diagnose. */
        gStats.good_flags = 0;

        for (uint16_t i = 0; i < 12u && i < gLen; i++)
            if (gAir[i] == 0x01u)
                gStats.good_flags++;

        memcpy(gStats.first, gAir, (gLen < sizeof gStats.first) ? gLen
                                                               : sizeof gStats.first);

        if (AX25_FromAir(gAir, (uint32_t)gLen * 8u, AX25_NRZI_INITIAL, &gFrame)) {
            gStats.frames++;
            gHaveFrame = true;
            ev         = APRS_RX_EVENT_FRAME;
        }
    }

    Rearm();

    return ev;
}

APRS_RxEvent_t APRS_RX_Poll(void)
{
    BK4819_HwFskRxEvent_t  hw;
    uint8_t                chunk[BK4819_HWFSK_RX_CHUNK];
    APRS_RxEvent_t         ev = APRS_RX_EVENT_NONE;

    if (!gRunning)
        return APRS_RX_EVENT_NONE;

    const uint8_t n = BK4819_HwFskRxPoll(chunk, sizeof chunk, &hw);

    if (hw.sync) {
        const uint16_t rssi = BK4819_GetRSSI();

        gStats.syncs++;
        gStats.rssi = rssi;
        gSyncRssi   = (rssi == 0) ? 1u : rssi;   // 0 is the "not armed" marker
        gInSync     = true;
        gStall      = 0;
    }

    if (n > 0) {
        const uint16_t room = (uint16_t)(sizeof gAir - gLen);

        memcpy(gAir + gLen, chunk, (n < room) ? n : room);
        gLen        = (uint16_t)(gLen + ((n < room) ? n : room));
        gStats.bytes = (uint16_t)(gStats.bytes + n);
        gStall      = 0;
    } else if ((gLen > 0 || gProbe) &&
               ++gStall > (gProbe ? PROBE_STALL_POLLS : RX_STALL_POLLS)) {
        return Flush();
    }

    if (hw.finished || gLen >= gModeData[gMode])
        return Flush();

    /* Checked after the FIFO has been drained, not before: a packet already
     * waiting there would otherwise be thrown away along with the carrier. */
    if (gSyncRssi != 0) {
        const uint16_t rssi = BK4819_GetRSSI();

        if ((uint32_t)rssi + RX_CARRIER_DROP < gSyncRssi)
            return Flush();
    }

    return ev;
}
