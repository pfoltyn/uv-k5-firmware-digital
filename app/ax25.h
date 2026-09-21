/* AX.25 UI frames, and the HDLC bit stream that carries them.
 *
 * This is the layer between an APRS text payload and the BK4819's FSK modem.
 * The modem gives us a byte pipe with a programmable preamble and sync word; it
 * knows nothing of HDLC, so flags, bit stuffing, NRZI and the frame check
 * sequence are all done here in software.
 *
 * Three bit orders meet in this file and it is worth being explicit about them,
 * because getting any of them wrong produces a stream that looks plausible and
 * decodes as nothing:
 *
 *   frame bytes   AX.25 transmits each byte least significant bit first
 *   air bits      NRZI, so a data 0 toggles the level and a data 1 holds it
 *   air bytes     the BK4819 serialises each FIFO byte most significant bit
 *                 first, proved by POCSAG's 0x7CD215D8 sync word working
 *
 * So the first bit on air lands in bit 7 of air byte 0, which is the same
 * packing POCSAG uses. The two features never build together, and sharing a bit
 * buffer would mean refactoring shipped POCSAG code for a coexistence that
 * cannot happen, so the buffer here is deliberately a separate small one.
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

#ifndef AX25_H
#define AX25_H

#include <stdbool.h>
#include <stdint.h>

#define AX25_FLAG            0x7Eu   // 01111110, and a bit palindrome
#define AX25_CONTROL_UI      0x03u   // unnumbered information, no poll
#define AX25_PID_NO_LAYER3   0xF0u   // what APRS always uses

#define AX25_CALL_CHARS      6
#define AX25_MAX_SSID        15
#define AX25_MAX_DIGIS       8       // what AX.25 allows, so no real frame is refused
#define AX25_ADDR_BYTES      7

/* What APRS allows, rather than what this radio generates, which is at most a
 * message: ':' + a 9 character addressee + ':' + 67 characters of text + a 5
 * character sequence suffix.
 *
 * Sized for receive. The parser rejects anything longer than this, so a real
 * station with a long payload would be silently ignored, and on a live channel
 * that is a noticeable fraction of traffic. There is no transmit cost to the
 * larger figure: the bit banged path streams from RAM and has no FIFO limit.
 */
#define AX25_MAX_INFO        256

// dest + src + digis + control + PID + info + FCS
#define AX25_MAX_FRAME       (AX25_ADDR_BYTES * (2 + AX25_MAX_DIGIS) + 2 + AX25_MAX_INFO + 2)

// Worst case on air: every frame byte stuffed at one bit in five, plus the
// flags either side. Rounded up generously; AX25_ToAir reports overflow anyway.
#define AX25_LEAD_FLAGS      16u     // see AX25_ToAir on why this is not TXDELAY

/* Bytes of alternating air bits sent ahead of the flags.
 *
 * Nothing in AX.25 asks for this. It is there so the BK4819's own receiver can
 * hear us: the chip will not hunt for a sync word until it has seen whole bytes
 * of 0xAA or 0x55, and an APRS transmission otherwise opens with flags, whose
 * NRZI form is 00000001 repeating and not alternating at all.
 *
 * It costs nothing in compatibility. HDLC receivers synchronise on flags and
 * discard whatever preceded them, so a standard TNC skips this and decodes the
 * frame as normal - which the direwolf round trips in tools/aprs confirm.
 *
 * It must be 0xAA and not 0x55. The first air bit of a flag is a 0, so a
 * preamble ending on 1 would extend the alternation by one more bit and put the
 * sync word one bit late; 0xAA ends on 0 and breaks the alternation exactly at
 * the flag boundary. TestPreambleAlignsWithTheSyncWord holds this.
 */
#define AX25_LEAD_ALT        8u

/* Flags sent after the closing one.
 *
 * Nothing in AX.25 needs these either; they exist because of how the BK4819's
 * receive FIFO works. It raises its almost-full interrupt at four words and gives
 * no count, so the driver reads exactly eight bytes per event and a final partial
 * chunk has no way of being read at all. On top of that, up to a FIFO's worth can
 * still be sitting there when the carrier drops and the capture is closed.
 *
 * Between them that strands the last few bytes of any transmission - which is
 * exactly where the FCS is, so every frame fails however clean the bits were. The
 * first receive with a working demodulator showed it precisely: twelve of twelve
 * lead flags correct, and ten bytes short of the frame.
 *
 * Sixteen bytes of trailing flags means what gets stranded is padding. A receiver
 * discards them, as it does the lead flags, so the cost is 107ms of air time.
 */
#define AX25_TAIL_FLAGS      16u

#define AX25_MAX_AIR_BYTES   ((AX25_MAX_FRAME * 8u * 6u / 5u) / 8u                \
                              + AX25_LEAD_ALT + AX25_LEAD_FLAGS                   \
                              + AX25_TAIL_FLAGS + 8u)

// The level NRZI starts from. 1 makes a run of flags come out as the air byte
// 0x01 repeating, which is also the sync word the receive path looks for.
#define AX25_NRZI_INITIAL    1

struct AX25_Addr_t {
    char    call[AX25_CALL_CHARS + 1];   // NUL terminated, 1..6 of A-Z 0-9
    uint8_t ssid;                        // 0..15
    bool    repeated;                    // digipeater H bit, set on receive
};
typedef struct AX25_Addr_t AX25_Addr_t;

struct AX25_Frame_t {
    AX25_Addr_t dest;                    // the APRS tocall
    AX25_Addr_t src;
    AX25_Addr_t digi[AX25_MAX_DIGIS];
    uint8_t     digis;
    const char *info;                    // NUL terminated APRS payload
};
typedef struct AX25_Frame_t AX25_Frame_t;

struct AX25_Decoded_t {
    AX25_Addr_t dest;
    AX25_Addr_t src;
    AX25_Addr_t digi[AX25_MAX_DIGIS];
    uint8_t     digis;
    char        info[AX25_MAX_INFO + 1];
    uint8_t     info_len;
};
typedef struct AX25_Decoded_t AX25_Decoded_t;

// Packed most significant bit first, one entry per 8 air bits.
struct AX25_Bits_t {
    uint8_t  *data;
    uint32_t  capacity_bits;
    uint32_t  length;        // bits written so far
    bool      overflow;
};
typedef struct AX25_Bits_t AX25_Bits_t;

void AX25_BitsInit(AX25_Bits_t *bits, uint8_t *storage, uint32_t capacity_bytes);
bool AX25_BitsGet(const AX25_Bits_t *bits, uint32_t index);

// True for 1 to 6 characters of A-Z or 0-9. Lower case is rejected rather than
// folded, so a mistyped callsign is caught at entry instead of going out wrong.
bool AX25_ValidCall(const char *call);

// CRC-16/X.25: reflected 0x1021, init 0xFFFF, final XOR 0xFFFF. Transmitted low
// byte first, which is what AX25_BuildFrame appends.
uint16_t AX25_Fcs(const uint8_t *data, uint32_t len);

// Assembles addresses, control, PID, info and FCS into out. Returns the length,
// or 0 if any callsign is invalid or the result would not fit.
uint32_t AX25_BuildFrame(const AX25_Frame_t *frame, uint8_t *out, uint32_t max);

// Wraps a frame and writes the air bit stream: alt_bytes of alternating bits for
// the sake of the BK4819's preamble detector, then lead_flags flags, then the
// stuffed and NRZI encoded frame, then at least one closing flag.
//
// lead_flags is not TXDELAY. TXDELAY exists so a transmitter's PLL and the far
// end's AGC have settled before the frame starts, and the modem's own preamble
// already covers that; these flags only have to give the receiver's clock
// recovery something to lock to.
//
// Pads to an even number of air bytes because the FIFO is written a word at a
// time. Bits after the closing flag are ignored by any HDLC receiver, so the
// padding is free.
bool AX25_ToAir(const uint8_t *frame, uint32_t len,
                uint8_t alt_bytes, uint8_t lead_flags, AX25_Bits_t *bits);

// Everything above in one call, for the common case.
bool AX25_Encode(const AX25_Frame_t *frame, uint8_t alt_bytes,
                 uint8_t lead_flags, AX25_Bits_t *bits);

// Finds the first frame in an air bit stream whose FCS checks out. Scans for
// flag pairs, so it does not care where in the stream it starts, how many flags
// separate frames, or whether the whole stream is inverted: NRZI codes
// transitions, so an inversion changes only the first decoded bit.
//
// initial_level is the NRZI level assumed before air bit 0. It only affects
// that first bit, and the flag search absorbs a wrong guess.
bool AX25_FromAir(const uint8_t *air, uint32_t air_bits,
                  bool initial_level, AX25_Decoded_t *out);

// Formats an address as CALL or CALL-SSID into out, which needs 10 bytes.
// Returns the length written.
uint8_t AX25_FormatAddr(const AX25_Addr_t *addr, char *out, uint8_t max);

// Parses CALL or CALL-SSID. Returns false on a malformed callsign or SSID.
bool AX25_ParseAddr(const char *text, AX25_Addr_t *addr);

#endif
