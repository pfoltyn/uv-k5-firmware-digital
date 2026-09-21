/* APRS receive: hunts for AX.25 frames with the BK4819's FSK demodulator.
 *
 * This is the experimental half of the feature, and the one part of it that is
 * not known to work. Two things about the chip stand in the way, and the reason
 * they are attacked with a probe rather than settled from the datasheet is that
 * the POCSAG receive work found this hardware contradicting its register list
 * twice.
 *
 * The demodulator has no Bell 202 mode. It offers plain FSK, FFSK 1200/1800 and
 * FFSK 1200/2400, and APRS is 1200/2200. Every one of them should still put 2200
 * on the correct side of its decision threshold, but which does it best is a
 * measurement, so all three are selectable.
 *
 * The preamble detector will not hunt for a sync word until it has seen whole
 * bytes of alternating bits, and a standards-compliant APRS transmission opens
 * with flags, whose NRZI form is 00000001 repeating. Our own transmit path
 * prepends an alternating run for exactly this reason (AX25_LEAD_ALT), so radio
 * to radio should work. Receiving other people's traffic depends on whether the
 * chip's sync hunt persists once armed, which noise on a quiet channel should do
 * roughly nine times a second, and that is the open question.
 *
 * The sync word is 0x01010101, not 0x7E7E7E7E: see ax25.h. Matching it lands the
 * FIFO on an HDLC byte boundary, so what arrives is the frame from its first
 * byte, addresses included and FCS checkable.
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

#ifndef APRS_RX_H
#define APRS_RX_H

#include <stdbool.h>
#include <stdint.h>

#include "app/ax25.h"

// Which demodulator to use. All three are wrong for Bell 202 in different ways,
// which is why the screen can cycle them.
enum APRS_RxMode_t {
    /* FFSK1800 first because the probe measured it perfect, with the narrow
     * filter, against a 1200/2400 flag stream: 32 of 32 bytes correct where
     * FFSK2400 managed 9 at best. That is the reverse of what the tone pairs
     * suggest and no mechanism for it has been established - see gModeNarrow in
     * aprs_rx.c. */
    APRS_RX_FFSK1800 = 0,
    APRS_RX_FFSK2400,
    APRS_RX_FSK12K,         // what POCSAG receives on, included as a control

    /* Direct 2-level FSK at 2400 baud: the same demodulator as FSK12K but with
     * REG_72 set for twice the rate. Pairs with APRS_TONE_FSK2400, and is the one
     * remaining route to more throughput now that the measured audio filter has
     * capped AFSK at 1200 baud. */
    APRS_RX_FSK2400,

    /* And 4800, which is where the FIFO starts to bite rather than the modem: at
     * 4800 baud a byte is 1.67ms, so the eight byte almost-full threshold leaves
     * about 13ms before overflow against a 10ms poll. REG_72's word does not
     * overflow until about 6347 baud, so this is the last doubling available. */
    APRS_RX_FSK4800,

    APRS_RX_PROBE,          // sweep every combination; see APRS_RX_PROBE_STEPS
    APRS_RX_MODE_COUNT
};

/* The probe.
 *
 * Two rounds of guessing one variable at a time cost a flash and a test each and
 * were both half right: 2200Hz gave a wrong first byte, coherent 2400 gave a
 * correct first byte and then noise. That is the point to stop reasoning and
 * start measuring, which is exactly what settled POCSAG receive.
 *
 * So this sweeps all three demodulators against both filter bandwidths, scoring
 * each on how many bytes came back as 0x01 - the value every lead flag has on air,
 * and therefore the only ground truth available. Captures are short so one long
 * transmission covers every combination rather than needing six.
 */
#define APRS_RX_PROBE_BYTES   32u
#define APRS_RX_PROBE_STEPS   6u    // 3 demodulators x 2 bandwidths
typedef enum APRS_RxMode_t APRS_RxMode_t;

enum APRS_RxEvent_t {
    APRS_RX_EVENT_NONE = 0,
    APRS_RX_EVENT_FRAME       // a frame arrived and its FCS checks out
};
typedef enum APRS_RxEvent_t APRS_RxEvent_t;

// Counters, which are the whole point during bring-up. A sync count that moves
// while the frame count does not says the preamble detector and sync word are
// right and something after them is wrong, which is a completely different
// problem from nothing happening at all.
struct APRS_RxStats_t {
    uint16_t syncs;      // sync word matches
    uint16_t bytes;      // bytes clocked out of the FIFO
    uint16_t frames;     // frames whose FCS passed
    uint16_t flushes;    // captures examined, whether they decoded or not
    uint16_t rssi;       // raw REG_67 at the last sync; dBm = value/2 - 160

    /* The first bytes of the last capture, and how many of the first twelve were
     * exactly 0x01.
     *
     * This is the bit error rate, measured against a pattern whose value is known
     * in advance. Sync consumes two of the sixteen lead flags, so what the FIFO
     * delivers first is the rest of them, and every one is 0x01 on air. Twelve out
     * of twelve means the demodulator is clean and the fault is further on; nine
     * or ten means occasional bit errors, which is enough to destroy every frame
     * because AX.25 has no error correction; near zero means the wrong
     * demodulator entirely.
     */
    uint8_t  good_flags;
    uint8_t  first[6];

    // Probe results: best 0x01 count seen per step, index bandwidth*3 + mode.
    uint8_t  probe[APRS_RX_PROBE_STEPS];
    uint8_t  probe_step;
};
typedef struct APRS_RxStats_t APRS_RxStats_t;

void APRS_RX_Start(uint32_t frequency, APRS_RxMode_t mode);
void APRS_RX_Stop(void);

// Call about every 10ms. Drains the FIFO and reports when a frame completes.
APRS_RxEvent_t APRS_RX_Poll(void);

// True between a sync match and the end of the capture that followed it.
bool APRS_RX_InSync(void);

// The last frame whose FCS passed. Valid once Poll has returned FRAME.
const AX25_Decoded_t *APRS_RX_Frame(void);

void APRS_RX_GetStats(APRS_RxStats_t *stats);

// Name of a mode, for the screen.
const char *APRS_RX_ModeName(APRS_RxMode_t mode);

#endif
