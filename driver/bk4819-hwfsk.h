/* BK4819 hardware FSK modem.
 *
 * The modem's framing is fully programmable: preamble length and pattern, a 2
 * or 4 byte sync word, optional CRC, optional scrambling and a data length of
 * up to 2047 bytes. That is enough to carry formats the modem was never
 * designed for, POCSAG among them, by folding their framing into the payload.
 *
 * It modulates the same synthesiser the bit banged path shifts by hand, but
 * the timing comes from the chip rather than from SPI writes, so there is no
 * jitter, no per-bit CPU cost and no carrier-split restriction.
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

#ifndef DRIVER_BK4819_HWFSK_H
#define DRIVER_BK4819_HWFSK_H

#include <stdbool.h>
#include <stdint.h>

#define BK4819_HWFSK_MAX_PREAMBLE    16u    // REG_59<7:4>, 1 to 16 bytes
#define BK4819_HWFSK_FIFO_WORDS     128u    // REG_5E, TX FIFO depth
#define BK4819_HWFSK_MAX_PAYLOAD    (BK4819_HWFSK_FIFO_WORDS * 2u)

enum BK4819_HwFskStatus_t {
    BK4819_HWFSK_OK = 0,
    BK4819_HWFSK_ERR_BAUD,
    BK4819_HWFSK_ERR_PREAMBLE,    // outside 1..16 bytes
    BK4819_HWFSK_ERR_SYNC_LEN,    // not 2 or 4 bytes
    BK4819_HWFSK_ERR_LENGTH,      // payload empty, odd, or past what one FIFO holds
    BK4819_HWFSK_ERR_TIMEOUT      // the chip never reported TX finished
};
typedef enum BK4819_HwFskStatus_t BK4819_HwFskStatus_t;

struct BK4819_HwFskConfig_t {
    uint32_t baud;              // REG_72, see BK4819_HwFskBaudWord
    uint8_t  preamble_bytes;    // 1..16, transmitted as 0xAA
    uint8_t  sync_bytes;        // 2 or 4
    uint16_t sync01;            // REG_5A, sync byte 0 in the high half
    uint16_t sync23;            // REG_5B, used when sync_bytes is 4
    uint16_t deviation;         // REG_40<11:0>, 0 = min, 0xFFF = max
    uint8_t  gain;              // REG_70<6:0>, TONE2/FSK tuning gain
    bool     invert;            // REG_59<9>, invert the data on air
    bool     crc;               // REG_5C<6>, appends 2 bytes when set
    bool     scramble;          // REG_59<13>

};
typedef struct BK4819_HwFskConfig_t BK4819_HwFskConfig_t;

// REG_72 word for a bit rate. The datasheet gives the FSK rate register as
// freq(Hz) * 10.32444 for a 13MHz or 26MHz crystal, and it is multiplexed with
// the Tone2 register. 1200 baud comes out as 0x3065, which is the constant the
// existing aircopy code uses, so the formula checks out.
uint16_t BK4819_HwFskBaudWord(uint32_t baud);

// Validates a config without touching the chip.
BK4819_HwFskStatus_t BK4819_HwFskCheck(const BK4819_HwFskConfig_t *cfg, uint32_t len);

// Programs the modem. Does not key the transmitter.
BK4819_HwFskStatus_t BK4819_HwFskSetup(const BK4819_HwFskConfig_t *cfg);

// Loads the payload and transmits it, blocking until the chip reports the
// packet finished. The payload must be an even number of bytes and fit in one
// FIFO load; the modem emits preamble and sync ahead of it automatically.
BK4819_HwFskStatus_t BK4819_HwFskSend(const BK4819_HwFskConfig_t *cfg,
                                      const uint8_t *payload,
                                      uint32_t len);

// Disables the modem and the tone generator it shares a gain field with.
void BK4819_HwFskStop(void);

// --- receive ---------------------------------------------------------------
//
// The modem receives the same framing it transmits: it waits for a run of
// alternating preamble, matches the sync word, then clocks REG_5D bytes into
// the RX FIFO. Point the sync word at a format's own frame sync and the FIFO
// delivers that format's data, which is how POCSAG is received here.
//
// The RX FIFO is 8 words where the TX one is 128, so it has to be drained
// while the packet is still arriving. The chip raises FSK_FIFO_ALMOST_FULL
// once the threshold is reached and that is the only indication of how much is
// waiting, so exactly BK4819_HWFSK_RX_CHUNK bytes are read per event.

#define BK4819_HWFSK_RX_FIFO_WORDS   8u    // REG_5E, the RX side is much smaller
#define BK4819_HWFSK_RX_THRESHOLD    4u    // words held before almost-full fires
#define BK4819_HWFSK_RX_CHUNK        (BK4819_HWFSK_RX_THRESHOLD * 2u)   // bytes
#define BK4819_HWFSK_RX_MAX_LENGTH   2040u // REG_5D holds 11 bits; see RxSetup

struct BK4819_HwFskRxEvent_t {
    bool sync;            // the sync word was matched, a packet is starting
    bool sync_inverted;   // ...and it matched only with the data inverted
    bool finished;        // the programmed data length was reached
    bool overrun;         // bytes were dropped because the caller's buffer filled
};
typedef struct BK4819_HwFskRxEvent_t BK4819_HwFskRxEvent_t;

// Validates an RX config and data length without touching the chip. data_bytes
// must be a multiple of BK4819_HWFSK_RX_CHUNK, because a partial final chunk
// would sit in the FIFO with no way to tell how much of it is there.
BK4819_HwFskStatus_t BK4819_HwFskRxCheck(const BK4819_HwFskConfig_t *cfg,
                                         uint32_t data_bytes);

// Programs the modem for receive and arms it. The receiver itself has to be
// tuned and turned on separately; this only sets up the data path.
BK4819_HwFskStatus_t BK4819_HwFskRxSetup(const BK4819_HwFskConfig_t *cfg,
                                         uint32_t data_bytes);

// As above, but with REG_58 given explicitly, which is what selects the
// demodulator: plain FSK, or one of the two FFSK tone pairs. Bell 202 is
// 1200/2200 and the chip offers neither 1200/1800 nor 1200/2400 exactly, so
// which of them copes better with 2200 is a question for a bench probe rather
// than for the datasheet. See BK4819_HWFSK_RX58_* below.
BK4819_HwFskStatus_t BK4819_HwFskRxSetupMode(const BK4819_HwFskConfig_t *cfg,
                                             uint32_t data_bytes,
                                             uint16_t reg58);

/* REG_58 values for receive, assembled from BK4819V3Registers_List_20201218.pdf.
 * Common to all three: <7:6>=11, which the register list does not document at all
 * but the stock aircopy code sets as though it were the preamble type; <5:4>=11,
 * the documented preamble type, 11 meaning 0xAA; and <0>=1 to enable.
 *
 *   FSK12K    <12:10>=000 mode, <3:1>=000 bandwidth. What POCSAG receives on.
 *   FSK24K    <12:10>=000 mode, <3:1>=100 bandwidth, which the list gives as
 *             "100 for FSK 2.4K". The same demodulator with a wider front end, for
 *             direct FSK above 1200 baud - 2400 works on the 1.2K setting too, but
 *             only just, and nothing locks at 4800 with it.
 *   FFSK2400  <12:10>=100 mode, <3:1>=100 bandwidth.
 *   FFSK1800  <12:10>=111 mode, <3:1>=001 bandwidth.
 */
#define BK4819_HWFSK_RX58_FSK12K     0x00F1u
#define BK4819_HWFSK_RX58_FSK24K     0x00F9u
#define BK4819_HWFSK_RX58_FFSK2400   0x10F9u
#define BK4819_HWFSK_RX58_FFSK1800   0x1CF3u

// Throws away any part-received packet and starts hunting for preamble again.
// Cheaper than a full setup, and needed whenever the sender stops mid packet:
// the chip would otherwise keep clocking receiver noise into the FIFO until the
// programmed length was reached, deaf to the next real transmission.
void BK4819_HwFskRxRestart(const BK4819_HwFskConfig_t *cfg);

// Services the modem. Writes up to max bytes of received data to dst, returns
// how many, and reports what else happened in ev. Call it often enough that a
// chunk cannot arrive twice over: at 1200 baud that is every 50ms or so.
uint8_t BK4819_HwFskRxPoll(uint8_t *dst, uint8_t max, BK4819_HwFskRxEvent_t *ev);

#endif
