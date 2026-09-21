/* SMS screen. See sms_ui.h. */

#include <string.h>

#include "app/sms_ui.h"

#include "app/sms_link.h"
#ifdef ENABLE_SMS_CRYPTO
    #include "app/sms_crypto.h"
#endif
#include "driver/backlight.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "misc.h"
#include "radio.h"
#include "ui/helper.h"

// The small font is 6px wide on a 1px pitch, so a row holds 18 characters starting
// at x=2: 2 + 17*7 + 6 is exactly 127, the last byte of the row.
#define COLS                18
#define TAP_TIMEOUT_10MS    80      // how long the same key keeps cycling a letter
#define REPEAT_DELAY_10MS   60
#define REPEAT_RATE_10MS    10

#ifndef SMS_UI_FREQ
    #define SMS_UI_FREQ     14550000u    // 145.500, the 2m simplex calling channel
#endif
#ifndef SMS_UI_KEY
    #define SMS_UI_KEY      "change me"
#endif

enum {
    FIELD_OWN = 0,    // the address this radio answers to
    FIELD_TO,         // where the message goes
    FIELD_FREQ,
    FIELD_MSG,
    FIELD_COUNT
};

/* gFrameBuffer holds FRAME_LINES = 7 rows, drawn as hardware rows 1 to 7; the
 * hardware's row 0 is the separate gStatusLine buffer. So there is no row 7, and
 * writing one runs off the end of the array - a mistake the POCSAG screen made and
 * showed as characters at the edge of the display. The title and the status go on
 * the status line, leaving all seven rows for content. */
#define ROW_FIELD0          0
#define ROW_INBOX           FIELD_COUNT
#define INBOX_ROWS          3

#define STATUS_X_TITLE      2
#define STATUS_X_TEXT       46

static uint16_t gOwn;
static bool     gOwnSet;
/* Not 1, which was a default that could never work: OWN is a 16 bit hash of the chip
 * ID, so it is essentially never 1, and a fresh pair of radios therefore exchanged
 * nothing until somebody read one address off a screen and typed it into the other.
 *
 * Broadcast reaches whoever is listening, and hearing anything at all now adopts that
 * station as the destination - see gToSet - so two radios pair themselves. */
static uint16_t gTo = SMS_ADDR_BROADCAST;
static bool     gToSet;           // the operator has chosen one; stop adopting
static uint32_t gFreq = SMS_UI_FREQ;

static char     gMsg[SMS_MAX_CHARS + 1];
static uint8_t  gMsgLen;

static uint8_t  gField;
static uint8_t  gEntryLen;        // digits typed into the current numeric field
static bool     gRunning;
static bool     gRestart;         // OWN or the frequency changed; re-arm the link
static bool     gRedraw;
static char     gStatus[COLS + 2];

// Multi-tap state: which key is being cycled, how far through, and how long is
// left before the letter commits.
static KEY_Code_t gTapKey;
static uint8_t    gTapIndex;
static uint16_t   gTapTimer;

static const char *const gTap[10] = {
    " 0",  ".,?!-1",  "ABC2", "DEF3", "GHI4",
    "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9"
};

static char *AppendStr(char *p, const char *s)
{
    while (*s != '\0')
        *p++ = *s++;

    *p = '\0';

    return p;
}

static char *AppendNum(char *p, uint32_t v, uint8_t digits)
{
    uint32_t scale = 1;

    for (uint8_t i = 1; i < digits; i++)
        scale *= 10u;

    for (; scale > 0; scale /= 10u)
        *p++ = (char)('0' + ((v / scale) % 10u));

    *p = '\0';

    return p;
}

static void FormatValue(uint8_t field, char *out)
{
    switch (field) {
        case FIELD_OWN:
            AppendNum(out, gOwn, 1);
            break;

        case FIELD_TO:
            if (gTo == SMS_ADDR_BROADCAST)
                AppendStr(out, "ALL");
            else
                AppendNum(out, gTo, 1);
            break;

        case FIELD_FREQ: {
            // stored in 10Hz units, shown as MHz to four places
            char *p = AppendNum(out, gFreq / 100000u, 3);

            *p++ = '.';
            AppendNum(p, (gFreq % 100000u) / 10u, 4);
            break;
        }

        default:
            out[0] = '\0';
            break;
    }
}

static void Render(void)
{
    static const char *label[FIELD_COUNT] = { "OWN", "TO ", "FRQ", "MSG" };
    char value[COLS + 2];
    char line[COLS + 2];

    UI_DisplayClear();
    memset(gStatusLine, 0, LCD_WIDTH);

    UI_PrintStringSmallBufferNormal("SMS", gStatusLine + STATUS_X_TITLE);

    if (gStatus[0] != '\0')
        UI_PrintStringSmallBufferNormal(gStatus, gStatusLine + STATUS_X_TEXT);

    for (uint8_t f = 0; f < FIELD_COUNT; f++) {
        char *p = line;

        if (f == FIELD_MSG) {
            /* The tail of the message with a cursor, so long text stays visible as
             * it is typed rather than scrolling off to the left unseen. */
            const uint8_t room  = COLS - 5;
            const uint8_t shown = (gMsgLen > room) ? room : gMsgLen;

            memcpy(value, gMsg + (gMsgLen - shown), shown);
            value[shown]     = '_';
            value[shown + 1] = '\0';
        } else {
            FormatValue(f, value);
        }

        *p++ = (f == gField) ? '>' : ' ';
        p    = AppendStr(p, label[f]);
        p    = AppendStr(p, " ");
        AppendStr(p, value);

        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, (uint8_t)(ROW_FIELD0 + f));
    }

    /* The rest of the screen is the last message received, if there is one. An if
     * rather than an early return, deliberately: returning here skipped the blits
     * below, so the framebuffer was built and never sent to the display. That is
     * what made the screen appear frozen - nothing was wrong except that the LCD
     * was never told. */
    const SMS_Rx_t *in = SMS_Link_Inbox();

    if (in == NULL) {
#ifdef ENABLE_SMS_DEBUG
        /* The link counters, behind a flag because they are for bring-up rather than
         * for using the radio. Every fault this feature had was found by reading them
         * and none by reading the code, so they are kept a rebuild away rather than
         * deleted: sent, accepted, acknowledged, then CRC and tag failures, and the
         * destination of anything addressed elsewhere. */
        SMS_LinkStats_t st;
        char           *q = line;

        SMS_Link_GetStats(&st);

        q = AppendStr(q, "S");
        q = AppendNum(q, st.sent, 1);
        q = AppendStr(q, " R");
        q = AppendNum(q, st.rx_ok, 1);
        q = AppendStr(q, " A");
        AppendNum(q, st.acks, 1);

        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INBOX);

        q = line;
        q = AppendStr(q, "CRC");
        q = AppendNum(q, st.crc_bad, 1);
        q = AppendStr(q, " TAG");
        q = AppendNum(q, st.tag_bad, 1);

        if (st.addr_bad > 0) {
            q = AppendStr(q, " >");
            AppendNum(q, st.last_dst, 1);
        }

        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INBOX + 1);
#endif

        /* Who has been heard, which is not debug: it is how the operator knows there
         * is a station to send to, and what # on the TO row will adopt. */
        SMS_LinkStats_t heard;

        SMS_Link_GetStats(&heard);

        if (heard.last_src != 0) {
            char *q = AppendStr(line, "HEARD ");

            AppendNum(q, heard.last_src, 1);
            line[COLS] = '\0';
            UI_PrintStringSmallNormal(line, 2, 0, ROW_INBOX + 2);
        }
    } else {
        char *p = AppendStr(line, "FROM ");

        AppendNum(p, in->src, 1);
        line[COLS] = '\0';
        UI_PrintStringSmallNormal(line, 2, 0, ROW_INBOX);

        for (uint8_t r = 0; r + 1 < INBOX_ROWS; r++) {
            const uint16_t off = (uint16_t)(r * COLS);

            if (off >= in->len)
                break;

            uint8_t n = (uint8_t)(in->len - off);

            if (n > COLS)
                n = COLS;

            memcpy(line, in->text + off, n);
            line[n] = '\0';

            // Say when there is more than these rows can show rather than cutting
            // it off silently.
            if (r + 2 == INBOX_ROWS && in->len > off + n)
                line[COLS - 1] = '>';

            UI_PrintStringSmallNormal(line, 2, 0, (uint8_t)(ROW_INBOX + 1 + r));
        }
    }

    // Both, and at the end of every path: the status line and the seven content
    // rows are separate buffers and each needs its own blit.
    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

// --- editing ---------------------------------------------------------------

static void CommitTap(void)
{
    gTapKey   = KEY_INVALID;
    gTapIndex = 0;
    gTapTimer = 0;
}

static void OnDigit(KEY_Code_t key)
{
    const uint8_t digit = (uint8_t)(key - KEY_0);

    if (gField == FIELD_MSG) {
        const char *set = gTap[digit];
        const uint8_t n = (uint8_t)strlen(set);

        /* Pressing the same key again cycles through its letters, as long as it is
         * within the timeout; a different key, or a pause, commits what is there
         * and starts a new character. */
        if (key == gTapKey && gTapTimer > 0 && gMsgLen > 0) {
            gTapIndex = (uint8_t)((gTapIndex + 1) % n);
            gMsg[gMsgLen - 1] = set[gTapIndex];
        } else if (gMsgLen < SMS_MAX_CHARS) {
            gTapKey   = key;
            gTapIndex = 0;
            gMsg[gMsgLen++] = set[0];
            gMsg[gMsgLen]   = '\0';
        }

        gTapTimer = TAP_TIMEOUT_10MS;
        gRedraw   = true;
        return;
    }

    switch (gField) {
        case FIELD_OWN:
        case FIELD_TO: {
            // Typed left to right; restarts once five digits are in, since 65535 is
            // the widest a 16 bit address can be.
            uint32_t v = (gField == FIELD_OWN) ? gOwn : gTo;

            if (gEntryLen >= 5)
                gEntryLen = 0;

            v = (gEntryLen == 0) ? digit : (v * 10u + digit);
            gEntryLen++;

            if (v > 0xFFFFu)
                v = 0xFFFFu;

            if (gField == FIELD_OWN) {
                gOwn     = (uint16_t)v;
                gOwnSet  = true;
                gRestart = true;
            } else {
                gTo     = (uint16_t)v;
                gToSet  = true;
            }
            break;
        }

        case FIELD_FREQ:
            if (gEntryLen >= 8)
                gEntryLen = 0;

            gFreq    = (gEntryLen == 0) ? digit : (gFreq * 10u + digit);
            gEntryLen++;
            gRestart = true;
            break;

        default:
            break;
    }

    gRedraw = true;
}

static void OnBackspace(void)
{
    if (gField == FIELD_MSG && gMsgLen > 0) {
        gMsg[--gMsgLen] = '\0';
        CommitTap();
    }

    gEntryLen = 0;
    gRedraw   = true;
}

// The # key. A space on the message, and on TO it toggles the broadcast address,
// which is more useful than clearing a destination to zero.
static void OnClear(void)
{
    switch (gField) {
        case FIELD_MSG:
            if (gMsgLen < SMS_MAX_CHARS) {
                gMsg[gMsgLen++] = ' ';
                gMsg[gMsgLen]   = '\0';
                CommitTap();
            }
            break;

        case FIELD_TO: {
            /* Cycles the station last heard, then broadcast. Adopting the last station
             * heard is the whole of pairing: the other radio only has to transmit once
             * and its address is available here without anybody reading a screen aloud. */
            SMS_LinkStats_t st;

            SMS_Link_GetStats(&st);

            if (gTo != st.last_src && st.last_src != 0)
                gTo = st.last_src;
            else
                gTo = SMS_ADDR_BROADCAST;

            gToSet = true;
            break;
        }

        case FIELD_OWN:
            gOwn     = SMS_Link_DefaultAddress();
            gOwnSet  = true;
            gRestart = true;
            break;

        default:
            break;
    }

    gEntryLen = 0;
    gRedraw   = true;
}

static void OnAdjust(int8_t dir)
{
    switch (gField) {
        case FIELD_OWN:
            gOwn = (uint16_t)(gOwn + dir); gOwnSet = true; gRestart = true;
            break;

        case FIELD_TO:
            gTo    = (uint16_t)(gTo + dir);
            gToSet = true;
            break;

        case FIELD_FREQ:
            gFreq = (uint32_t)((int32_t)gFreq + dir * 1250); gRestart = true;
            break;
        default: break;
    }

    gEntryLen = 0;
    gRedraw   = true;
}

static void Send(void)
{
    gStatus[0] = '\0';

    if (gMsgLen == 0) {
        AppendStr(gStatus, "EMPTY");
        gRedraw = true;
        return;
    }

    if (SMS_Link_Busy()) {
        AppendStr(gStatus, "BUSY");
        gRedraw = true;
        return;
    }

    // Broadcast has nobody to acknowledge it, so say what it is rather than
    // reporting a failure four attempts later.
    if (!SMS_Link_Send(gTo, gMsg))
        AppendStr(gStatus, "REFUSED");
    else
        AppendStr(gStatus, (gTo == SMS_ADDR_BROADCAST) ? "BCAST" : "SENDING");

    gRedraw = true;
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
            CommitTap();
            gRedraw   = true;
            break;

        case KEY_DOWN:
            gField    = (uint8_t)((gField + 1) % FIELD_COUNT);
            gEntryLen = 0;
            CommitTap();
            gRedraw   = true;
            break;

        case KEY_SIDE1: OnAdjust(+1);   break;
        case KEY_SIDE2: OnAdjust(-1);   break;
        case KEY_STAR:  OnBackspace();  break;
        case KEY_F:     OnClear();      break;
        case KEY_MENU:  Send();         break;

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

static void ShowEvent(SMS_LinkEvent_t ev)
{
    uint8_t done = 0, total = 0, attempt = 0;

    gStatus[0] = '\0';

    switch (ev) {
        case SMS_LINK_EVENT_SENT:
            // The acknowledgement is the delivery proof, so say so plainly.
            AppendStr(gStatus, "DELIVERED");
            gMsgLen = 0;
            gMsg[0] = '\0';
            break;

        case SMS_LINK_EVENT_FAILED:
            SMS_Link_Progress(&done, &total, &attempt);

            // How far it got matters: two of three delivered is a different
            // problem from nothing getting through.
            {
                char *p = AppendStr(gStatus, "FAIL ");

                p = AppendNum(p, done, 1);
                p = AppendStr(p, "/");
                AppendNum(p, total, 1);
            }
            break;

        case SMS_LINK_EVENT_MESSAGE:
            AppendStr(gStatus, "NEW MSG");
            break;

        default:
            break;
    }

    gRedraw = true;
}

void APP_RunSms(void)
{
    KEY_Code_t rawPrev     = KEY_INVALID;
    KEY_Code_t stableKey   = KEY_INVALID;
    uint8_t    stableCount = 0;
    uint16_t   heldTicks   = 0;

    if (!gOwnSet) {
        gOwn    = SMS_Link_DefaultAddress();
        gOwnSet = true;
    }

    gField     = FIELD_MSG;
    gRunning   = true;
    gRedraw    = true;
    gStatus[0] = '\0';
    gEntryLen  = 0;
    CommitTap();

#ifdef ENABLE_SMS_CRYPTO
    /* Keyed once on entry. The derivation iterates ChaCha20 a few thousand times and
     * takes about 40ms, which is why it is not done per message. */
    if (!SMS_Crypto_HaveKey())
        SMS_Crypto_SetKey(SMS_UI_KEY);
#endif

    BACKLIGHT_TurnOn();
    SMS_Link_Start(gFreq, SMS_BAUD_DEFAULT, gOwn);

    while (gRunning) {
        if (!gNextTimeslice) {
            if (gRedraw) {
                gRedraw = false;
                Render();
            }
            continue;
        }

        gNextTimeslice = false;

        /* Changing OWN or the frequency has to reach the link, which captured both
         * when it started. Without this the screen showed a new address while the
         * radio went on answering to the old one, which is indistinguishable from a
         * dead link and would have been a miserable thing to debug on the air. */
        if (gRestart && !SMS_Link_Busy()) {
            gRestart = false;
            SMS_Link_Start(gFreq, SMS_BAUD_DEFAULT, gOwn);
            gRedraw  = true;
        }

        if (gTapTimer > 0 && --gTapTimer == 0)
            CommitTap();

        const SMS_LinkEvent_t ev = SMS_Link_Poll();

        if (ev != SMS_LINK_EVENT_NONE)
            ShowEvent(ev);

        /* Adopt the first station heard as the destination, until the operator picks
         * one. Two radios then pair themselves: whichever transmits first is answered,
         * and its address is learned by the other. */
        if (!gToSet) {
            SMS_LinkStats_t st;

            SMS_Link_GetStats(&st);

            if (st.last_src != 0 && st.last_src != gTo) {
                gTo     = st.last_src;
                gRedraw = true;
            }
        }

#ifdef ENABLE_SMS_DEBUG
        /* The counters move without a keypress, so they need their own trigger. Only
         * on a change: a redraw blits the whole screen, and doing that every timeslice
         * would add delay to a FIFO holding 107ms of audio. */
        {
            static uint16_t was_sent, was_rx, was_bad;
            SMS_LinkStats_t st;

            SMS_Link_GetStats(&st);

            if (st.sent != was_sent || st.rx_ok != was_rx ||
                (uint16_t)(st.crc_bad + st.tag_bad) != was_bad) {
                was_sent = st.sent;
                was_rx   = st.rx_ok;
                was_bad  = (uint16_t)(st.crc_bad + st.tag_bad);
                gRedraw  = true;
            }
        }
#endif

        // Debounce by requiring the same reading twice, then act once on the edge
        // rather than for as long as the key is down.
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

    SMS_Link_Stop();

    gUpdateDisplay = true;
    gUpdateStatus  = true;
}
