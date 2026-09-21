/* APRS screen. See aprs_ui.h. */

#include <string.h>

#include "app/aprs_ui.h"

#include "app/aprs.h"
#include "app/aprs_rx.h"
#include "driver/bk4819-afsk.h"
#include "app/aprs_tx.h"
#include "app/ax25.h"
#include "driver/backlight.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "misc.h"
#include "radio.h"
#include "ui/helper.h"

// The small font is 6px wide on a 1px pitch, so a row holds 18 characters
// starting at x=2: 2 + 17*7 + 6 is exactly 127, the last byte of the row.
#define COLS                18

// How long a tone test transmits. Fixed now that the row that used to set it
// belongs to the receiver; three seconds is ample for a spectrum measurement.
#define TONE_TEST_MS        3000u
#define REPEAT_DELAY_10MS   60
#define REPEAT_RATE_10MS    10

// Set at build time; see the Makefile. A measurement goes into a dummy load, so
// the default is deliberately not a real callsign.
#ifndef APRS_UI_CALL
    #define APRS_UI_CALL    "NOCALL"
#endif
#ifndef APRS_UI_SSID
    #define APRS_UI_SSID    7
#endif
#ifndef APRS_UI_PATH
    #define APRS_UI_PATH    "WIDE1-1,WIDE2-1"
#endif
#ifndef APRS_UI_GRID
    #define APRS_UI_GRID    "JO90xa"
#endif

enum {
    FIELD_FREQ = 0,
    FIELD_GAIN,      // REG_70<14:8>, which for this path is the deviation
    FIELD_SEND,      // what MENU does
    FIELD_RXMODE,    // which demodulator to listen with
    FIELD_SPACE,     // space tone: Bell 202, or what the chip can receive
    FIELD_COUNT
};

/* gFrameBuffer holds FRAME_LINES = 7 rows, drawn as hardware rows 1 to 7; the
 * hardware's row 0 is the separate gStatusLine buffer. So there is no row 7 and
 * writing one runs off the end of the array. The title and the result go on the
 * status line, which nothing else maintains while this screen owns the display. */
#define ROW_FIELD0          0
#define ROW_INFO            FIELD_COUNT
#define INFO_ROWS           2

#define STATUS_X_TITLE      2
#define STATUS_X_TEXT       46
#define STATUS_X_RX         104

enum {
    SEND_BEACON = 0,
    SEND_STATUS,
    SEND_STD,        // a beacon with no preamble: what a real station sends
    SEND_MARK,
    SEND_SPACE,
    SEND_ALT,
    SEND_FLAGS,
    SEND_FSK,        // direct 2-level FSK at 2400, bypassing the audio path
    SEND_FSK48,      // and at 4800, the last doubling REG_72 allows
    SEND_COUNT
};

static const uint32_t gFreqPreset[] = {
    APRS_TX_FREQ_EU, APRS_TX_FREQ_NA, APRS_TX_FREQ_ISS
};

static uint32_t gFreq   = APRS_TX_FREQ_EU;
static uint8_t  gGain   = APRS_TX_GAIN;
static uint8_t  gSend   = SEND_BEACON;
static uint8_t  gRxMode = APRS_RX_FFSK1800;

// Set when the receiver needs restarting: after a transmission, which takes the
// chip over completely, and after the mode is changed.
static bool     gRxRestart = true;

static uint8_t  gField;
static uint8_t  gEntryLen;         // digits typed into the current field
static bool     gRunning;
static bool     gRedraw;
static char     gStatus[COLS + 2];
static char     gPayload[AX25_MAX_INFO + 1];

static char *AppendStr(char *p, const char *s)
{
    while (*s != '\0')
        *p++ = *s++;

    *p = '\0';

    return p;
}

static char *AppendNum(char *p, uint32_t value, uint8_t digits)
{
    uint32_t scale = 1;

    for (uint8_t i = 1; i < digits; i++)
        scale *= 10u;

    for (; scale > 0; scale /= 10u)
        *p++ = (char)('0' + ((value / scale) % 10u));

    *p = '\0';

    return p;
}

static AX25_Addr_t OwnAddr(void)
{
    AX25_Addr_t a;

    memset(&a, 0, sizeof a);
    memcpy(a.call, APRS_UI_CALL, sizeof(APRS_UI_CALL) > sizeof a.call
                                 ? sizeof a.call - 1 : sizeof(APRS_UI_CALL) - 1);
    a.ssid = APRS_UI_SSID;

    return a;
}

// Builds whatever the SEND field currently selects, so the screen can show the
// operator the exact payload before it goes out.
static void BuildPayload(void)
{
    switch (gSend) {
        case SEND_BEACON:
            if (APRS_FormatPosition(gPayload, sizeof gPayload, APRS_UI_GRID,
                                    APRS_SYM_TABLE, APRS_SYM_CODE, "UV-K5") == 0)
                AppendStr(gPayload, "bad grid");
            break;

        case SEND_STATUS:
            APRS_FormatStatus(gPayload, sizeof gPayload, "UV-K5 APRS");
            break;

        case SEND_STD:
            APRS_FormatStatus(gPayload, sizeof gPayload, "no preamble");
            break;

        default:
            gPayload[0] = '\0';
            break;
    }
}

static void FormatValue(uint8_t field, char *out)
{
    static const char *send_name[SEND_COUNT] = {
        "POSITION", "STATUS", "NO PREAMBLE", "TONE MARK", "TONE SPACE",
        "TONE 1010", "FLAGS", "FSK 2400", "FSK 4800"
    };

    switch (field) {
        case FIELD_FREQ: {
            // stored in 10Hz units, shown as MHz to four places
            char *p = AppendNum(out, gFreq / 100000u, 3);

            *p++ = '.';
            AppendNum(p, gFreq % 100000u, 5);
            break;
        }

        case FIELD_GAIN:
            AppendNum(out, gGain, 3);
            break;

        case FIELD_SEND:
            AppendStr(out, send_name[gSend]);
            break;

        case FIELD_RXMODE:
            AppendStr(out, APRS_RX_ModeName((APRS_RxMode_t)gRxMode));
            break;

        case FIELD_SPACE: {
            char *p = AppendNum(out, BK4819_AfskSpaceTone(), 4);

            AppendStr(p, (BK4819_AfskSpaceTone() == BK4819_AFSK_SPACE_HZ)
                         ? " BELL" : " FFSK");
            break;
        }

        default:
            out[0] = '\0';
            break;
    }
}

static void Render(void)
{
    static const char *label[FIELD_COUNT] = { "FRQ", "DEV", "TX ", "RX ", "SPC" };
    char value[COLS + 2];
    char line[COLS + 2];

    UI_DisplayClear();
    memset(gStatusLine, 0, LCD_WIDTH);

    UI_PrintStringSmallBufferNormal("APRS", gStatusLine + STATUS_X_TITLE);

    if (gStatus[0] != '\0')
        UI_PrintStringSmallBufferNormal(gStatus, gStatusLine + STATUS_X_TEXT);

    // The receiver is on whenever this screen is, so say so, and say when a sync
    // has been matched rather than only that it is listening.
    UI_PrintStringSmallBufferNormal(APRS_RX_InSync() ? "RX*" : "RX",
                                    gStatusLine + STATUS_X_RX);

    for (uint8_t f = 0; f < FIELD_COUNT; f++) {
        char *p = line;

        FormatValue(f, value);

        *p++ = (f == gField) ? '>' : ' ';
        p    = AppendStr(p, label[f]);
        p    = AppendStr(p, " ");
        AppendStr(p, value);

        line[COLS] = '\0';

        UI_PrintStringSmallNormal(line, 2, 0, (uint8_t)(ROW_FIELD0 + f));
    }

    /* The last two rows belong to the receiver: who sent the last frame and what
     * it said, or the counters while nothing has arrived.
     *
     * The counters are what make a failure diagnosable rather than just silent. A
     * sync count that climbs while the frame count stays at zero says the
     * preamble detector and the sync word are right and something after them is
     * wrong, which is a different problem entirely from nothing happening. */
    const AX25_Decoded_t *rx = APRS_RX_Frame();

    if (rx != NULL) {
        char        addr[AX25_CALL_CHARS + 4];
        AX25_Addr_t src = rx->src;
        char       *p   = line;

        AX25_FormatAddr(&src, addr, sizeof addr);

        p = AppendStr(p, addr);
        p = AppendStr(p, ">");
        AppendStr(p, rx->dest.call);
        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INFO);

        memcpy(line, rx->info, (rx->info_len < COLS) ? rx->info_len : COLS);
        line[(rx->info_len < COLS) ? rx->info_len : COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INFO + 1);
    } else if (gRxMode == APRS_RX_PROBE) {
        /* Six scores: how many bytes of each 32 byte capture came back as 0x01,
         * which is what every lead flag is on air. Bandwidth letter, then the
         * demodulator by its space tone in hundreds. */
        APRS_RxStats_t st;

        APRS_RX_GetStats(&st);

        for (uint8_t bw = 0; bw < 2; bw++) {
            char *p = line;

            *p++ = bw ? 'N' : 'W';

            for (uint8_t m = 0; m < 3; m++) {
                static const char *tone[3] = { "18", "24", "12" };

                p = AppendStr(p, tone[m]);
                *p++ = ':';
                p = AppendNum(p, st.probe[bw * 3u + m], 2);
                *p++ = ' ';
            }

            *p = '\0';
            line[COLS] = '\0';
            UI_PrintStringSmallNormal(line, 2, 0, (uint8_t)(ROW_INFO + bw));
        }
    } else {
        APRS_RxStats_t st;
        char          *p = line;

        APRS_RX_GetStats(&st);

        p = AppendStr(p, "S");
        p = AppendNum(p, st.syncs, 3);
        p = AppendStr(p, " B");
        p = AppendNum(p, st.bytes, 4);
        p = AppendStr(p, " G");
        p = AppendNum(p, st.good_flags, 2);
        p = AppendStr(p, " F");
        p = AppendNum(p, st.frames, 1);
        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INFO);

        /* The first bytes as they arrived. Every one of them should be 01: they
         * are the lead flags that sync did not consume. Seeing what they are
         * instead says whether the demodulator is nearly right or completely
         * wrong, which no counter can. */
        p = line;

        for (uint8_t i = 0; i < sizeof st.first; i++) {
            static const char hex[] = "0123456789abcdef";

            *p++ = hex[st.first[i] >> 4];
            *p++ = hex[st.first[i] & 0x0Fu];
            *p++ = ' ';
        }

        *p = '\0';
        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INFO + 1);
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

static void OnDigit(KEY_Code_t key)
{
    const uint8_t digit = (uint8_t)(key - KEY_0);

    switch (gField) {
        case FIELD_FREQ:
            // Typed left to right over eight digits of 10Hz units, so 14480000
            // is 144.8000 MHz. Restarts once eight have been entered.
            if (gEntryLen >= 8)
                gEntryLen = 0;

            gFreq      = (gEntryLen == 0) ? digit : (gFreq * 10u + digit);
            gEntryLen++;
            gRxRestart = true;
            break;

        case FIELD_GAIN:
            if (gEntryLen >= 3)
                gEntryLen = 0;

            gGain     = (uint8_t)((gEntryLen == 0) ? digit : (gGain * 10u + digit));
            gEntryLen = (uint8_t)(gEntryLen + 1);

            if (gGain > 127)
                gGain = 127;
            break;

        case FIELD_SEND:
            if (digit < SEND_COUNT)
                gSend = digit;

            BuildPayload();
            break;

        case FIELD_RXMODE:
            if (digit < APRS_RX_MODE_COUNT) {
                gRxMode    = digit;
                gRxRestart = true;
            }
            break;

        case FIELD_SPACE:
            BK4819_AfskSetSpaceTone(
                (BK4819_AfskSpaceTone() == BK4819_AFSK_SPACE_HZ)
                ? BK4819_AFSK_SPACE_FFSK_HZ : BK4819_AFSK_SPACE_HZ);
            break;


        default:
            break;
    }

    gRedraw = true;
}

static void OnAdjust(int8_t dir)
{
    switch (gField) {
        case FIELD_FREQ:
            // 12.5kHz, the channel step on this band
            gFreq      = (uint32_t)((int32_t)gFreq + dir * 1250);
            gRxRestart = true;
            break;

        case FIELD_GAIN:
            if (dir > 0 && gGain < 127) gGain++;
            if (dir < 0 && gGain > 0)   gGain--;
            break;

        case FIELD_SEND:
            gSend = (uint8_t)((gSend + SEND_COUNT + dir) % SEND_COUNT);
            BuildPayload();
            break;

        case FIELD_RXMODE:
            gRxMode    = (uint8_t)((gRxMode + APRS_RX_MODE_COUNT + dir) % APRS_RX_MODE_COUNT);
            gRxRestart = true;
            break;

        case FIELD_SPACE:
            BK4819_AfskSetSpaceTone(
                (BK4819_AfskSpaceTone() == BK4819_AFSK_SPACE_HZ)
                ? BK4819_AFSK_SPACE_FFSK_HZ : BK4819_AFSK_SPACE_HZ);
            break;


        default:
            break;
    }

    gEntryLen = 0;
    gRedraw   = true;
}

// The # key. On the frequency it cycles the three APRS channels rather than
// clearing to zero, which is never a useful frequency; elsewhere it restores the
// default.
static void OnClear(void)
{
    switch (gField) {
        case FIELD_FREQ: {
            uint8_t next = 0;

            for (uint8_t i = 0; i < ARRAY_SIZE(gFreqPreset); i++)
                if (gFreqPreset[i] == gFreq)
                    next = (uint8_t)((i + 1) % ARRAY_SIZE(gFreqPreset));

            gFreq      = gFreqPreset[next];
            gRxRestart = true;
            break;
        }

        case FIELD_GAIN:
            gGain = APRS_TX_GAIN;
            break;

        case FIELD_RXMODE:
            gRxMode    = APRS_RX_FFSK1800;
            gRxRestart = true;
            break;

        case FIELD_SPACE:
            BK4819_AfskSetSpaceTone(BK4819_AFSK_SPACE_HZ);
            break;

        default:
            break;
    }

    gEntryLen = 0;
    gRedraw   = true;
}

static void ShowResult(APRS_TxResult_t r, const APRS_TxStats_t *stats)
{
    switch (r) {
        case APRS_TX_OK:
            if (stats != NULL && stats->air_bits != 0) {
                char *p = AppendStr(gStatus, "B");

                p = AppendNum(p, stats->air_bits, 4);
                break;
            }

            AppendStr(gStatus, "SENT");
            break;

        case APRS_TX_ERR_TX_NOT_ALLOWED: AppendStr(gStatus, "NO TX");   break;
        case APRS_TX_ERR_CALL:           AppendStr(gStatus, "BAD CALL"); break;
        case APRS_TX_ERR_TOO_LONG:       AppendStr(gStatus, "TOO LONG"); break;
        case APRS_TX_ERR_MODEM:          AppendStr(gStatus, "MODEM");    break;
        default:                         AppendStr(gStatus, "ERR");      break;
    }
}

static void Transmit(void)
{
    APRS_TxStats_t  stats = { 0, 0, 0 };
    APRS_TxResult_t r;

    gStatus[0] = '\0';
    AppendStr(gStatus, "TX...");
    Render();

    if (gSend == SEND_BEACON || gSend == SEND_STATUS || gSend == SEND_STD) {
        const AX25_Addr_t own = OwnAddr();

        BuildPayload();

        /* SEND_STD omits the alternating preamble, so what goes out is exactly
         * what any other station transmits. If a second radio can hear that, real
         * APRS traffic is receivable; if it cannot, the preamble detector is the
         * wall it was always expected to be. */
        r = APRS_TX_SendPayload(gFreq, gCurrentVfo->TXP_CalculatedSetting,
                                &own, APRS_UI_PATH, gPayload,
                                (gSend == SEND_STD) ? 0u : AX25_LEAD_ALT, &stats);
    } else {
        const APRS_ToneTest_t test = (gSend == SEND_MARK)  ? APRS_TONE_MARK   :
                                     (gSend == SEND_SPACE) ? APRS_TONE_SPACE  :
                                     (gSend == SEND_FLAGS) ? APRS_TONE_FLAGS  :
                                     (gSend == SEND_FSK)   ? APRS_TONE_FSK2400 :
                                     (gSend == SEND_FSK48) ? APRS_TONE_FSK4800 :
                                                             APRS_TONE_ALTERNATING;

        r = APRS_TX_Tone(gFreq, gCurrentVfo->TXP_CalculatedSetting,
                         test, TONE_TEST_MS, gGain);
    }

    gStatus[0] = '\0';
    ShowResult(r, &stats);

    // Transmitting reprograms the chip completely, so the receiver has to be
    // built again rather than assumed to have survived.
    gRxRestart = true;
    gRedraw    = true;
}

static void OnKeyPress(KEY_Code_t key)
{
    BACKLIGHT_TurnOn();

    switch (key) {
        case KEY_0 ... KEY_9:
            OnDigit(key);
            break;

        case KEY_UP:
            gField    = (uint8_t)((gField + FIELD_COUNT - 1) % FIELD_COUNT);
            gEntryLen = 0;
            gRedraw   = true;
            break;

        case KEY_DOWN:
            gField    = (uint8_t)((gField + 1) % FIELD_COUNT);
            gEntryLen = 0;
            gRedraw   = true;
            break;

        case KEY_SIDE1:
            OnAdjust(+1);
            break;

        case KEY_SIDE2:
            OnAdjust(-1);
            break;

        case KEY_F:
            OnClear();
            break;

        case KEY_MENU:
            Transmit();
            break;

        case KEY_EXIT:
            gRunning = false;
            break;

        default:
            break;
    }
}

static bool Repeatable(KEY_Code_t key)
{
    return key == KEY_UP || key == KEY_DOWN || key == KEY_SIDE1 || key == KEY_SIDE2;
}

void APP_RunAprs(void)
{
    KEY_Code_t rawPrev     = KEY_INVALID;
    KEY_Code_t stableKey   = KEY_INVALID;
    uint8_t    stableCount = 0;
    uint16_t   heldTicks   = 0;

    gField     = FIELD_SEND;
    gRunning   = true;
    gRedraw    = true;
    gStatus[0] = '\0';
    gEntryLen  = 0;
    gRxRestart = true;

    BuildPayload();
    BACKLIGHT_TurnOn();

    while (gRunning) {
        if (!gNextTimeslice) {
            if (gRedraw) {
                gRedraw = false;
                Render();
            }
            continue;
        }

        gNextTimeslice = false;

        if (gRxRestart) {
            gRxRestart = false;
            APRS_RX_Start(gFreq, (APRS_RxMode_t)gRxMode);
            gRedraw    = true;
        }

        if (APRS_RX_Poll() == APRS_RX_EVENT_FRAME) {
            gStatus[0] = '\0';
            AppendStr(gStatus, "GOT ONE");
            gRedraw = true;
        }

        /* The counters and the sync indicator change on their own, so they need
         * their own trigger rather than waiting for a keypress. Redrawing only on
         * a change keeps this off the display for most timeslices. */
        {
            static uint16_t was_flushes;
            static uint16_t was_syncs;
            APRS_RxStats_t  st;

            APRS_RX_GetStats(&st);

            /* Deliberately not on the byte count. Redrawing blits the whole
             * screen, and doing that between polls while a capture is arriving
             * adds delay to a FIFO that holds only 107ms of audio. Syncs and
             * completed captures are rare enough to be safe. */
            if (st.flushes != was_flushes || st.syncs != was_syncs) {
                was_flushes = st.flushes;
                was_syncs   = st.syncs;
                gRedraw     = true;
            }
        }

        // Debounce by requiring the same reading twice, then act once on the
        // edge rather than for as long as the key is down.
        const KEY_Code_t raw = KEYBOARD_Poll();

        if (raw != rawPrev) {
            rawPrev     = raw;
            stableCount = 0;
            continue;
        }

        if (stableCount < 2) {
            stableCount++;

            if (stableCount == 2 && raw != stableKey) {
                stableKey = raw;
                heldTicks = 0;

                if (raw != KEY_INVALID)
                    OnKeyPress(raw);
            }
            continue;
        }

        if (stableKey != KEY_INVALID && Repeatable(stableKey)) {
            heldTicks++;

            if (heldTicks > REPEAT_DELAY_10MS && (heldTicks % REPEAT_RATE_10MS) == 0)
                OnKeyPress(stableKey);
        }
    }

    APRS_RX_Stop();

    // hand the radio back to the main screen
    RADIO_SetupRegisters(true);

    gUpdateDisplay = true;
    gUpdateStatus  = true;
}
