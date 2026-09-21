/* Short messages over the BK4819's FSK modem. See sms.h. */

#include <stddef.h>   // NULL

#include "app/sms.h"

// Frame layout, all little endian where a field is wider than a byte:
//
//   0      type<7:6> | frags<5:4> | frag<3:2> | reserved<1:0>
//   1..2   src
//   3..4   dst
//   5      msg_id
//   6      len
//   7      reserved, zero
//   8..61  payload
//   62..63 CRC-16 over bytes 0 to 61
#define OFF_FLAGS     0
#define OFF_SRC       1
#define OFF_DST       3
#define OFF_MSGID     5
#define OFF_LEN       6
#define OFF_PAYLOAD   8

static uint32_t StrLen(const char *s)
{
    uint32_t n = 0;

    if (s != NULL)
        while (s[n] != '\0')
            n++;

    return n;
}

uint16_t SMS_Crc(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;

    /* CRC-16/CCITT-FALSE, computed bitwise. A 512 byte table would be a poor
     * trade against a budget of about 8kB for the whole feature, and a frame is
     * 62 bytes. */
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);

        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }

    return crc;
}

void SMS_FrameEncode(const SMS_Frame_t *frame, uint8_t *out)
{
    /* Padded with 0xAA, not zero, and this matters more than it looks.
     *
     * A short message leaves most of the payload unused. Zero filled, a 13 character
     * message produced 234 consecutive identical bits on air, and no clock recovery
     * survives that: the sync word matched on its fixed pattern, the header and
     * ciphertext clocked through, and then a quarter of a second with no transition
     * destroyed the bit timing so every CRC failed. It looked like a signal problem
     * and was not - the APRS direct FSK test passed 64 of 64 bytes at the same rate
     * because it happened to send 0x01 repeated, which transitions every eight bits.
     *
     * 0xAA alternates every bit, so the padding now carries the clock rather than
     * starving it. The value need not agree between the two ends for the CRC to
     * match, since each computes over the bytes it has - but it must be deterministic,
     * and it is. */
    for (uint32_t i = 0; i < SMS_FRAME_BYTES; i++)
        out[i] = 0xAAu;

    out[OFF_FLAGS]     = (uint8_t)(((frame->type  & 3u) << 6) |
                                   ((frame->frags & 3u) << 4) |
                                   ((frame->frag  & 3u) << 2));
    out[OFF_SRC]       = (uint8_t)(frame->src & 0xFFu);
    out[OFF_SRC + 1]   = (uint8_t)(frame->src >> 8);
    out[OFF_DST]       = (uint8_t)(frame->dst & 0xFFu);
    out[OFF_DST + 1]   = (uint8_t)(frame->dst >> 8);
    out[OFF_MSGID]     = frame->msg_id;
    out[OFF_LEN]       = frame->len;

    for (uint8_t i = 0; i < frame->len && i < SMS_PAYLOAD_BYTES; i++)
        out[OFF_PAYLOAD + i] = frame->payload[i];

    const uint16_t crc = SMS_Crc(out, SMS_FRAME_BYTES - SMS_CRC_BYTES);

    out[SMS_FRAME_BYTES - 2] = (uint8_t)(crc & 0xFFu);
    out[SMS_FRAME_BYTES - 1] = (uint8_t)(crc >> 8);
}

bool SMS_FrameDecode(const uint8_t *in, SMS_Frame_t *frame)
{
    if (in == NULL || frame == NULL)
        return false;

    const uint16_t want = (uint16_t)(in[SMS_FRAME_BYTES - 2] |
                                    ((uint16_t)in[SMS_FRAME_BYTES - 1] << 8));

    if (SMS_Crc(in, SMS_FRAME_BYTES - SMS_CRC_BYTES) != want)
        return false;

    frame->type   = (uint8_t)((in[OFF_FLAGS] >> 6) & 3u);
    frame->frags  = (uint8_t)((in[OFF_FLAGS] >> 4) & 3u);
    frame->frag   = (uint8_t)((in[OFF_FLAGS] >> 2) & 3u);
    frame->src    = (uint16_t)(in[OFF_SRC] | ((uint16_t)in[OFF_SRC + 1] << 8));
    frame->dst    = (uint16_t)(in[OFF_DST] | ((uint16_t)in[OFF_DST + 1] << 8));
    frame->msg_id = in[OFF_MSGID];
    frame->len    = in[OFF_LEN];

    /* Range checks matter as much as the CRC. One frame in 65536 passes a 16 bit
     * CRC by chance, and one claiming four fragments or a 200 byte payload would
     * corrupt the reassembly state rather than merely being wrong. */
    if (frame->len > SMS_PAYLOAD_BYTES)
        return false;

    if (frame->frags == 0 || frame->frags > SMS_MAX_FRAGS)
        return false;

    if (frame->frag >= frame->frags)
        return false;

    if (frame->type != SMS_TYPE_DATA && frame->type != SMS_TYPE_ACK)
        return false;

    for (uint8_t i = 0; i < SMS_PAYLOAD_BYTES; i++)
        frame->payload[i] = in[OFF_PAYLOAD + i];

    return true;
}

// --- sending ---------------------------------------------------------------

bool SMS_TxBegin(SMS_Tx_t *tx, uint16_t dst, uint8_t msg_id, const char *text)
{
    const uint32_t len = StrLen(text);

    if (tx == NULL || len == 0 || len > SMS_MAX_CHARS)
        return false;

    tx->dst     = dst;
    tx->msg_id  = msg_id;
    tx->len     = (uint8_t)len;
    tx->frags   = (uint8_t)((len + SMS_TEXT_PER_FRAG - 1) / SMS_TEXT_PER_FRAG);
    tx->pending = (uint8_t)((1u << tx->frags) - 1u);
    tx->attempt = 0;

    for (uint32_t i = 0; i < len; i++)
        tx->text[i] = text[i];

    tx->text[len] = '\0';

    return true;
}

bool SMS_TxNext(const SMS_Tx_t *tx, uint16_t src, SMS_Frame_t *frame)
{
    if (tx == NULL || frame == NULL || tx->pending == 0)
        return false;

    uint8_t f = 0;

    while (f < tx->frags && (tx->pending & (1u << f)) == 0)
        f++;

    if (f >= tx->frags)
        return false;

    const uint32_t off  = (uint32_t)f * SMS_TEXT_PER_FRAG;
    uint32_t       take = tx->len - off;

    if (take > SMS_TEXT_PER_FRAG)
        take = SMS_TEXT_PER_FRAG;

    frame->type   = SMS_TYPE_DATA;
    frame->frag   = f;
    frame->frags  = tx->frags;
    frame->src    = src;
    frame->dst    = tx->dst;
    frame->msg_id = tx->msg_id;
    frame->len    = (uint8_t)take;

    for (uint32_t i = 0; i < SMS_PAYLOAD_BYTES; i++)
        frame->payload[i] = (i < take) ? (uint8_t)tx->text[off + i] : 0u;

    return true;
}

void SMS_TxOnAck(SMS_Tx_t *tx, const SMS_Frame_t *ack)
{
    if (tx == NULL || ack == NULL || ack->type != SMS_TYPE_ACK)
        return;

    /* Ignore an acknowledgement for a different message or from the wrong
     * station. A stale ACK arriving late would otherwise mark the current
     * message delivered when it was not. */
    if (ack->msg_id != tx->msg_id || ack->src != tx->dst)
        return;

    // Byte 0 is the bitmap of what the receiver holds; clear those.
    tx->pending = (uint8_t)(tx->pending & ~ack->payload[0]);
}

bool SMS_TxComplete(const SMS_Tx_t *tx)
{
    return tx != NULL && tx->pending == 0;
}

bool SMS_TxFailed(const SMS_Tx_t *tx)
{
    return tx != NULL && tx->pending != 0 && tx->attempt >= SMS_MAX_ATTEMPTS;
}

void SMS_TxRetry(SMS_Tx_t *tx)
{
    if (tx != NULL && tx->attempt < 0xFFu)
        tx->attempt++;
}

// --- receiving -------------------------------------------------------------

void SMS_RxInit(SMS_Rx_t *rx)
{
    if (rx == NULL)
        return;

    rx->src     = 0;
    rx->msg_id  = 0;
    rx->frags   = 0;
    rx->have    = 0;
    rx->len     = 0;
    rx->active  = false;
    rx->text[0] = '\0';
}

bool SMS_RxOnData(SMS_Rx_t *rx, const SMS_Frame_t *data, uint16_t self,
                  SMS_Frame_t *ack)
{
    if (rx == NULL || data == NULL || data->type != SMS_TYPE_DATA)
        return false;

    if (data->dst != self && data->dst != SMS_ADDR_BROADCAST)
        return false;

    // A different sender or message id starts a new reassembly. Keeping only one
    // in flight is what keeps this small; two stations sending at once is a
    // collision the retries already handle.
    if (!rx->active || rx->src != data->src || rx->msg_id != data->msg_id) {
        SMS_RxInit(rx);

        rx->src    = data->src;
        rx->msg_id = data->msg_id;
        rx->frags  = data->frags;
        rx->active = true;

        for (uint32_t i = 0; i < sizeof rx->text; i++)
            rx->text[i] = '\0';
    }

    const uint32_t off = (uint32_t)data->frag * SMS_TEXT_PER_FRAG;

    for (uint8_t i = 0; i < data->len; i++)
        rx->text[off + i] = (char)data->payload[i];

    rx->have = (uint8_t)(rx->have | (1u << data->frag));

    // The last fragment is the one that fixes the length; the others are full.
    if (data->frag + 1u == data->frags)
        rx->len = (uint8_t)(off + data->len);

    const uint8_t all = (uint8_t)((1u << rx->frags) - 1u);

    /* Acknowledge whatever is held, complete or not. A partial ACK is what tells
     * the sender which fragments to repeat, and answering a duplicate matters
     * too: a lost ACK looks exactly like a lost fragment from the far end, and
     * the cure for both is to say again what we have. */
    if (ack != NULL) {
        ack->type       = SMS_TYPE_ACK;
        ack->frag       = 0;
        ack->frags      = 1;
        ack->src        = self;
        ack->dst        = data->src;
        ack->msg_id     = data->msg_id;
        ack->len        = 1;
        ack->payload[0] = rx->have;

        for (uint8_t i = 1; i < SMS_PAYLOAD_BYTES; i++)
            ack->payload[i] = 0;
    }

    if (rx->have != all)
        return false;

    rx->text[rx->len] = '\0';

    return true;
}
