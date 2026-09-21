/* Host tests for app/ax25.c and app/aprs.c.
 *
 * The golden address vectors come from direwolf 1.8.1 rather than from reading
 * the specification, because the SSID octet's C bits are the one field where
 * real APRS traffic and a literal reading of AX.25 disagree.
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/aprs.h"
#include "app/ax25.h"

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

// --- helpers ---------------------------------------------------------------

static AX25_Addr_t Addr(const char *call, uint8_t ssid)
{
    AX25_Addr_t a = { { 0 }, ssid, false };

    for (uint8_t i = 0; i < AX25_CALL_CHARS && call[i] != '\0'; i++)
        a.call[i] = call[i];

    return a;
}

// Most tests want no alternating preamble, so the bit offsets they check stay
// the offsets of the flags. TestPreambleAlignsWithTheSyncWord covers it instead.
static uint32_t Encode(const AX25_Frame_t *f, uint8_t lead_flags,
                       uint8_t *storage, uint32_t storage_bytes,
                       AX25_Bits_t *bits)
{
    AX25_BitsInit(bits, storage, storage_bytes);

    if (!AX25_Encode(f, 0, lead_flags, bits))
        return 0;

    return bits->length;
}

// NRZI decode of the whole stream, for tests that want to look at data bits.
static bool DataBitAt(const uint8_t *air, uint32_t i, bool initial)
{
    const bool cur  = (air[i >> 3] >> (7 - (i & 7))) & 1u;
    const bool prev = (i == 0) ? initial : ((air[(i - 1) >> 3] >> (7 - ((i - 1) & 7))) & 1u);

    return cur == prev;
}

// --- tests -----------------------------------------------------------------

static void TestFcsCheckValue(void)
{
    // The documented check value for CRC-16/X-25 over the ASCII digits 1 to 9.
    const uint8_t data[] = "123456789";

    CHECK(AX25_Fcs(data, 9) == 0x906E, "got %04x want 906e", AX25_Fcs(data, 9));
}

static void TestAddressBytesMatchDirewolf(void)
{
    /* direwolf 1.8.1: gen_packets for
     *   SP9ABC-7>APZK5F,WIDE1-1,WIDE2-1:>hello
     * then atest -h, which prints the frame without the FCS. */
    static const uint8_t want[] = {
        0x82, 0xa0, 0xb4, 0x96, 0x6a, 0x8c, 0xe0,   // APZK5F   dest
        0xa6, 0xa0, 0x72, 0x82, 0x84, 0x86, 0xee,   // SP9ABC-7 source
        0xae, 0x92, 0x88, 0x8a, 0x62, 0x40, 0x62,   // WIDE1-1
        0xae, 0x92, 0x88, 0x8a, 0x64, 0x40, 0x63,   // WIDE2-1, last
        0x03, 0xf0,
        '>', 'h', 'e', 'l', 'l', 'o',
    };

    AX25_Frame_t f = { 0 };

    f.dest    = Addr("APZK5F", 0);
    f.src     = Addr("SP9ABC", 7);
    f.digi[0] = Addr("WIDE1", 1);
    f.digi[1] = Addr("WIDE2", 1);
    f.digis   = 2;
    f.info    = ">hello";

    uint8_t        got[AX25_MAX_FRAME];
    const uint32_t len = AX25_BuildFrame(&f, got, sizeof got);

    CHECK(len == sizeof want + 2, "length %u want %zu", len, sizeof want + 2);

    if (len != sizeof want + 2)
        return;

    for (uint32_t i = 0; i < sizeof want; i++)
        CHECK(got[i] == want[i], "byte %u is %02x want %02x", i, got[i], want[i]);
}

static void TestFlagsAreZeroOneOnAir(void)
{
    /* The claim the receive sync word rests on: NRZI turns a run of flags into
     * the air byte 0x01 repeating, so the sync word is 0x01010101 and not
     * 0x7E7E7E7E. If this fails, the receive design is wrong too. */
    AX25_Frame_t f = { 0 };

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = ">x";

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;

    CHECK(Encode(&f, 8, storage, sizeof storage, &bits) != 0, "encode failed");

    for (uint32_t i = 0; i < 8; i++)
        CHECK(storage[i] == 0x01, "lead byte %u is %02x want 01", i, storage[i]);
}

static void TestPreambleAlignsWithTheSyncWord(void)
{
    /* The alternating preamble exists only so the BK4819's own receiver will hunt
     * for a sync word at all; the chip refuses to until it has seen whole bytes
     * of 0xAA or 0x55.
     *
     * The alignment is the part that is easy to get wrong. A flag's first air bit
     * is a 0, so a preamble ending on 1 keeps the alternation going for one more
     * bit and the sync word would be matched one bit late, which is to say never.
     * 0xAA ends on 0 and breaks it exactly at the flag boundary, putting the four
     * flags that follow at 0x01010101, which is the sync word.
     */
    AX25_Frame_t f = { 0 };

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = ">x";

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;

    AX25_BitsInit(&bits, storage, sizeof storage);
    CHECK(AX25_Encode(&f, 8, 8, &bits), "encode failed");

    for (uint32_t i = 0; i < 8; i++)
        CHECK(storage[i] == 0xAA, "preamble byte %u is %02x want aa", i, storage[i]);

    // The sync word the receiver is programmed with, landing immediately after.
    for (uint32_t i = 8; i < 12; i++)
        CHECK(storage[i] == 0x01, "byte %u is %02x want 01", i, storage[i]);

    // And the alternation must stop there rather than running one bit into the
    // flags, which is what 0x55 would have done.
    uint32_t alt = 1;

    while (alt < bits.length &&
           AX25_BitsGet(&bits, alt) != AX25_BitsGet(&bits, alt - 1))
        alt++;

    CHECK(alt == 64, "alternation runs %u bits, want exactly 64", alt);
}

static void TestStuffingCapsRunsOfOnes(void)
{
    // 0xFF info bytes are the worst case for stuffing: without it the data
    // stream would carry long runs of ones and look like flags or aborts.
    char         info[20];
    AX25_Frame_t f = { 0 };

    for (int i = 0; i < 19; i++)
        info[i] = (char)0x7F;   // 0x7F is seven 1 bits, still heavily stuffed

    info[19] = '\0';

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = info;

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;
    uint32_t    n = Encode(&f, 4, storage, sizeof storage, &bits);

    CHECK(n != 0, "encode failed");

    // Between the last lead flag and the closing flag there must be no run of
    // six 1 data bits anywhere. The search has to stop at the closing flag: the
    // flag itself contains six, and the mark idle padding after it is all ones
    // by design, neither of which is data.
    const uint32_t start = 4 * 8;
    uint32_t       flag_start = 0;
    uint8_t        window = 0;

    for (uint32_t i = start; i < n; i++) {
        window = (uint8_t)((window >> 1) |
                           (DataBitAt(storage, i, AX25_NRZI_INITIAL) ? 0x80u : 0u));

        if (i >= start + 7 && window == AX25_FLAG) {
            flag_start = i - 7;
            break;
        }
    }

    CHECK(flag_start > start, "no closing flag found");

    uint32_t run   = 0;
    uint32_t worst = 0;

    for (uint32_t i = start; i < flag_start; i++) {
        run = DataBitAt(storage, i, AX25_NRZI_INITIAL) ? run + 1 : 0;

        if (run > worst)
            worst = run;
    }

    CHECK(worst == 5, "longest run of ones is %u, want exactly 5", worst);
}

static void TestRoundTrip(void)
{
    static const struct {
        const char *dest;
        uint8_t     dest_ssid;
        const char *src;
        uint8_t     src_ssid;
        uint8_t     digis;
        const char *info;
    } cases[] = {
        { "APZK5F", 0,  "SP9ABC", 7,  0, ">status text" },
        { "APZK5F", 0,  "SP9ABC", 7,  2, "=5001.25N/01957.50E-home" },
        { "APRS",   0,  "W1AW",   0,  1, ":SP9ABC-7 :hello there{01" },
        { "APZK5F", 15, "2E0XYZ", 1,  3, "" },
        { "APZK5F", 0,  "G0ABC",  0,  0, "\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7f" },
    };

    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        AX25_Frame_t f = { 0 };

        f.dest  = Addr(cases[c].dest, cases[c].dest_ssid);
        f.src   = Addr(cases[c].src, cases[c].src_ssid);
        f.digis = cases[c].digis;
        f.info  = cases[c].info;

        for (uint8_t d = 0; d < cases[c].digis; d++)
            f.digi[d] = Addr("WIDE2", (uint8_t)(d + 1));

        uint8_t     storage[AX25_MAX_AIR_BYTES];
        AX25_Bits_t bits;
        uint32_t    n = Encode(&f, 6, storage, sizeof storage, &bits);

        CHECK(n != 0, "case %zu encode failed", c);

        AX25_Decoded_t got;

        memset(&got, 0, sizeof got);

        if (!AX25_FromAir(storage, n, AX25_NRZI_INITIAL, &got)) {
            CHECK(false, "case %zu did not decode", c);
            continue;
        }

        CHECK(!strcmp(got.dest.call, cases[c].dest), "case %zu dest '%s'", c, got.dest.call);
        CHECK(got.dest.ssid == cases[c].dest_ssid, "case %zu dest ssid %u", c, got.dest.ssid);
        CHECK(!strcmp(got.src.call, cases[c].src), "case %zu src '%s'", c, got.src.call);
        CHECK(got.src.ssid == cases[c].src_ssid, "case %zu src ssid %u", c, got.src.ssid);
        CHECK(got.digis == cases[c].digis, "case %zu digis %u", c, got.digis);
        CHECK(!strcmp(got.info, cases[c].info), "case %zu info '%s'", c, got.info);
    }
}

static void TestInversionDoesNotMatter(void)
{
    /* POCSAG receive found this chip hands back inverted data. NRZI codes
     * transitions, so inverting the whole stream must change nothing but the
     * very first bit, which is ahead of the flags. If this fails, the receive
     * path needs a polarity setting after all. */
    AX25_Frame_t f = { 0 };

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = ">inverted";

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;
    uint32_t    n = Encode(&f, 6, storage, sizeof storage, &bits);

    CHECK(n != 0, "encode failed");

    for (uint32_t i = 0; i < sizeof storage; i++)
        storage[i] = (uint8_t)~storage[i];

    AX25_Decoded_t got;

    memset(&got, 0, sizeof got);

    CHECK(AX25_FromAir(storage, n, AX25_NRZI_INITIAL, &got),
          "inverted stream did not decode");
    CHECK(!strcmp(got.info, ">inverted"), "info '%s'", got.info);
}

static void TestSingleBitErrorIsRejected(void)
{
    // AX.25 has no error correction, so every corruption must be refused. A
    // decoder that accepted any of these would be showing made up text.
    AX25_Frame_t f = { 0 };

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = ">check the fcs";

    uint8_t     clean[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;
    uint32_t    n = Encode(&f, 4, clean, sizeof clean, &bits);

    CHECK(n != 0, "encode failed");

    /* Only bits inside the frame itself. A flipped bit among the trailing flags
     * does not corrupt anything - that is what they are there for - so including
     * them would be asserting the opposite of the intended behaviour. */
    const uint32_t last = n - (AX25_TAIL_FLAGS + 2u) * 8u;
    uint32_t       accepted = 0;

    for (uint32_t bit = 4 * 8; bit < last; bit++) {
        uint8_t        broken[AX25_MAX_AIR_BYTES];
        AX25_Decoded_t got;

        memcpy(broken, clean, sizeof broken);
        broken[bit >> 3] ^= (uint8_t)(1u << (7 - (bit & 7)));

        memset(&got, 0, sizeof got);

        if (AX25_FromAir(broken, n, AX25_NRZI_INITIAL, &got))
            accepted++;
    }

    CHECK(accepted == 0, "%u of %u single bit errors decoded as a frame",
          accepted, last - 4 * 8);
}

static void TestNoiseDecodesNothing(void)
{
    /* A 16 bit FCS lets one random frame in 65536 through, so this is a check
     * that nothing systematic is wrong, not proof of silence. The UI filter
     * (control 0x03, PID 0xF0, sane callsigns) is what makes it rarer still. */
    uint32_t seed     = 12345u;
    uint32_t accepted = 0;
    const uint32_t trials = 3000;

    for (uint32_t t = 0; t < trials; t++) {
        uint8_t        noise[128];
        AX25_Decoded_t got;

        for (uint32_t i = 0; i < sizeof noise; i++) {
            seed     = seed * 1103515245u + 12345u;
            noise[i] = (uint8_t)(seed >> 16);
        }

        memset(&got, 0, sizeof got);

        if (AX25_FromAir(noise, sizeof noise * 8, AX25_NRZI_INITIAL, &got))
            accepted++;
    }

    CHECK(accepted == 0, "%u of %u noise buffers decoded as a frame",
          accepted, trials);
}

static void TestTruncatedStreamDecodesNothing(void)
{
    // A packet cut short mid frame, which is what a receiver sees when the
    // sender stops or the signal drops, must not produce a partial message.
    AX25_Frame_t f = { 0 };

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = ">a message that gets cut off part way through";

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;
    uint32_t    n = Encode(&f, 4, storage, sizeof storage, &bits);

    CHECK(n != 0, "encode failed");

    /* Cuts that land inside the frame. A cut among the trailing flags leaves the
     * frame and its FCS complete and must still decode, which
     * TestWhatTheFifoDeliversDecodes covers; asserting silence for those would
     * contradict it. */
    const uint32_t last     = n - (AX25_TAIL_FLAGS + 2u) * 8u;
    uint32_t       accepted = 0;

    for (uint32_t cut = 8 * 8; cut < last; cut += 8) {
        AX25_Decoded_t got;

        memset(&got, 0, sizeof got);

        if (AX25_FromAir(storage, cut, AX25_NRZI_INITIAL, &got))
            accepted++;
    }

    CHECK(accepted == 0, "%u truncated streams decoded as a frame", accepted);
}

static void TestNonUiFramesRejected(void)
{
    // Only UI frames with no layer 3 carry APRS. Anything else is a valid AX.25
    // frame that this radio has no use for, and rejecting it also throws out
    // most chance FCS matches on noise.
    AX25_Frame_t f = { 0 };

    f.dest = Addr("APZK5F", 0);
    f.src  = Addr("SP9ABC", 7);
    f.info = ">x";

    uint8_t        raw[AX25_MAX_FRAME];
    const uint32_t len = AX25_BuildFrame(&f, raw, sizeof raw);

    CHECK(len != 0, "build failed");

    // Make the control field something other than UI and repair the FCS, so
    // the only thing wrong with the frame is its type.
    raw[AX25_ADDR_BYTES * 2] = 0x00;

    const uint16_t fcs = AX25_Fcs(raw, len - 2);

    raw[len - 2] = (uint8_t)(fcs & 0xFF);
    raw[len - 1] = (uint8_t)(fcs >> 8);

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;

    AX25_BitsInit(&bits, storage, sizeof storage);
    CHECK(AX25_ToAir(raw, len, 0, 4, &bits), "to air failed");

    AX25_Decoded_t got;

    memset(&got, 0, sizeof got);
    CHECK(!AX25_FromAir(storage, bits.length, AX25_NRZI_INITIAL, &got),
          "a non-UI frame decoded");
}

static void TestInvalidCalls(void)
{
    CHECK(!AX25_ValidCall("sp9abc"), "lower case accepted");
    CHECK(!AX25_ValidCall("SP9ABCD"), "seven characters accepted");
    CHECK(!AX25_ValidCall(""), "empty accepted");
    CHECK(!AX25_ValidCall("SP9-BC"), "punctuation accepted");
    CHECK(AX25_ValidCall("W1AW"), "W1AW rejected");
    CHECK(AX25_ValidCall("SP9ABC"), "SP9ABC rejected");
    CHECK(AX25_ValidCall("2E0XYZ"), "2E0XYZ rejected");

    AX25_Addr_t a;

    CHECK(AX25_ParseAddr("SP9ABC-7", &a) && a.ssid == 7 && !strcmp(a.call, "SP9ABC"),
          "SP9ABC-7 did not parse");
    CHECK(AX25_ParseAddr("WIDE2-15", &a) && a.ssid == 15, "SSID 15 did not parse");
    CHECK(AX25_ParseAddr("W1AW", &a) && a.ssid == 0, "bare call did not parse");
    CHECK(!AX25_ParseAddr("SP9ABC-16", &a), "SSID 16 accepted");
    CHECK(!AX25_ParseAddr("SP9ABC-", &a), "trailing dash accepted");
    CHECK(!AX25_ParseAddr("SP9ABC-x", &a), "non numeric SSID accepted");

    char text[12];

    a = Addr("SP9ABC", 7);
    CHECK(AX25_FormatAddr(&a, text, sizeof text) == 8 && !strcmp(text, "SP9ABC-7"),
          "formatted '%s'", text);

    a = Addr("W1AW", 0);
    CHECK(AX25_FormatAddr(&a, text, sizeof text) == 4 && !strcmp(text, "W1AW"),
          "formatted '%s'", text);

    a = Addr("WIDE2", 15);
    CHECK(AX25_FormatAddr(&a, text, sizeof text) == 8 && !strcmp(text, "WIDE2-15"),
          "formatted '%s'", text);
}

// --- APRS payload ----------------------------------------------------------

static void TestGridSquares(void)
{
    int32_t lat, lon;

    // JO90xa: field J O, square 9 0, subsquare x a. Longitude
    // -180 + 9*20 + 9*2 = 18 degrees, plus 23 subsquares of 5 minutes plus
    // 2.5 to centre = 19 degrees 57.5 minutes. Latitude -90 + 14*10 = 50
    // degrees, plus 1.25 to centre.
    CHECK(APRS_GridToLatLon("JO90xa", &lat, &lon), "JO90xa rejected");
    CHECK(lat == 50 * 6000 + 125, "JO90xa lat %d", lat);
    CHECK(lon == 19 * 6000 + 5750, "JO90xa lon %d", lon);

    // Lower left corner of the grid system, centred in its subsquare.
    CHECK(APRS_GridToLatLon("AA00aa", &lat, &lon), "AA00aa rejected");
    CHECK(lat == -90 * 6000 + 125, "AA00aa lat %d", lat);
    CHECK(lon == -180 * 6000 + 250, "AA00aa lon %d", lon);

    // Four characters report the centre of the square instead.
    CHECK(APRS_GridToLatLon("JO90", &lat, &lon), "JO90 rejected");
    CHECK(lat == 50 * 6000 + 3000, "JO90 lat %d", lat);
    CHECK(lon == 18 * 6000 + 6000, "JO90 lon %d", lon);

    // The subsquare may be given in upper case.
    int32_t lat2, lon2;

    CHECK(APRS_GridToLatLon("JO90XA", &lat2, &lon2), "JO90XA rejected");
    CHECK(APRS_GridToLatLon("JO90xa", &lat, &lon) && lat == lat2 && lon == lon2,
          "case of the subsquare changed the answer");

    CHECK(!APRS_GridToLatLon("JO9", NULL, NULL), "3 characters accepted");
    CHECK(!APRS_GridToLatLon("JO905", NULL, NULL), "5 characters accepted");
    CHECK(!APRS_GridToLatLon("SO90xa", NULL, NULL), "field S accepted");
    CHECK(!APRS_GridToLatLon("JO90ya", NULL, NULL), "subsquare y accepted");
    CHECK(!APRS_GridToLatLon("J090xa", NULL, NULL), "digit in the field accepted");
    CHECK(!APRS_GridToLatLon("", NULL, NULL), "empty accepted");
}

static void TestPositionFormat(void)
{
    char out[64];

    CHECK(APRS_FormatPosition(out, sizeof out, "JO90xa", '/', '-', "UV-K5") != 0 &&
          !strcmp(out, "=5001.25N/01957.50E-UV-K5"), "position '%s'", out);

    // Southern and western hemispheres, and the three digit degrees field.
    CHECK(APRS_FormatPosition(out, sizeof out, "AA00aa", '/', '-', "") != 0 &&
          !strcmp(out, "=8958.75S/17957.50W-"), "position '%s'", out);

    CHECK(APRS_FormatPosition(out, sizeof out, "NOPE", '/', '-', "") == 0,
          "an invalid grid produced '%s'", out);

    // Too small a buffer has to fail rather than truncate: a clipped position
    // report is a wrong position, not a shorter one.
    CHECK(APRS_FormatPosition(out, 10, "JO90xa", '/', '-', "UV-K5") == 0,
          "truncation was not reported");
}

static void TestStatusAndSanitise(void)
{
    char out[80];

    CHECK(APRS_FormatStatus(out, sizeof out, "UV-K5 on 144.800") != 0 &&
          !strcmp(out, ">UV-K5 on 144.800"), "status '%s'", out);

    // The four characters APRS reserves, plus a control character, all become
    // dots so the text keeps the length the operator typed.
    CHECK(APRS_FormatStatus(out, sizeof out, "a{b}c|d~e\nf") != 0 &&
          !strcmp(out, ">a.b.c.d.e.f"), "status '%s'", out);

    CHECK(APRS_Sanitise("ok text", out, sizeof out) == 7 && !strcmp(out, "ok text"),
          "sanitise '%s'", out);

    // Status is capped at its APRS limit rather than overflowing the frame.
    char  longtext[200];

    memset(longtext, 'x', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = '\0';

    CHECK(APRS_FormatStatus(out, sizeof out, longtext) == APRS_MAX_STATUS + 1,
          "long status came out %u", APRS_FormatStatus(out, sizeof out, longtext));
}

static void TestMessageRoundTrip(void)
{
    const AX25_Addr_t to = Addr("SP9ABC", 7);
    char              out[96];

    CHECK(APRS_FormatMessage(out, sizeof out, &to, "hello there", 1) != 0 &&
          !strcmp(out, ":SP9ABC-7 :hello there{01"), "message '%s'", out);

    // Nine characters of addressee, space padded, so the colon is always in the
    // same place whatever the callsign length.
    const AX25_Addr_t shortcall = Addr("W1AW", 0);

    CHECK(APRS_FormatMessage(out, sizeof out, &shortcall, "hi", 0) != 0 &&
          !strcmp(out, ":W1AW     :hi"), "message '%s'", out);

    AX25_Addr_t parsed;
    char        text[80];
    char        seq[8];

    CHECK(APRS_FormatMessage(out, sizeof out, &to, "round trip", 42) != 0,
          "format failed");
    CHECK(APRS_ParseMessage(out, &parsed, text, sizeof text, seq, sizeof seq),
          "parse failed");
    CHECK(!strcmp(parsed.call, "SP9ABC") && parsed.ssid == 7,
          "addressee '%s'-%u", parsed.call, parsed.ssid);
    CHECK(!strcmp(text, "round trip"), "text '%s'", text);
    CHECK(!strcmp(seq, "42"), "seq '%s'", seq);

    // No sequence number means no acknowledgement was asked for.
    CHECK(APRS_FormatMessage(out, sizeof out, &shortcall, "no ack", 0) != 0 &&
          APRS_ParseMessage(out, &parsed, text, sizeof text, seq, sizeof seq) &&
          seq[0] == '\0', "seq '%s' should be empty", seq);

    CHECK(APRS_FormatAck(out, sizeof out, &to, "42") != 0 &&
          !strcmp(out, ":SP9ABC-7 :ack42"), "ack '%s'", out);

    CHECK(!APRS_ParseMessage(">not a message", &parsed, text, sizeof text, seq, sizeof seq),
          "a status parsed as a message");
    CHECK(!APRS_ParseMessage(":short", &parsed, text, sizeof text, seq, sizeof seq),
          "a truncated message parsed");
}

static void TestWhatTheFifoDeliversDecodes(void)
{
    /* The assumption the whole receive design rests on.
     *
     * The chip hands over the bytes that follow a sync word match, and nothing
     * before it. Our air stream is AX25_LEAD_ALT bytes of 0xAA, then the flags,
     * whose NRZI form is 0x01 each; the sync word 0x01010101 therefore matches
     * the first four flags, and the FIFO starts delivering from the fifth. If
     * what arrives from there is not a decodable stream, the sync word is wrong.
     */
    AX25_Frame_t f = { 0 };

    f.dest    = Addr("APZK5F", 0);
    f.src     = Addr("SP9ABC", 7);
    f.digi[0] = Addr("WIDE1", 1);
    f.digis   = 1;
    f.info    = "=5211.25N/00007.50E-from the fifo";

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;

    AX25_BitsInit(&bits, storage, sizeof storage);
    CHECK(AX25_Encode(&f, AX25_LEAD_ALT, AX25_LEAD_FLAGS, &bits), "encode failed");

    // Where the chip would start handing bytes over: past the preamble and the
    // four flags the sync word consumed.
    const uint32_t offset = AX25_LEAD_ALT + 4u;

    CHECK(storage[offset - 1] == 0x01, "sync does not end on a flag byte");

    AX25_Decoded_t got;

    memset(&got, 0, sizeof got);

    CHECK(AX25_FromAir(storage + offset, (bits.length / 8u - offset) * 8u,
                       AX25_NRZI_INITIAL, &got),
          "what the FIFO would deliver did not decode");
    CHECK(!strcmp(got.src.call, "SP9ABC") && got.src.ssid == 7,
          "source came out '%s'-%u", got.src.call, got.src.ssid);
    CHECK(!strcmp(got.info, f.info), "info came out '%s'", got.info);

    /* And inverted, because the chip matches sync in both senses and POCSAG
     * receive found this hardware handing back inverted data. NRZI codes
     * transitions, so this must change nothing. */
    uint8_t flipped[AX25_MAX_AIR_BYTES];

    for (uint32_t i = 0; i < sizeof flipped; i++)
        flipped[i] = (uint8_t)~storage[i];

    memset(&got, 0, sizeof got);

    CHECK(AX25_FromAir(flipped + offset, (bits.length / 8u - offset) * 8u,
                       AX25_NRZI_INITIAL, &got),
          "the inverted stream did not decode");
    CHECK(!strcmp(got.info, f.info), "inverted info came out '%s'", got.info);

    /* And with the tail cut off, which is what actually happens: the chip's FIFO
     * strands its final partial chunk and can hold a further sixteen bytes when
     * the carrier drops. This is the whole reason AX25_TAIL_FLAGS exists, and the
     * first receive with a working demodulator failed here - twelve of twelve
     * lead flags correct and ten bytes missing from the end. */
    for (uint32_t lost = 1; lost <= AX25_TAIL_FLAGS; lost++) {
        const uint32_t bytes = bits.length / 8u - offset - lost;

        memset(&got, 0, sizeof got);

        CHECK(AX25_FromAir(storage + offset, bytes * 8u, AX25_NRZI_INITIAL, &got),
              "losing the last %u bytes stopped it decoding", lost);
    }
}

static void TestFullFrameFitsOneFifoLoad(void)
{
    /* The worst case frame has to fit the air buffer, and the receive path has to
     * be able to capture it.
     *
     * This used to assert it fitted one 256 byte FIFO load, which mattered when
     * transmit went through the hardware modem. The bit banged path streams from
     * RAM and has no such limit, so what matters now is only that the buffer and
     * the receive capture length are both big enough for what AX.25 permits. */
    char         info[AX25_MAX_INFO + 1];
    AX25_Frame_t f = { 0 };

    for (int i = 0; i < AX25_MAX_INFO; i++)
        info[i] = (char)0x7F;   // worst case for bit stuffing

    info[AX25_MAX_INFO] = '\0';

    f.dest  = Addr("APZK5F", 0);
    f.src   = Addr("SP9ABC", 7);
    f.digis = AX25_MAX_DIGIS;
    f.info  = info;

    for (uint8_t d = 0; d < AX25_MAX_DIGIS; d++)
        f.digi[d] = Addr("WIDE2", 2);

    uint8_t     storage[AX25_MAX_AIR_BYTES];
    AX25_Bits_t bits;
    uint32_t    n = Encode(&f, AX25_LEAD_FLAGS, storage, sizeof storage, &bits);

    CHECK(n != 0, "the worst case frame overflowed AX25_MAX_AIR_BYTES");
    CHECK(n / 8 <= AX25_MAX_AIR_BYTES, "worst case is %u air bytes, buffer is %u",
          n / 8, AX25_MAX_AIR_BYTES);
    CHECK((n / 8) % 2 == 0, "air length %u bytes is odd, the FIFO takes words", n / 8);

    // The full eight digipeaters AX.25 allows, which is what a heavily relayed
    // real frame carries and what the old limit of three would have refused.
    CHECK(AX25_MAX_DIGIS == 8, "AX25_MAX_DIGIS is %u, AX.25 allows 8", AX25_MAX_DIGIS);
}

// ---------------------------------------------------------------------------

int main(void)
{
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "FCS check value",              TestFcsCheckValue },
        { "addresses match direwolf",     TestAddressBytesMatchDirewolf },
        { "flags are 0x01 on air",        TestFlagsAreZeroOneOnAir },
        { "preamble aligns with sync",    TestPreambleAlignsWithTheSyncWord },
        { "stuffing caps runs of ones",   TestStuffingCapsRunsOfOnes },
        { "round trip",                   TestRoundTrip },
        { "inversion does not matter",    TestInversionDoesNotMatter },
        { "single bit errors rejected",   TestSingleBitErrorIsRejected },
        { "noise decodes nothing",        TestNoiseDecodesNothing },
        { "truncated stream rejected",    TestTruncatedStreamDecodesNothing },
        { "non-UI frames rejected",       TestNonUiFramesRejected },
        { "callsign validation",          TestInvalidCalls },
        { "grid squares",                 TestGridSquares },
        { "position format",              TestPositionFormat },
        { "status and sanitise",          TestStatusAndSanitise },
        { "message round trip",           TestMessageRoundTrip },
        { "what the FIFO delivers",       TestWhatTheFifoDeliversDecodes },
        { "worst case fits one FIFO",     TestFullFrameFitsOneFifoLoad },
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = failures;

        tests[i].fn();
        printf("%-32s %s\n", tests[i].name, (failures == before) ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
