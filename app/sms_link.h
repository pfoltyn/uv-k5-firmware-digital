/* The radio side of SMS: keying, turnaround and the retry clock.
 *
 * app/sms.c owns the protocol and knows nothing of hardware; this owns the chip
 * and knows nothing of message content. It drives the BK4819's hardware FSK
 * modem directly rather than through audio tones, which is what allows 2400 or
 * 4800 baud: the radio's transmit audio filter rolls off just above 2200Hz and
 * caps any AFSK mode at 1200, but direct FSK shifts the carrier itself and never
 * touches that path. Both rates were measured working K5 to K5.
 *
 * Every setting here was measured rather than inferred, and several cost a bench
 * round on the APRS side before they were right:
 *
 *   REG_58 receive    the bandwidth field follows the bit rate. Left at the
 *                     1.2K setting, 2400 baud works with almost no margin and
 *                     4800 will not lock at all. The driver derives it now.
 *   REG_59<10>        set: this chip hands back inverted data. Invisible in the
 *                     APRS path because NRZI codes transitions, unmissable here.
 *   sync word         32 bits. A 16 bit word of low density is matched by noise
 *                     constantly once a raw discriminator is being sliced - the
 *                     first direct-FSK attempt spent its time capturing noise.
 *   deviation, gain   POCSAG's measured values, since that path is proven here.
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

#ifndef SMS_LINK_H
#define SMS_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "app/sms.h"

/* CCSDS's attached sync marker, borrowed for its autocorrelation rather than its
 * meaning: 32 bits, balanced, and designed to be found in noise by spacecraft
 * receivers. Distinct from POCSAG's codeword so the two features cannot trigger
 * each other if both are ever on the air at once. */
#define SMS_SYNC_WORD        0x1ACFFC1Du

// 2400 works and 4800 works; 2400 is the default because it occupies half the
// bandwidth and a message is short either way.
#define SMS_BAUD_DEFAULT     2400u

/* How long to wait for an acknowledgement, in 10ms poll ticks.
 *
 * A 64 byte frame plus the modem's preamble and sync is about 253ms at 2400 baud,
 * and the far end has to turn its radio round before answering. 600ms covers that
 * with room for the keying delays at both ends.
 */
#define SMS_ACK_TICKS        60u

enum SMS_LinkEvent_t {
    SMS_LINK_EVENT_NONE = 0,
    SMS_LINK_EVENT_MESSAGE,   // a complete message arrived; see SMS_Link_Inbox
    SMS_LINK_EVENT_SENT,      // the message we were sending was acknowledged
    SMS_LINK_EVENT_FAILED     // it was not, after SMS_MAX_ATTEMPTS
};
typedef enum SMS_LinkEvent_t SMS_LinkEvent_t;

/* Counters, so a failure can be diagnosed rather than merely observed.
 *
 * Leaving these out was a mistake this project has now made three times. POCSAG and
 * APRS both needed them added mid-bring-up before anything could be worked out, and
 * SMS shipped without them and immediately needed them too. Which number moves says
 * where the fault is:
 *
 *   sent climbs, rx_ok stays 0    the far end hears nothing, or we hear nothing back
 *   crc_bad climbs               frames arrive corrupt: signal, rate or polarity
 *   tag_bad climbs               frames arrive intact but the passphrases differ
 *   rx_ok climbs, acks stays 0   data gets through but acknowledgements do not
 *   addr_bad climbs              frames arrive perfectly and are for someone else,
 *                                which is the sender's TO not matching this OWN.
 *                                last_dst says what they were addressed to, so the
 *                                two together name the mistake outright
 */
struct SMS_LinkStats_t {
    uint16_t sent;       // frames transmitted
    uint16_t rx_ok;      // frames accepted
    uint16_t crc_bad;    // read out of the FIFO but the CRC failed
    uint16_t tag_bad;    // CRC passed and the authentication tag did not
    uint16_t acks;       // acknowledgements applied
    uint16_t addr_bad;   // accepted, and addressed to some other station
    uint16_t last_dst;   // the destination of the last frame accepted
    uint16_t last_src;   // the station it came from, whoever it was addressed to
};
typedef struct SMS_LinkStats_t SMS_LinkStats_t;

void SMS_Link_GetStats(SMS_LinkStats_t *stats);

// A station address derived from SYSCON_CHIP_ID, so two radios differ without
// being configured. Never 0 or SMS_ADDR_BROADCAST.
uint16_t SMS_Link_DefaultAddress(void);

void SMS_Link_Start(uint32_t frequency, uint32_t baud, uint16_t self);
void SMS_Link_Stop(void);

// Begins sending. Returns false if one is already in flight or the text will not
// fit. Delivery is reported later by Poll.
bool SMS_Link_Send(uint16_t dst, const char *text);

// Call about every 10ms. Drives the modem, the retries and the acknowledgements.
SMS_LinkEvent_t SMS_Link_Poll(void);

const SMS_Rx_t *SMS_Link_Inbox(void);

// For the screen: how many fragments of the message in flight are acknowledged,
// out of how many, and which attempt it is on.
void SMS_Link_Progress(uint8_t *done, uint8_t *total, uint8_t *attempt);

bool SMS_Link_Busy(void);

#endif
