/* APRS transmit: builds a frame and puts it on air as Bell 202 AFSK.
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

#ifndef APRS_TX_H
#define APRS_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "app/ax25.h"

/* REG_70<14:8>, the TONE1 gain, which for this path is the FM deviation: the
 * tone is what modulates the carrier, so its amplitude sets the deviation and
 * REG_40 has nothing to do with it. 66 is what the stock tone code uses and is a
 * starting point, not a calibrated value. APRS wants about 3kHz, and
 * APRS_TX_Tone exists so it can be measured. */
#define APRS_TX_GAIN         66u

// 144.800 in Europe, 144.390 in North America, 145.825 for the ISS digipeater.
#define APRS_TX_FREQ_EU      14480000u   // 10Hz units, as the radio stores them
#define APRS_TX_FREQ_NA      14439000u
#define APRS_TX_FREQ_ISS     14582500u

enum APRS_TxResult_t {
    APRS_TX_OK = 0,
    APRS_TX_ERR_CALL,            // a callsign is not 1..6 of A-Z 0-9
    APRS_TX_ERR_TOO_LONG,        // the payload or the air stream would not fit
    APRS_TX_ERR_ENCODE,
    APRS_TX_ERR_TX_NOT_ALLOWED,  // outside a band this radio may transmit in
    APRS_TX_ERR_MODEM            // the hardware FSK modem refused the config
};
typedef enum APRS_TxResult_t APRS_TxResult_t;

// What to emit for bench measurement. ALTERNATING is a 1010 pattern at the bit
// rate, which shows up as two tones and a toggle rate, so
// tools/pocsag/sdr_measure.py reads the tone pair and the deviation from it
// without being told anything about the modulation.
enum APRS_ToneTest_t {
    APRS_TONE_MARK = 0,
    APRS_TONE_SPACE,
    APRS_TONE_ALTERNATING,

    /* An alternating preamble followed by nothing but flags, for as long as the
     * buffer holds, which is about 1.3 seconds.
     *
     * This is what the receive probe listens to. Every byte of it has a known
     * value on air - 0xAA then 0x01 - so a receiver can score itself against it
     * without needing a frame to decode, and it lasts long enough for one
     * transmission to cover every combination the probe sweeps. */
    APRS_TONE_FLAGS,

    /* Direct 2-level FSK at 2400 baud through the chip's hardware modem, which
     * shifts the carrier itself and never touches the audio path.
     *
     * This is the one route left to more throughput. The measured audio filter caps
     * AFSK at 1200 baud - see bk4819-afsk.h - but it cannot touch direct FSK, which
     * is how POCSAG works on this radio at 1200 baud in both directions. The
     * register list says REG_58 mode 000 covers "FSK 1.2K and FSK 2.4K", and
     * REG_72's word does not overflow until about 6347 baud.
     *
     * Sends a known repeating byte so a receiver in APRS_RX_FSK2400 can score
     * itself on the G counter without a frame being involved.
     */
    APRS_TONE_FSK2400,
    APRS_TONE_FSK4800
};
typedef enum APRS_ToneTest_t APRS_ToneTest_t;

struct APRS_TxStats_t {
    uint32_t frame_bytes;   // before stuffing
    uint32_t air_bits;
    uint32_t ms;            // how long the transmission should have taken
};
typedef struct APRS_TxStats_t APRS_TxStats_t;

// Keys the transmitter, sends one frame, and unkeys. Blocks for the duration,
// which for a beacon is about three quarters of a second.
// alt_bytes is the alternating preamble ahead of the flags: AX25_LEAD_ALT for
// anything the BK4819's own receiver should hear, or 0 to transmit exactly what a
// standard TNC would. Sending 0 is how the preamble question gets answered without
// waiting for a real station to come up.
APRS_TxResult_t APRS_TX_SendFrame(uint32_t frequency, uint8_t pa_bias,
                                  const AX25_Frame_t *frame, uint8_t alt_bytes,
                                  APRS_TxStats_t *stats);

// A position or status beacon, which is the common case.
APRS_TxResult_t APRS_TX_SendPayload(uint32_t frequency, uint8_t pa_bias,
                                    const AX25_Addr_t *src,
                                    const char *path,
                                    const char *info,
                                    uint8_t alt_bytes,
                                    APRS_TxStats_t *stats);

// Emits a test signal for the stated number of milliseconds. Transmit into a
// dummy load: this is not a legal on-air signal, it is a measurement.
APRS_TxResult_t APRS_TX_Tone(uint32_t frequency, uint8_t pa_bias,
                             APRS_ToneTest_t test, uint32_t ms, uint8_t gain);

// Parses "WIDE1-1,WIDE2-1" into a frame's digipeater list. An empty or NULL path
// means no digipeaters. Returns false on a malformed or over-long path.
bool APRS_TX_ParsePath(const char *path, AX25_Frame_t *frame);

#endif
