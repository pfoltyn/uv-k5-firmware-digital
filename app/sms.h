/* Short messages over the BK4819's FSK modem, with delivery confirmation.
 *
 * This is the protocol layer: framing, the CRC, splitting a message into
 * fragments, and the retransmission logic on both sides. It touches no hardware
 * and is tested on the host.
 *
 * Design notes worth reading before changing any of the constants, because each
 * one was chosen against something measured on this radio.
 *
 * FIXED FRAME SIZE. Every frame is exactly SMS_FRAME_BYTES, padded. POCSAG and
 * APRS both program the modem a maximum length and work out where the
 * transmission ended from a stall or a carrier drop, and that is what strands
 * the tail: the receive FIFO raises almost-full at four words with no count
 * available, so a final partial chunk of fewer than eight bytes can never be
 * read out. A frame that is a whole number of chunks avoids it entirely - the
 * chip's RX-finished interrupt fires with everything delivered.
 *
 * SMALL FRAMES, AND FRAGMENTS. 64 bytes is 213ms at 2400 baud. Short frames
 * matter twice over: a bit error destroys less, and a retransmission costs less.
 * With a per-fragment acknowledgement only the missing pieces go again.
 *
 * ARQ RATHER THAN FORWARD ERROR CORRECTION, which is the significant choice. A
 * Viterbi decoder is 2 to 3kB of flash and halves throughput permanently; a
 * Reed-Solomon decoder is similar with tables on top. Against that, resending a
 * 64 byte frame costs 213ms. FEC earns its place when the round trip is
 * expensive - satellites, one-way paging - and this is a handheld link where
 * asking again is cheap. The CRC detects, the ACK confirms, the retry repairs.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifndef SMS_H
#define SMS_H

#include <stdbool.h>
#include <stdint.h>

/* 64 bytes: a whole number of the modem's 8-byte FIFO chunks, even as the TX
 * FIFO needs, and well inside the 256 byte single-load limit. */
#define SMS_FRAME_BYTES      64
#define SMS_HEADER_BYTES     8
#define SMS_CRC_BYTES        2
#define SMS_PAYLOAD_BYTES    (SMS_FRAME_BYTES - SMS_HEADER_BYTES - SMS_CRC_BYTES)

/* Text bytes per fragment. Sealing adds a nonce and a tag, which live inside the
 * payload rather than extending the frame, so encryption costs message length
 * rather than air time: 42 characters per fragment instead of 54.
 *
 * This layer stays crypto-agnostic otherwise - it deals in payload bytes and never
 * calls the cipher. Sealing happens in sms_link.c around the wire format, which is
 * the only place that knows a frame is about to become radio. */
#ifdef ENABLE_SMS_CRYPTO
    #include "app/sms_crypto.h"
    #define SMS_TEXT_PER_FRAG    (SMS_PAYLOAD_BYTES - SMS_CRYPTO_OVERHEAD)
#else
    #define SMS_TEXT_PER_FRAG    SMS_PAYLOAD_BYTES
#endif

// Three fragments is 162 characters, longer than an SMS, and a 3-bit bitmap.
#define SMS_MAX_FRAGS        3
#define SMS_MAX_CHARS        (SMS_TEXT_PER_FRAG * SMS_MAX_FRAGS)

// How many times a fragment is sent before the message is given up on.
#define SMS_MAX_ATTEMPTS     4

// Reserved: a frame addressed here is accepted by every station, and no station
// may use it as its own address.
#define SMS_ADDR_BROADCAST   0xFFFFu

enum SMS_Type_t {
    SMS_TYPE_DATA = 0,
    SMS_TYPE_ACK  = 1
};
typedef enum SMS_Type_t SMS_Type_t;

struct SMS_Frame_t {
    uint8_t  type;       // SMS_Type_t
    uint8_t  frag;       // 0 based
    uint8_t  frags;      // how many the message was split into
    uint16_t src;
    uint16_t dst;
    uint8_t  msg_id;     // wraps; distinguishes a retry from a new message
    uint8_t  len;        // payload bytes used, 0..SMS_PAYLOAD_BYTES

    /* On a DATA frame this is message text. On an ACK, byte 0 is a bitmap of the
     * fragments the receiver holds, which is what lets the sender resend only
     * what is missing rather than the whole message. */
    uint8_t  payload[SMS_PAYLOAD_BYTES];
};
typedef struct SMS_Frame_t SMS_Frame_t;

// CRC-16/CCITT-FALSE over the header and payload. Detection only: correction is
// the retry's job.
uint16_t SMS_Crc(const uint8_t *data, uint32_t len);

// Serialises to exactly SMS_FRAME_BYTES, zero padding the unused payload so the
// same message always produces the same frame.
void SMS_FrameEncode(const SMS_Frame_t *frame, uint8_t *out);

// Returns false if the CRC fails or a field is out of range. An out of range
// field matters as much as the CRC: a frame that passes the CRC by chance but
// claims nine fragments would otherwise corrupt the reassembly state.
bool SMS_FrameDecode(const uint8_t *in, SMS_Frame_t *frame);

// --- sending ---------------------------------------------------------------

struct SMS_Tx_t {
    uint16_t dst;
    uint8_t  msg_id;
    uint8_t  frags;
    uint8_t  pending;    // bitmap: fragments still unacknowledged
    uint8_t  attempt;    // of SMS_MAX_ATTEMPTS
    uint8_t  len;
    char     text[SMS_MAX_CHARS + 1];
};
typedef struct SMS_Tx_t SMS_Tx_t;

// Prepares a message. Returns false if the text is empty or too long.
bool SMS_TxBegin(SMS_Tx_t *tx, uint16_t dst, uint8_t msg_id, const char *text);

// Fills the next fragment still waiting for acknowledgement. Returns false when
// every fragment has been acknowledged.
bool SMS_TxNext(const SMS_Tx_t *tx, uint16_t src, SMS_Frame_t *frame);

// Applies an acknowledgement. Ignores one for a different message or sender, so
// a stale ACK cannot mark the wrong thing delivered.
void SMS_TxOnAck(SMS_Tx_t *tx, const SMS_Frame_t *ack);

bool SMS_TxComplete(const SMS_Tx_t *tx);

// True once the fragments still outstanding have each been sent SMS_MAX_ATTEMPTS
// times, at which point the message has failed and the operator should be told.
bool SMS_TxFailed(const SMS_Tx_t *tx);

// Counts one round of sending everything outstanding.
void SMS_TxRetry(SMS_Tx_t *tx);

// --- receiving -------------------------------------------------------------

struct SMS_Rx_t {
    uint16_t src;
    uint8_t  msg_id;
    uint8_t  frags;
    uint8_t  have;       // bitmap of fragments reassembled so far
    uint8_t  len;
    bool     active;
    char     text[SMS_MAX_CHARS + 1];
};
typedef struct SMS_Rx_t SMS_Rx_t;

void SMS_RxInit(SMS_Rx_t *rx);

/* Takes a decoded DATA frame addressed to us and reassembles it, writing the
 * acknowledgement to send back. Returns true when the message is complete.
 *
 * The ACK is built whether or not the message is complete, because a partial
 * acknowledgement is what tells the sender which fragments to repeat. It is also
 * built for a duplicate, since a lost ACK looks exactly like a lost fragment
 * from the sender's side and the cure for both is to say again what is held.
 */
bool SMS_RxOnData(SMS_Rx_t *rx, const SMS_Frame_t *data, uint16_t self,
                  SMS_Frame_t *ack);

#endif
