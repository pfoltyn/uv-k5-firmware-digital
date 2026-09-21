/* APRS payload formatting. See aprs.h. */

#include <stddef.h>   // NULL

#include "app/aprs.h"

// Bounded appender. max counts the terminating NUL, and anything that does not
// fit sets full, so a caller gets 0 back rather than a quietly truncated frame.
struct sink_t {
    char     *out;
    uint32_t  max;
    uint32_t  n;
    bool      full;
};

static void Put(struct sink_t *s, char c)
{
    if (s->n + 1u >= s->max) {
        s->full = true;
        return;
    }

    s->out[s->n++] = c;
}

static void PutNum(struct sink_t *s, uint32_t value, uint8_t digits)
{
    uint32_t scale = 1;

    for (uint8_t i = 1; i < digits; i++)
        scale *= 10u;

    for (; scale > 0; scale /= 10u)
        Put(s, (char)('0' + ((value / scale) % 10u)));
}

static uint32_t Finish(struct sink_t *s)
{
    if (s->max > 0)
        s->out[s->n] = '\0';

    return s->full ? 0 : s->n;
}

static bool InRange(char c, char lo, char hi)
{
    return c >= lo && c <= hi;
}

bool APRS_GridToLatLon(const char *grid, int32_t *lat_hmin, int32_t *lon_hmin)
{
    uint32_t n = 0;

    if (grid == NULL)
        return false;

    while (grid[n] != '\0')
        n++;

    if (n != 4 && n != APRS_GRID_CHARS)
        return false;

    if (!InRange(grid[0], 'A', 'R') || !InRange(grid[1], 'A', 'R') ||
        !InRange(grid[2], '0', '9') || !InRange(grid[3], '0', '9'))
        return false;

    // Field 20 degrees of longitude by 10 of latitude, square 2 by 1, subsquare
    // 5 minutes by 2.5. Reported at the centre of whichever is the last one
    // given, which is the usual convention and never off by more than half a
    // square.
    int32_t lon = ((int32_t)(grid[0] - 'A') * 20 + (int32_t)(grid[2] - '0') * 2)
                  * APRS_HMIN_PER_DEGREE;
    int32_t lat = ((int32_t)(grid[1] - 'A') * 10 + (int32_t)(grid[3] - '0'))
                  * APRS_HMIN_PER_DEGREE;

    if (n == APRS_GRID_CHARS) {
        char a = grid[4];
        char b = grid[5];

        if (InRange(a, 'A', 'X')) a = (char)(a - 'A' + 'a');
        if (InRange(b, 'A', 'X')) b = (char)(b - 'A' + 'a');

        if (!InRange(a, 'a', 'x') || !InRange(b, 'a', 'x'))
            return false;

        lon += (int32_t)(a - 'a') * 500 + 250;   // 5 minutes, centred
        lat += (int32_t)(b - 'a') * 250 + 125;   // 2.5 minutes, centred
    } else {
        lon += 6000;                             // 2 degrees, centred
        lat += 3000;                             // 1 degree, centred
    }

    if (lat_hmin != NULL)
        *lat_hmin = lat - 90 * APRS_HMIN_PER_DEGREE;

    if (lon_hmin != NULL)
        *lon_hmin = lon - 180 * APRS_HMIN_PER_DEGREE;

    return true;
}

uint32_t APRS_Sanitise(const char *text, char *out, uint32_t max)
{
    struct sink_t s = { out, max, 0, false };

    if (text != NULL)
        for (uint32_t i = 0; text[i] != '\0'; i++) {
            const char c = text[i];

            /* { } | ~ delimit message sequences, third party traffic and
             * telemetry, and a CR or LF ends the payload early in most parsers.
             * Substituting rather than dropping keeps the text the length the
             * operator typed, so what they see is what goes out. */
            const bool ok = c >= 0x20 && c <= 0x7E &&
                            c != '{' && c != '}' && c != '|' && c != '~';

            Put(&s, ok ? c : '.');
        }

    return Finish(&s);
}

// Appends text with the reserved characters replaced, clipped to limit.
static void PutText(struct sink_t *s, const char *text, uint32_t limit)
{
    if (text == NULL)
        return;

    for (uint32_t i = 0; text[i] != '\0' && i < limit; i++) {
        const char c  = text[i];
        const bool ok = c >= 0x20 && c <= 0x7E &&
                        c != '{' && c != '}' && c != '|' && c != '~';

        Put(s, ok ? c : '.');
    }
}

static void PutCoord(struct sink_t *s, int32_t hmin, uint8_t deg_digits,
                     char positive, char negative)
{
    const bool     neg = hmin < 0;
    const uint32_t mag = (uint32_t)(neg ? -hmin : hmin);

    PutNum(s, mag / APRS_HMIN_PER_DEGREE, deg_digits);
    PutNum(s, (mag % APRS_HMIN_PER_DEGREE) / 100u, 2);
    Put(s, '.');
    PutNum(s, mag % 100u, 2);
    Put(s, neg ? negative : positive);
}

uint32_t APRS_FormatPosition(char *out, uint32_t max, const char *grid,
                             char sym_table, char sym_code, const char *comment)
{
    struct sink_t s = { out, max, 0, false };
    int32_t       lat, lon;

    if (out == NULL || !APRS_GridToLatLon(grid, &lat, &lon))
        return 0;

    Put(&s, APRS_DTI_POSITION);
    PutCoord(&s, lat, 2, 'N', 'S');
    Put(&s, (sym_table != '\0') ? sym_table : APRS_SYM_TABLE);
    PutCoord(&s, lon, 3, 'E', 'W');
    Put(&s, (sym_code != '\0') ? sym_code : APRS_SYM_CODE);
    PutText(&s, comment, APRS_MAX_COMMENT);

    return Finish(&s);
}

uint32_t APRS_FormatStatus(char *out, uint32_t max, const char *text)
{
    struct sink_t s = { out, max, 0, false };

    if (out == NULL)
        return 0;

    Put(&s, APRS_DTI_STATUS);
    PutText(&s, text, APRS_MAX_STATUS);

    return Finish(&s);
}

// The addressee is a fixed nine characters, space padded, so that the colon
// after it is always in the same place.
static void PutAddressee(struct sink_t *s, const AX25_Addr_t *to)
{
    char    text[AX25_CALL_CHARS + 4];
    uint8_t n = AX25_FormatAddr(to, text, sizeof text);

    for (uint8_t i = 0; i < APRS_ADDRESSEE_CHARS; i++)
        Put(s, (i < n) ? text[i] : ' ');
}

uint32_t APRS_FormatMessage(char *out, uint32_t max, const AX25_Addr_t *to,
                            const char *text, uint16_t seq)
{
    struct sink_t s = { out, max, 0, false };

    if (out == NULL || to == NULL || !AX25_ValidCall(to->call))
        return 0;

    Put(&s, APRS_DTI_MESSAGE);
    PutAddressee(&s, to);
    Put(&s, ':');
    PutText(&s, text, APRS_MAX_MSG_TEXT);

    // A sequence number is what asks for an acknowledgement; without one the
    // message is send and forget. Two digits is the common form, so wrap into
    // 1..99 rather than widening the field.
    if (seq != 0) {
        Put(&s, '{');
        PutNum(&s, ((uint32_t)(seq - 1u) % 99u) + 1u, 2);
    }

    return Finish(&s);
}

uint32_t APRS_FormatAck(char *out, uint32_t max, const AX25_Addr_t *to,
                        const char *seq)
{
    struct sink_t s = { out, max, 0, false };

    if (out == NULL || to == NULL || seq == NULL || seq[0] == '\0')
        return 0;

    if (!AX25_ValidCall(to->call))
        return 0;

    Put(&s, APRS_DTI_MESSAGE);
    PutAddressee(&s, to);
    Put(&s, ':');
    Put(&s, 'a');
    Put(&s, 'c');
    Put(&s, 'k');

    for (uint32_t i = 0; seq[i] != '\0' && i < APRS_MAX_SEQ; i++)
        Put(&s, seq[i]);

    return Finish(&s);
}

bool APRS_ParseMessage(const char *info, AX25_Addr_t *to,
                       char *text, uint32_t text_max,
                       char *seq, uint32_t seq_max)
{
    char    addressee[APRS_ADDRESSEE_CHARS + 1];
    uint8_t n = 0;

    if (info == NULL || info[0] != APRS_DTI_MESSAGE)
        return false;

    for (uint8_t i = 0; i < APRS_ADDRESSEE_CHARS; i++) {
        const char c = info[1 + i];

        if (c == '\0')
            return false;         // truncated before the addressee finished

        if (c != ' ')
            n = (uint8_t)(i + 1);

        addressee[i] = c;
    }

    addressee[n] = '\0';

    if (info[1 + APRS_ADDRESSEE_CHARS] != ':')
        return false;

    if (to != NULL && !AX25_ParseAddr(addressee, to))
        return false;

    const char *body = info + 1 + APRS_ADDRESSEE_CHARS + 1;
    uint32_t    len  = 0;

    while (body[len] != '\0' && body[len] != '{')
        len++;

    if (text != NULL) {
        struct sink_t s = { text, text_max, 0, false };

        for (uint32_t i = 0; i < len; i++)
            Put(&s, body[i]);

        if (Finish(&s) == 0 && len != 0)
            return false;         // the caller's buffer was too small
    }

    if (seq != NULL) {
        struct sink_t s = { seq, seq_max, 0, false };

        if (body[len] == '{')
            for (uint32_t i = 0; body[len + 1u + i] != '\0' && i < APRS_MAX_SEQ; i++)
                Put(&s, body[len + 1u + i]);

        if (Finish(&s) == 0 && body[len] == '{')
            return false;
    }

    return true;
}
