/* Bell 202 AFSK by bit banging the BK4819's tone generator.
 *
 * The chip's own FFSK mode gives a 1200/2400 tone pair, and tools/aprs/tones.sh
 * measured what that costs: stock direwolf decodes 2200 through 2375 every time,
 * 2400 four times in five, and 2425 not at all. About 25Hz of margin, and
 * whether 2400 works at all depends on which demodulator the far end runs. That
 * is not enough for a feature whose point is reaching arbitrary igates.
 *
 * So the tones are generated here instead: TONE1 alone, no FSK modem, with
 * REG_71 rewritten between 1200Hz and 2200Hz at each bit boundary. That is
 * exactly Bell 202, and BK4819_PlayRogerNormal already changes REG_71 in the
 * middle of a transmission, so the mechanism is not new to this hardware.
 *
 * One SPI write per bit costs about 76us of the 833us bit period: every SPI bit
 * in BK4819_WriteU8 and BK4819_WriteU16 carries three 1us delays, so 24 bits
 * come to 72us plus the framing. Nowhere near the budget, and the bit clock
 * absorbs it.
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

#ifndef DRIVER_BK4819_AFSK_H
#define DRIVER_BK4819_AFSK_H

#include <stdbool.h>
#include <stdint.h>

#define BK4819_AFSK_MARK_HZ   1200u    // a 1 bit
#define BK4819_AFSK_SPACE_HZ  2200u    // a 0 bit: Bell 202
#define BK4819_AFSK_BAUD      1200u

/* A coherent space tone, kept only as an experimental alternative.
 *
 * It is NOT needed, and the reasoning that introduced it was wrong. The argument
 * ran: FFSK is phase coherent by definition, its tone pairs being a whole number
 * of half cycles per bit, so Bell 202's 1.833 cycles per bit must defeat a
 * coherent demodulator. It explained the early failures neatly.
 *
 * The probe disproved it. With the FFSK1800 demodulator and the narrow filter, a
 * 2200Hz space scores 32 of 32 bytes correct - exactly the same as 2400. Every
 * earlier failure was in the FFSK2400 mode, which scores 3 to 11 whatever tone it
 * is given. The variable was always the demodulator and the bandwidth; the tone
 * barely matters.
 *
 * So the default is Bell 202 and stays that way: one standards-compliant mode that
 * works in both directions. This constant remains because the SPC row that selects
 * it is how the comparison was made, and it costs almost nothing to keep.
 */
#define BK4819_AFSK_SPACE_FFSK_HZ  2400u

// SysTick runs at the 48MHz core clock, which SYSTICK_Init fixes by configuring
// it for 480000 ticks per 10ms interrupt. 48000000/1200 divides exactly, so a
// bit is a whole number of ticks and the bit clock has no rounding error at all.
#define BK4819_AFSK_TICK_HZ   48000000u
#define BK4819_AFSK_BIT_TICKS (BK4819_AFSK_TICK_HZ / BK4819_AFSK_BAUD)

/* The bit clock.
 *
 * SysTick counts down and reloads, so elapsed time is a subtraction with a wrap
 * case. Ticks measured but not yet spent are carried in credit rather than
 * discarded, which is what stops the error accumulating: a bit whose register
 * write ran long makes the next wait correspondingly shorter, so the hundredth
 * bit boundary is as accurate as the first.
 *
 * Exposed rather than static so the arithmetic can be tested on the host
 * against a simulated counter, wrap included. Timing is the one part of this
 * that cannot be checked by looking at it.
 */
struct BK4819_AfskClock_t {
    uint32_t prev;     // last counter value seen
    uint32_t credit;   // ticks elapsed and not yet consumed
};
typedef struct BK4819_AfskClock_t BK4819_AfskClock_t;

void BK4819_AfskClockInit(BK4819_AfskClock_t *clock);
void BK4819_AfskClockWait(BK4819_AfskClock_t *clock, uint32_t ticks);

// Accumulates elapsed ticks from one counter reading. Split out so the host
// test can drive it without a busy loop. reload is the counter's top value.
void BK4819_AfskClockFeed(BK4819_AfskClock_t *clock, uint32_t value, uint32_t reload);

/* Programs TONE1 at the mark frequency and starts emitting it.
 *
 * The transmitter must already be keyed, and keying must come first: the low 7
 * bits of REG_70 are the TONE2/FSK gain, and the POCSAG transmit path found that
 * its keying sequence clears REG_70, so programming the tone before keying
 * leaves an unmodulated carrier.
 *
 * gain is REG_70<14:8>, 0 to 127. The stock tone code uses 66.
 */
void BK4819_AfskStart(uint8_t gain);

// Chooses the space tone for everything sent afterwards. Bell 202 is 2200;
// BK4819_AFSK_SPACE_FFSK_HZ is what the chip can receive. Out of range values are
// ignored, so a bad call cannot put an arbitrary tone on air.
void BK4819_AfskSetSpaceTone(uint32_t hz);
uint32_t BK4819_AfskSpaceTone(void);

/* MEASURED: this radio's transmit audio path is a steep low pass with its corner
 * just above 2200Hz, which caps AFSK at 1200 baud and is not negotiable.
 *
 * Deviation produced by a steady tone, RTL-SDR into a dummy load:
 *
 *     1200Hz   2437Hz     0 dB
 *     2200Hz   2201Hz    -0.9 dB
 *     2400Hz   1205Hz    -6.1 dB
 *     3600Hz    165Hz   -23.4 dB
 *
 * A splatter filter doing its job. Bell 202's 2200Hz space sits just inside the
 * corner, which is luck rather than design; 2400 is already half amplitude and
 * 3600 is not transmitted at all.
 *
 * A selectable 2400 baud mode was built to test this and then reverted, because
 * the probe scored zero in all six demodulator and bandwidth combinations and the
 * numbers above say why: the space tone was absent, so the demodulator had nothing
 * to fail at. Keeping a setting whose only effect is silence was not worth the
 * flash. It also explains why the deviation is slightly tone dependent even at
 * 1200 baud - 2200 is already on the filter's shoulder.
 *
 * Anything faster has to leave AFSK behind. Direct 2-level FSK shifts the carrier
 * itself with no audio tones, bypassing this filter entirely, which is the path
 * POCSAG uses and the register list says covers FSK 2.4K. Untested above 1200.
 */
// Sends air bits, most significant bit first within each byte, which is the
// order AX25_Bits_t packs them. A 1 is mark and a 0 is space. The first bit's
// tone is written unconditionally, so this is safe to call repeatedly and does
// not assume BK4819_AfskStart has just run.
void BK4819_AfskSendBits(const uint8_t *air, uint32_t nbits);

// A steady tone for a whole number of bit periods. For bench measurement: this
// is what tools/pocsag/sdr_measure.py needs to read the tone frequencies and the
// FM deviation, neither of which has been calibrated for this path.
void BK4819_AfskSendTone(bool mark, uint32_t bits);

// Silences the tone generator. Does not unkey the transmitter.
void BK4819_AfskStop(void);

#endif
