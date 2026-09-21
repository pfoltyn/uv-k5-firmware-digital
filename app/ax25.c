/* AX.25 UI frames and the HDLC bit stream that carries them. See ax25.h. */

#include <stddef.h>   // NULL

#include "app/ax25.h"

/* SSID octet layout:  <7> C/R or has-been-repeated, <6:5> reserved,
 *                     <4:1> SSID, <0> last address in the list.
 *
 * The constants are taken from a frame direwolf 1.8.1 generated rather than
 * from the specification, because the C bits are the one field where real APRS
 * traffic and a literal reading of AX.25 disagree. gen_packets for
 * SP9ABC-7>APZK5F,WIDE1-1,WIDE2-1 gives SSID octets e0 ee 62 63, so both
 * reserved bits are set, dest and source both have <7> set, and a digipeater
 * that has not repeated the frame yet has it clear. */
#define SSID_LAST       0x01u
#define SSID_RESERVED   0x60u
#define SSID_CR         0x80u   // on dest and source
#define SSID_REPEATED   0x80u   // the same bit on a digipeater

#define AX25_MIN_FRAME  (AX25_ADDR_BYTES * 2 + 2 + 2)   // no digis, no info

void AX25_BitsInit(AX25_Bits_t *bits, uint8_t *storage, uint32_t capacity_bytes)
{
    bits->data          = storage;
    bits->capacity_bits = capacity_bytes * 8;
    bits->length        = 0;
    bits->overflow      = false;

    for (uint32_t i = 0; i < capacity_bytes; i++)
        storage[i] = 0;
}

bool AX25_BitsGet(const AX25_Bits_t *bits, uint32_t index)
{
    if (index >= bits->length)
        return false;
    return (bits->data[index >> 3] >> (7 - (index & 7))) & 1u;
}

static void PushBit(AX25_Bits_t *bits, bool bit)
{
    if (bits->length >= bits->capacity_bits) {
        bits->overflow = true;
        return;
    }

    if (bit)
        bits->data[bits->length >> 3] |= 1u << (7 - (bits->length & 7));
    else
        bits->data[bits->length >> 3] &= ~(1u << (7 - (bits->length & 7)));

    bits->length++;
}

static uint32_t StrLen(const char *s)
{
    uint32_t n = 0;

    if (s != NULL)
        while (s[n] != '\0')
            n++;

    return n;
}

bool AX25_ValidCall(const char *call)
{
    if (call == NULL || call[0] == '\0')
        return false;

    uint32_t n = 0;

    for (; call[n] != '\0'; n++) {
        const char c = call[n];

        // Lower case is rejected rather than folded: a mistyped callsign should
        // fail at entry, not go out on air looking like someone else's.
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }

    return n <= AX25_CALL_CHARS;
}

uint16_t AX25_Fcs(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;

    // Bitwise rather than table driven: a 512 byte table is a poor trade
    // against 2 kB of free flash, and a frame is only a couple of hundred bytes.
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];

        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u)   // reflected 0x1021
                             : (uint16_t)(crc >> 1);
    }

    return (uint16_t)(crc ^ 0xFFFFu);
}

static void EncodeAddr(const AX25_Addr_t *addr, uint8_t *out, uint8_t base, bool last)
{
    const uint32_t n = StrLen(addr->call);

    for (uint8_t i = 0; i < AX25_CALL_CHARS; i++)
        out[i] = (uint8_t)(((i < n) ? addr->call[i] : ' ') << 1);

    out[AX25_CALL_CHARS] = (uint8_t)(base | ((addr->ssid & 0x0Fu) << 1) |
                                     (last ? SSID_LAST : 0u));
}

uint32_t AX25_BuildFrame(const AX25_Frame_t *frame, uint8_t *out, uint32_t max)
{
    if (frame == NULL || out == NULL)
        return 0;

    if (frame->digis > AX25_MAX_DIGIS)
        return 0;

    if (!AX25_ValidCall(frame->dest.call) || !AX25_ValidCall(frame->src.call))
        return 0;

    for (uint8_t i = 0; i < frame->digis; i++)
        if (!AX25_ValidCall(frame->digi[i].call))
            return 0;

    const uint32_t info_len = StrLen(frame->info);

    if (info_len > AX25_MAX_INFO)
        return 0;

    const uint32_t total = AX25_ADDR_BYTES * (2u + frame->digis) + 2u + info_len + 2u;

    if (total > max)
        return 0;

    uint32_t n = 0;

    EncodeAddr(&frame->dest, out + n, SSID_CR | SSID_RESERVED, false);
    n += AX25_ADDR_BYTES;

    EncodeAddr(&frame->src, out + n, SSID_CR | SSID_RESERVED, frame->digis == 0);
    n += AX25_ADDR_BYTES;

    for (uint8_t i = 0; i < frame->digis; i++) {
        EncodeAddr(&frame->digi[i], out + n,
                   (uint8_t)(SSID_RESERVED | (frame->digi[i].repeated ? SSID_REPEATED : 0u)),
                   i + 1u == frame->digis);
        n += AX25_ADDR_BYTES;
    }

    out[n++] = AX25_CONTROL_UI;
    out[n++] = AX25_PID_NO_LAYER3;

    for (uint32_t i = 0; i < info_len; i++)
        out[n++] = (uint8_t)frame->info[i];

    const uint16_t fcs = AX25_Fcs(out, n);

    out[n++] = (uint8_t)(fcs & 0xFFu);   // low byte first
    out[n++] = (uint8_t)(fcs >> 8);

    return n;
}

// --- air bit stream --------------------------------------------------------

struct air_t {
    AX25_Bits_t *bits;
    bool         level;   // current NRZI level
    uint8_t      ones;    // consecutive 1 data bits, for stuffing
};

static void PushNrzi(struct air_t *a, bool data_bit)
{
    if (!data_bit)             // a 0 toggles, a 1 holds
        a->level = !a->level;

    PushBit(a->bits, a->level);
}

static void PushFlag(struct air_t *a)
{
    // Flags are never stuffed, and they reset the stuffing state.
    a->ones = 0;

    for (uint8_t i = 0; i < 8; i++)
        PushNrzi(a, (AX25_FLAG >> i) & 1u);   // least significant bit first
}

static void PushByte(struct air_t *a, uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++) {
        const bool b = (byte >> i) & 1u;

        PushNrzi(a, b);

        if (!b) {
            a->ones = 0;
            continue;
        }

        if (++a->ones == 5) {   // five 1s in a row, so insert a 0
            PushNrzi(a, false);
            a->ones = 0;
        }
    }
}

bool AX25_ToAir(const uint8_t *frame, uint32_t len,
                uint8_t alt_bytes, uint8_t lead_flags, AX25_Bits_t *bits)
{
    if (frame == NULL || bits == NULL || len < AX25_MIN_FRAME)
        return false;

    struct air_t a = { bits, AX25_NRZI_INITIAL, 0 };

    if (lead_flags == 0)
        lead_flags = 1;

    /* Alternating air levels, written straight out rather than through the NRZI
     * encoder: this is not data, it is a run pattern for the far end's preamble
     * detector, and it has to end on 0 so the flags that follow break the
     * alternation at the right bit. Starting at 1 and ending at 0 is what a byte
     * of 0xAA looks like. */
    for (uint8_t i = 0; i < alt_bytes; i++)
        for (uint8_t b = 0; b < 8; b++)
            PushBit(bits, (b & 1u) == 0);

    for (uint8_t i = 0; i < lead_flags; i++)
        PushFlag(&a);

    for (uint32_t i = 0; i < len; i++)
        PushByte(&a, frame[i]);

    PushFlag(&a);

    // Trailing flags, so that whatever the far end's FIFO strands is padding
    // rather than the FCS. See AX25_TAIL_FLAGS.
    for (uint8_t i = 0; i < AX25_TAIL_FLAGS; i++)
        PushFlag(&a);

    /* Stuffing means the closing flag rarely lands on a byte boundary, and the
     * modem transmits whole bytes. Holding the level fills out the byte as a
     * steady tone, and a flag brings the count to even for the FIFO, which is
     * written a word at a time. An HDLC receiver has already closed the frame
     * at the flag, so none of this padding can be mistaken for data. */
    while ((bits->length & 7u) != 0)
        PushNrzi(&a, true);

    if (((bits->length >> 3) & 1u) != 0)
        PushFlag(&a);

    return !bits->overflow;
}

bool AX25_Encode(const AX25_Frame_t *frame, uint8_t alt_bytes,
                 uint8_t lead_flags, AX25_Bits_t *bits)
{
    uint8_t        buf[AX25_MAX_FRAME];
    const uint32_t len = AX25_BuildFrame(frame, buf, sizeof buf);

    if (len == 0)
        return false;

    return AX25_ToAir(buf, len, alt_bytes, lead_flags, bits);
}

// --- receive ---------------------------------------------------------------

static bool AirBit(const uint8_t *air, uint32_t i)
{
    return (air[i >> 3] >> (7 - (i & 7))) & 1u;
}

static bool DataBit(const uint8_t *air, uint32_t i, bool initial)
{
    const bool prev = (i == 0) ? initial : AirBit(air, i - 1);

    return AirBit(air, i) == prev;   // NRZI: no change means 1
}

static void DecodeAddr(const uint8_t *in, AX25_Addr_t *addr)
{
    uint8_t n = 0;

    for (uint8_t i = 0; i < AX25_CALL_CHARS; i++) {
        const char c = (char)(in[i] >> 1);

        if (c != ' ')
            n = (uint8_t)(i + 1);     // keep embedded spaces, drop trailing ones

        addr->call[i] = c;
    }

    addr->call[n]  = '\0';
    addr->ssid     = (in[AX25_CALL_CHARS] >> 1) & 0x0Fu;
    addr->repeated = (in[AX25_CALL_CHARS] & SSID_REPEATED) != 0;
}

static bool ParseFrame(const uint8_t *buf, uint32_t len, AX25_Decoded_t *out)
{
    uint32_t n     = 0;
    uint8_t  addrs = 0;
    bool     last  = false;

    while (!last) {
        if (n + AX25_ADDR_BYTES > len || addrs > 2u + AX25_MAX_DIGIS)
            return false;

        last = (buf[n + AX25_CALL_CHARS] & SSID_LAST) != 0;

        AX25_Addr_t *dst = (addrs == 0) ? &out->dest :
                           (addrs == 1) ? &out->src  : &out->digi[addrs - 2];

        DecodeAddr(buf + n, dst);

        n += AX25_ADDR_BYTES;
        addrs++;
    }

    if (addrs < 2)
        return false;

    out->digis = (uint8_t)(addrs - 2);

    // Only UI frames with no layer 3 carry APRS. Anything else is a valid AX.25
    // frame we have no use for, and rejecting it here also filters a chance FCS
    // match on noise.
    if (n + 2 > len || buf[n] != AX25_CONTROL_UI || buf[n + 1] != AX25_PID_NO_LAYER3)
        return false;

    n += 2;

    const uint32_t info_len = len - n;

    if (info_len > AX25_MAX_INFO)
        return false;

    for (uint32_t i = 0; i < info_len; i++)
        out->info[i] = (char)buf[n + i];

    out->info[info_len] = '\0';
    out->info_len       = (uint8_t)info_len;

    return true;
}

static bool TryFrame(const uint8_t *air, uint32_t start, uint32_t nbits,
                     bool initial, AX25_Decoded_t *out)
{
    uint8_t  buf[AX25_MAX_FRAME];
    uint32_t nbytes   = 0;
    uint8_t  acc      = 0;
    uint8_t  acc_bits = 0;
    uint8_t  ones     = 0;

    if (nbits < AX25_MIN_FRAME * 8u)
        return false;

    for (uint32_t i = 0; i < nbits; i++) {
        const bool b = DataBit(air, start + i, initial);

        if (ones == 5 && !b) {   // the stuffed zero, which carries no data
            ones = 0;
            continue;
        }

        if (b) {
            if (++ones > 5)      // six 1s cannot occur inside a frame
                return false;
        } else {
            ones = 0;
        }

        acc |= (uint8_t)(b << acc_bits);   // least significant bit first

        if (++acc_bits == 8) {
            if (nbytes >= sizeof buf)
                return false;

            buf[nbytes++] = acc;
            acc           = 0;
            acc_bits      = 0;
        }
    }

    if (acc_bits != 0 || nbytes < AX25_MIN_FRAME)
        return false;

    const uint16_t want = (uint16_t)(buf[nbytes - 2] | (buf[nbytes - 1] << 8));

    if (AX25_Fcs(buf, nbytes - 2) != want)
        return false;

    return ParseFrame(buf, nbytes - 2, out);
}

bool AX25_FromAir(const uint8_t *air, uint32_t air_bits,
                  bool initial_level, AX25_Decoded_t *out)
{
    uint8_t  window        = 0;
    uint32_t prev_flag_end = 0;
    bool     have_flag     = false;

    if (air == NULL || out == NULL)
        return false;

    /* Flags delimit frames and cannot occur inside one, so finding them needs no
     * state beyond an 8 bit window. Each pair of flags brackets a candidate,
     * which is then de-stuffed and FCS checked; the first that passes wins. */
    for (uint32_t i = 0; i < air_bits; i++) {
        window = (uint8_t)((window >> 1) |
                           (DataBit(air, i, initial_level) ? 0x80u : 0u));

        if (i < 7 || window != AX25_FLAG)
            continue;

        const uint32_t start = have_flag ? prev_flag_end + 1u : 0u;

        // The flag occupies bits i-7 to i, so the candidate ends at i-8.
        if (have_flag && (i - 7u) > start &&
            TryFrame(air, start, (i - 7u) - start, initial_level, out))
            return true;

        prev_flag_end = i;
        have_flag     = true;
    }

    return false;
}

// --- text form -------------------------------------------------------------

uint8_t AX25_FormatAddr(const AX25_Addr_t *addr, char *out, uint8_t max)
{
    const uint32_t n = StrLen(addr->call);
    uint8_t        w = 0;

    if (out == NULL || max == 0)
        return 0;

    for (uint32_t i = 0; i < n && w + 1u < max; i++)
        out[w++] = addr->call[i];

    if (addr->ssid != 0 && w + 1u < max) {
        out[w++] = '-';

        if (addr->ssid >= 10 && w + 1u < max)
            out[w++] = '1';

        if (w + 1u < max)
            out[w++] = (char)('0' + (addr->ssid % 10u));
    }

    out[w] = '\0';

    return w;
}

bool AX25_ParseAddr(const char *text, AX25_Addr_t *addr)
{
    uint8_t n = 0;

    if (text == NULL || addr == NULL)
        return false;

    while (text[n] != '\0' && text[n] != '-') {
        if (n >= AX25_CALL_CHARS)
            return false;

        addr->call[n] = text[n];
        n++;
    }

    addr->call[n]  = '\0';
    addr->ssid     = 0;
    addr->repeated = false;

    if (!AX25_ValidCall(addr->call))
        return false;

    if (text[n] == '\0')
        return true;

    const char *s = text + n + 1;

    if (*s < '0' || *s > '9')
        return false;

    uint16_t ssid = 0;

    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return false;

        ssid = (uint16_t)(ssid * 10u + (uint16_t)(*s - '0'));

        if (ssid > AX25_MAX_SSID)
            return false;
    }

    addr->ssid = (uint8_t)ssid;

    return true;
}
