/* APRS transmit. See aprs_tx.h. */

#include <stddef.h>   // NULL

#include "app/aprs.h"
#include "app/aprs_tx.h"

#include "driver/bk4819-afsk.h"
#include "driver/bk4819-hwfsk.h"
#include "driver/bk4819.h"
#include "driver/system.h"
#include "frequencies.h"
#include "radio.h"

static uint8_t gAprsAirBuffer[AX25_MAX_AIR_BYTES];

/* Keying. Lifted from the POCSAG transmit path, which took it from
 * RADIO_SetTxParameters, because that is the sequence known to work on this
 * hardware. Two details there were found the hard way and matter here too:
 *
 * The RX_ENABLE line is what routes the PA to the antenna. Leaving it set gave
 * no RF at all while the PA ran happily.
 *
 * PrepareTransmit rather than EnableTXLink, because the latter sets REG_30 but
 * skips REG_37 and REG_52.
 *
 * REG_70 is cleared here and programmed afterwards, never before: keying clears
 * it, so a tone set up first would be wiped and leave an unmodulated carrier. */
static void TxOn(uint32_t frequency, uint8_t pa_bias)
{
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

    BK4819_WriteRegister(BK4819_REG_70, 0);   // no tone generators yet
    BK4819_ExitSubAu();                       // no CTCSS/CDCSS
    BK4819_SetCompander(0);

    BK4819_SetFrequency(frequency);

    BK4819_PrepareTransmit();
    SYSTEM_DelayMs(10);

    BK4819_PickRXFilterPathBasedOnFrequency(frequency);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);
    SYSTEM_DelayMs(5);

    BK4819_SetupPowerAmplifier(pa_bias, frequency);
    SYSTEM_DelayMs(10);
}

static void TxOff(void)
{
    BK4819_AfskStop();

    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_30, 0);

    // put the transmit/receive switch back so the radio can hear again
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

    RADIO_SetupRegisters(true);
}

bool APRS_TX_ParsePath(const char *path, AX25_Frame_t *frame)
{
    char    field[AX25_CALL_CHARS + 4];
    uint8_t n = 0;

    if (frame == NULL)
        return false;

    frame->digis = 0;

    if (path == NULL || path[0] == '\0')
        return true;

    for (const char *p = path; ; p++) {
        if (*p != ',' && *p != '\0') {
            if (n + 1u >= sizeof field)
                return false;

            field[n++] = *p;
            continue;
        }

        field[n] = '\0';
        n        = 0;

        if (frame->digis >= AX25_MAX_DIGIS)
            return false;

        if (!AX25_ParseAddr(field, &frame->digi[frame->digis]))
            return false;

        frame->digis++;

        if (*p == '\0')
            return true;
    }
}

APRS_TxResult_t APRS_TX_SendFrame(uint32_t frequency, uint8_t pa_bias,
                                  const AX25_Frame_t *frame, uint8_t alt_bytes,
                                  APRS_TxStats_t *stats)
{
    uint8_t raw[AX25_MAX_FRAME];

    if (frame == NULL)
        return APRS_TX_ERR_ENCODE;

    if (!AX25_ValidCall(frame->dest.call) || !AX25_ValidCall(frame->src.call))
        return APRS_TX_ERR_CALL;

    const uint32_t len = AX25_BuildFrame(frame, raw, sizeof raw);

    if (len == 0)
        return APRS_TX_ERR_TOO_LONG;

    AX25_Bits_t bits;

    AX25_BitsInit(&bits, gAprsAirBuffer, sizeof gAprsAirBuffer);

    if (!AX25_ToAir(raw, len, alt_bytes, AX25_LEAD_FLAGS, &bits))
        return APRS_TX_ERR_TOO_LONG;

    if (TX_freq_check(frequency) != 0)
        return APRS_TX_ERR_TX_NOT_ALLOWED;

    TxOn(frequency, pa_bias);

    /* The tone starts before the bits do, so the modulator has settled by the
     * time anything meaningful is sent. The lead flags give the far end's clock
     * recovery something to lock to on top of this. */
    BK4819_AfskStart(APRS_TX_GAIN);
    SYSTEM_DelayMs(10);

    BK4819_AfskSendBits(gAprsAirBuffer, bits.length);

    TxOff();

    if (stats != NULL) {
        stats->frame_bytes = len;
        stats->air_bits    = bits.length;
        stats->ms          = (bits.length * 1000u) / BK4819_AFSK_BAUD;
    }

    return APRS_TX_OK;
}

APRS_TxResult_t APRS_TX_SendPayload(uint32_t frequency, uint8_t pa_bias,
                                    const AX25_Addr_t *src,
                                    const char *path,
                                    const char *info,
                                    uint8_t alt_bytes,
                                    APRS_TxStats_t *stats)
{
    AX25_Frame_t frame;

    if (src == NULL || info == NULL)
        return APRS_TX_ERR_ENCODE;

    if (!AX25_ParseAddr(APRS_TOCALL, &frame.dest))
        return APRS_TX_ERR_CALL;

    frame.src  = *src;
    frame.info = info;

    if (!APRS_TX_ParsePath(path, &frame))
        return APRS_TX_ERR_CALL;

    return APRS_TX_SendFrame(frequency, pa_bias, &frame, alt_bytes, stats);
}

/* Direct FSK through the hardware modem. Deviation and gain are POCSAG's measured
 * values rather than new guesses, since that path is proven on this hardware; the
 * sync word matches what the receive side already looks for. */
static APRS_TxResult_t SendFsk(uint32_t frequency, uint8_t pa_bias, uint32_t baud)
{
    BK4819_HwFskConfig_t cfg;

    cfg.baud           = baud;
    cfg.preamble_bytes = 8;
    // Must match the receive side: a strong 32 bit word, POCSAG's, borrowed for its
    // autocorrelation rather than its meaning. 0x0101 is matched by noise far too
    // readily once the demodulator is slicing a raw discriminator.
    cfg.sync_bytes     = 4;
    cfg.sync01         = 0x7CD2;
    cfg.sync23         = 0x15D8;
    cfg.deviation      = 0x4D0;   // POCSAG_TX_HW_DEVIATION
    cfg.gain           = 96;      // POCSAG_TX_HW_GAIN
    cfg.invert         = false;
    cfg.crc            = false;
    cfg.scramble       = false;

    // A known repeating byte, so the far end can score itself with no frame.
    for (uint32_t i = 0; i < 64u; i++)
        gAprsAirBuffer[i] = 0x01u;

    if (BK4819_HwFskCheck(&cfg, 64u) != BK4819_HWFSK_OK)
        return APRS_TX_ERR_MODEM;

    /* Keying first, then the modem: keying clears REG_70, whose low bits are the
     * FSK gain, so a modem programmed beforehand would be left transmitting an
     * unmodulated carrier. Found the hard way on the POCSAG path. */
    TxOn(frequency, pa_bias);

    if (BK4819_HwFskSetup(&cfg) != BK4819_HWFSK_OK) {
        BK4819_HwFskStop();
        TxOff();
        return APRS_TX_ERR_MODEM;
    }

    const BK4819_HwFskStatus_t st = BK4819_HwFskSend(&cfg, gAprsAirBuffer, 64u);

    BK4819_HwFskStop();
    TxOff();

    return (st == BK4819_HWFSK_OK) ? APRS_TX_OK : APRS_TX_ERR_MODEM;
}

APRS_TxResult_t APRS_TX_Tone(uint32_t frequency, uint8_t pa_bias,
                             APRS_ToneTest_t test, uint32_t ms, uint8_t gain)
{
    if (TX_freq_check(frequency) != 0)
        return APRS_TX_ERR_TX_NOT_ALLOWED;

    if (test == APRS_TONE_FSK2400 || test == APRS_TONE_FSK4800)
        return SendFsk(frequency, pa_bias,
                       (test == APRS_TONE_FSK2400) ? 2400u : 4800u);

    const uint32_t bits = (ms * BK4819_AFSK_BAUD) / 1000u;

    TxOn(frequency, pa_bias);

    BK4819_AfskStart(gain);
    SYSTEM_DelayMs(10);

    if (test == APRS_TONE_FLAGS) {
        for (uint32_t i = 0; i < sizeof gAprsAirBuffer; i++)
            gAprsAirBuffer[i] = (i < AX25_LEAD_ALT) ? 0xAAu : 0x01u;

        BK4819_AfskSendBits(gAprsAirBuffer, sizeof gAprsAirBuffer * 8u);
    } else if (test == APRS_TONE_ALTERNATING) {
        /* A 1010 pattern through the ordinary bit sender rather than a special
         * case in the driver, so what gets measured is the same code path a
         * frame uses, timing and register writes included.
         *
         * One call, not a loop: each call restarts the bit clock, and the buffer
         * already holds 1488 bits, which is 1.2 seconds and far more than a
         * spectrum measurement needs. */
        uint32_t bytes = (bits + 7u) / 8u;

        if (bytes > sizeof gAprsAirBuffer)
            bytes = sizeof gAprsAirBuffer;

        for (uint32_t i = 0; i < bytes; i++)
            gAprsAirBuffer[i] = 0xAAu;

        BK4819_AfskSendBits(gAprsAirBuffer, bytes * 8u);
    } else {
        BK4819_AfskSendTone(test == APRS_TONE_MARK, bits);
    }

    TxOff();

    return APRS_TX_OK;
}
