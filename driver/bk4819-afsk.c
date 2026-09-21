/* Bell 202 AFSK by bit banging the BK4819's tone generator. See bk4819-afsk.h. */

#include <stdint.h>

#ifdef BK4819_AFSK_HOST_TEST
    // The host test drives the counter itself, so the bit clock's arithmetic can
    // be checked against a simulated SysTick including its wrap.
    uint32_t BK4819_AfskTickValue(void);
    uint32_t BK4819_AfskTickReload(void);
#else
    #include "ARMCM0.h"

    static uint32_t BK4819_AfskTickValue(void)  { return SysTick->VAL; }
    static uint32_t BK4819_AfskTickReload(void) { return SysTick->LOAD; }
#endif

#include "bk4819-afsk.h"
#include "bk4819.h"

/* freq(Hz) * 10.32444, the same rounded form as scale_freq in bk4819.c, which is
 * static there. 1200Hz comes out as 0x3065, which is also the constant the MDC
 * roger beep uses for 1200 baud, and 2200Hz as 0x58BA. */
static uint16_t ToneWord(uint32_t hz)
{
    return (uint16_t)(((hz * 1353245u) + (1u << 16)) >> 17);
}

static uint32_t gSpaceHz = BK4819_AFSK_SPACE_HZ;

void BK4819_AfskSetSpaceTone(uint32_t hz)
{
    if (hz == BK4819_AFSK_SPACE_HZ || hz == BK4819_AFSK_SPACE_FFSK_HZ)
        gSpaceHz = hz;
}

uint32_t BK4819_AfskSpaceTone(void)
{
    return gSpaceHz;
}

void BK4819_AfskClockInit(BK4819_AfskClock_t *clock)
{
    clock->prev   = BK4819_AfskTickValue();
    clock->credit = 0;
}

void BK4819_AfskClockFeed(BK4819_AfskClock_t *clock, uint32_t value, uint32_t reload)
{
    if (value == clock->prev)
        return;

    /* The counter decrements to 0, then reloads and carries on, so a reading
     * above the previous one means one wrap in between: prev decrements to reach
     * 0, one more for the reload itself, then reload-value after it. Polling is
     * continuous and a whole period is 10ms, so it cannot have wrapped twice. */
    clock->credit += (value < clock->prev)
                     ? (clock->prev - value)
                     : (clock->prev + reload + 1u - value);

    clock->prev = value;
}

void BK4819_AfskClockWait(BK4819_AfskClock_t *clock, uint32_t ticks)
{
    const uint32_t reload = BK4819_AfskTickReload();

    while (clock->credit < ticks)
        BK4819_AfskClockFeed(clock, BK4819_AfskTickValue(), reload);

    // Whatever was measured beyond the target stays on the books, so the next
    // wait is shorter by exactly that much and the error never accumulates.
    clock->credit -= ticks;
}

void BK4819_AfskStart(uint8_t gain)
{
    BK4819_WriteRegister(BK4819_REG_70,
        BK4819_REG_70_ENABLE_TONE1 |
        ((uint16_t)(gain & 0x7Fu) << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_WriteRegister(BK4819_REG_71, ToneWord(BK4819_AFSK_MARK_HZ));

    // The caller settles the transmitter; this only starts the tone.
}

void BK4819_AfskSendBits(const uint8_t *air, uint32_t nbits)
{
    BK4819_AfskClock_t clock;
    bool               mark;

    if (nbits == 0)
        return;

    /* The first bit's tone is written unconditionally rather than assumed from
     * whatever the generator happens to be emitting, so this is safe to call
     * more than once and does not depend on BK4819_AfskStart having just run. */
    mark = (air[0] >> 7) & 1u;

    BK4819_WriteRegister(BK4819_REG_71,
        ToneWord(mark ? BK4819_AFSK_MARK_HZ : gSpaceHz));

    BK4819_AfskClockInit(&clock);

    for (uint32_t i = 0; i < nbits; i++) {
        const bool bit = (air[i >> 3] >> (7 - (i & 7))) & 1u;

        /* Written only when the tone actually changes. Rewriting REG_71 with the
         * value it already holds may restart the generator's phase, and a phase
         * step on every bit would splatter the spectrum for nothing. Skipping it
         * costs no accuracy: the clock measures real elapsed ticks, so a bit
         * with no write simply spends longer waiting.
         *
         * The write takes about 76us, so a transition lands that much after the
         * nominal bit boundary. Every transition is late by the same amount, so
         * each tone still lasts an exact number of bit periods, which is what a
         * demodulator measures. */
        if (bit != mark) {
            BK4819_WriteRegister(BK4819_REG_71,
                ToneWord(bit ? BK4819_AFSK_MARK_HZ : gSpaceHz));

            mark = bit;
        }

        BK4819_AfskClockWait(&clock, BK4819_AFSK_BIT_TICKS);
    }
}

void BK4819_AfskSendTone(bool mark, uint32_t bits)
{
    BK4819_AfskClock_t clock;

    BK4819_WriteRegister(BK4819_REG_71,
        ToneWord(mark ? BK4819_AFSK_MARK_HZ : gSpaceHz));

    BK4819_AfskClockInit(&clock);

    for (uint32_t i = 0; i < bits; i++)
        BK4819_AfskClockWait(&clock, BK4819_AFSK_BIT_TICKS);
}

void BK4819_AfskStop(void)
{
    BK4819_WriteRegister(BK4819_REG_70, 0);
}
