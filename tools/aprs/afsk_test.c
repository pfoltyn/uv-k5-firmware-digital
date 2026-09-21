/* Host tests for the bit clock in driver/bk4819-afsk.c.
 *
 * Timing is the one part of a bit banged modem that cannot be checked by looking
 * at it, so SysTick is simulated here: a down counter derived from a monotonic
 * tick count, which every counter read and every register write advances by a
 * realistic amount. That makes the two things worth proving testable, namely
 * that the wrap arithmetic is right and that the 76us cost of an SPI write does
 * not accumulate into a drifting bit rate.
 *
 * This file is host tooling only, it is never compiled into the firmware.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/bk4819-afsk.h"
#include "driver/bk4819-regs.h"

static int checks;
static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __func__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

// --- the simulated chip and clock ------------------------------------------

#define SIM_RELOAD   479999u   // what SYSTICK_Init leaves behind: 10ms at 48MHz

// Every read of SysTick->VAL costs a few cycles, so the busy wait cannot land
// exactly on its target. 8 ticks is a plausible loop iteration at 48MHz.
#define SIM_POLL_COST   8u

// BK4819_WriteU8 and BK4819_WriteU16 put three 1us delays on every SPI bit, so
// 24 bits plus framing comes to about 76us. At 48 ticks per us that is 3648.
#define SIM_WRITE_COST  3648u

static uint64_t sim_now;
static uint32_t sim_poll_cost  = SIM_POLL_COST;
static uint32_t sim_write_cost = SIM_WRITE_COST;

struct write_t {
    BK4819_REGISTER_t reg;
    uint16_t          value;
    uint64_t          at;
};

#define MAX_WRITES 8192

static struct write_t writes[MAX_WRITES];
static uint32_t       nwrites;

uint32_t BK4819_AfskTickReload(void)
{
    return SIM_RELOAD;
}

uint32_t BK4819_AfskTickValue(void)
{
    // A down counter that reloads: value at time t is reload - (t mod period).
    const uint32_t v = (uint32_t)(SIM_RELOAD - (sim_now % ((uint64_t)SIM_RELOAD + 1u)));

    sim_now += sim_poll_cost;

    return v;
}

void BK4819_WriteRegister(BK4819_REGISTER_t reg, uint16_t value)
{
    if (nwrites < MAX_WRITES) {
        writes[nwrites].reg   = reg;
        writes[nwrites].value = value;
        writes[nwrites].at    = sim_now;
        nwrites++;
    }

    sim_now += sim_write_cost;
}

static void Reset(void)
{
    sim_now        = 0;
    nwrites        = 0;
    sim_poll_cost  = SIM_POLL_COST;
    sim_write_cost = SIM_WRITE_COST;
}

// --- the clock -------------------------------------------------------------

static void TestFeedNoWrap(void)
{
    BK4819_AfskClock_t c = { 1000u, 0u };

    BK4819_AfskClockFeed(&c, 900u, SIM_RELOAD);
    CHECK(c.credit == 100u, "credit %u want 100", c.credit);
    CHECK(c.prev == 900u, "prev %u want 900", c.prev);

    BK4819_AfskClockFeed(&c, 800u, SIM_RELOAD);
    CHECK(c.credit == 200u, "credit %u want 200", c.credit);
}

static void TestFeedAcrossWrap(void)
{
    /* From 10, the counter takes 10 decrements to reach 0, one more for the
     * reload, then 479999 down to 479990 is nine more. Twenty in total. Getting
     * the +1 wrong here would lose one tick per wrap, which is 12 wraps a second
     * and invisible until a long frame drifts. */
    BK4819_AfskClock_t c = { 10u, 0u };

    BK4819_AfskClockFeed(&c, 479990u, SIM_RELOAD);
    CHECK(c.credit == 20u, "credit %u want 20", c.credit);

    // A wrap landing exactly on the reload value.
    BK4819_AfskClock_t d = { 5u, 0u };

    BK4819_AfskClockFeed(&d, SIM_RELOAD, SIM_RELOAD);
    CHECK(d.credit == 6u, "credit %u want 6", d.credit);
}

static void TestFeedIgnoresARepeatedReading(void)
{
    // The counter runs at the core clock, so consecutive reads can return the
    // same value. Counting that as a whole period would be catastrophic.
    BK4819_AfskClock_t c = { 1234u, 77u };

    BK4819_AfskClockFeed(&c, 1234u, SIM_RELOAD);
    CHECK(c.credit == 77u, "credit %u want 77", c.credit);
}

static void TestWaitConsumesOnlyWhatItAsked(void)
{
    BK4819_AfskClock_t c;

    Reset();
    BK4819_AfskClockInit(&c);

    const uint64_t start = sim_now;

    BK4819_AfskClockWait(&c, 40000u);

    const uint64_t elapsed = sim_now - start;

    // It has to wait at least the requested time, and overshoot by no more than
    // one poll, with the overshoot left in credit rather than thrown away.
    CHECK(elapsed >= 40000u, "waited %llu ticks, want at least 40000",
          (unsigned long long)elapsed);
    CHECK(elapsed < 40000u + sim_poll_cost * 2u, "waited %llu ticks, overshot",
          (unsigned long long)elapsed);
    CHECK(c.credit < sim_poll_cost * 2u, "credit %u left over", c.credit);
}

// --- the modem -------------------------------------------------------------

static void TestToneWords(void)
{
    /* The whole point of this path is exact Bell 202, so the two control words
     * are worth pinning: 1200Hz is 0x3065, which is also what the MDC roger beep
     * uses, and 2200Hz is 0x58BA. */
    Reset();
    BK4819_AfskStart(66);

    CHECK(nwrites == 2, "start made %u writes want 2", nwrites);
    CHECK(writes[0].reg == BK4819_REG_70, "first write was not REG_70");
    CHECK(writes[0].value == (0x8000u | (66u << 8)), "REG_70 %04x", writes[0].value);
    CHECK(writes[1].reg == BK4819_REG_71, "second write was not REG_71");
    CHECK(writes[1].value == 0x3065u, "mark word %04x want 3065", writes[1].value);

    Reset();
    BK4819_AfskSendTone(false, 1);
    CHECK(writes[0].value == 0x58BAu, "space word %04x want 58ba", writes[0].value);
}

static void TestBitOrderIsMsbFirst(void)
{
    /* 0xAA most significant bit first is 1,0,1,0,1,0,1,0: one write to set the
     * first bit's tone, then seven transitions. Least significant bit first
     * would be 0,1,0,1,... so the first value written pins the order down: mark
     * for MSB first, space for LSB first. */
    const uint8_t air[] = { 0xAAu };

    Reset();
    BK4819_AfskSendBits(air, 8);

    CHECK(nwrites == 8, "%u writes want 8", nwrites);
    CHECK(nwrites > 0 && writes[0].value == 0x3065u,
          "first write %04x want the mark word", nwrites ? writes[0].value : 0);
}

static void TestRepeatedBitsWriteNothing(void)
{
    // A run of identical bits must not rewrite REG_71: that could restart the
    // generator's phase on every bit and splatter the spectrum for nothing.
    const uint8_t air[] = { 0xFFu, 0xFFu };   // sixteen mark bits

    Reset();
    BK4819_AfskSendBits(air, 16);
    CHECK(nwrites == 1, "%u writes for a steady mark, want 1", nwrites);
    CHECK(writes[0].value == 0x3065u, "%04x want the mark word", writes[0].value);

    const uint8_t space[] = { 0x00u, 0x00u };

    Reset();
    BK4819_AfskSendBits(space, 16);
    CHECK(nwrites == 1, "%u writes for a steady space, want 1", nwrites);
    CHECK(writes[0].value == 0x58BAu, "%04x want the space word", writes[0].value);
}

static void TestNoCumulativeDrift(void)
{
    /* The reason the clock carries credit instead of delaying a fixed amount. An
     * SPI write costs 76us of an 833us bit; if that were simply added to each
     * bit the rate would run 9% slow and no TNC would decode it. */
    Reset();

    const uint64_t start = sim_now;
    const uint32_t bits  = 2000;

    BK4819_AfskSendTone(true, bits);

    const uint64_t elapsed = sim_now - start;
    const uint64_t want    = (uint64_t)bits * BK4819_AFSK_BIT_TICKS;

    CHECK(elapsed >= want, "%llu ticks for %u bits, want at least %llu",
          (unsigned long long)elapsed, bits, (unsigned long long)want);

    // Allow one poll of slack per bit, which is 0.02% at these numbers. Anything
    // that drifts shows up here as a percentage, not as a handful of ticks.
    CHECK(elapsed < want + (uint64_t)bits * sim_poll_cost * 2u,
          "%llu ticks for %u bits, wanted about %llu: drifting",
          (unsigned long long)elapsed, bits, (unsigned long long)want);
}

static void TestTransitionsLandOnBitBoundaries(void)
{
    /* Alternating bits, so every bit boundary carries a write and the write cost
     * has the most opportunity to accumulate.
     *
     * What is checked is the spacing between consecutive transitions, not their
     * absolute position. A demodulator measures how long each tone lasts, and a
     * constant offset shifts every transition equally and is invisible to it;
     * this path has one, because the write that sets bit 0's tone happens before
     * the clock starts. Drift, which is what would actually break decoding,
     * shows up in the spacing. */
    uint8_t air[64];

    for (size_t i = 0; i < sizeof air; i++)
        air[i] = 0xAAu;

    Reset();
    BK4819_AfskSendBits(air, sizeof air * 8u);

    CHECK(nwrites == sizeof air * 8u, "%u writes want %zu",
          nwrites, sizeof air * 8u);

    uint64_t worst = 0;

    for (uint32_t k = 2; k < nwrites; k++) {
        const uint64_t gap = writes[k].at - writes[k - 1].at;
        const uint64_t off = (gap > BK4819_AFSK_BIT_TICKS)
                             ? gap - BK4819_AFSK_BIT_TICKS
                             : BK4819_AFSK_BIT_TICKS - gap;

        if (off > worst)
            worst = off;
    }

    // A quarter of a percent of a bit. Anything that drifts would show hundreds
    // of times this by the end of a frame.
    CHECK(worst < BK4819_AFSK_BIT_TICKS / 400u,
          "worst gap between transitions was %llu ticks from one bit period",
          (unsigned long long)worst);
}

static void TestSurvivesASlowPoll(void)
{
    /* A SysTick interrupt fires every 10ms, which is every twelfth bit, and the
     * UI runs in it. Model that as a much more expensive poll and confirm the
     * clock still keeps the average rate: it can only do that by shortening the
     * bits that follow, which is exactly what credit is for.
     */
    Reset();
    sim_poll_cost = 400u;   // 8.3us per poll

    const uint64_t start = sim_now;
    const uint32_t bits  = 500;

    BK4819_AfskSendTone(true, bits);

    const uint64_t elapsed = sim_now - start;
    const uint64_t want    = (uint64_t)bits * BK4819_AFSK_BIT_TICKS;

    CHECK(elapsed >= want, "%llu ticks want at least %llu",
          (unsigned long long)elapsed, (unsigned long long)want);
    CHECK(elapsed < want + (uint64_t)bits * sim_poll_cost * 2u,
          "%llu ticks want about %llu", (unsigned long long)elapsed,
          (unsigned long long)want);
}

static void TestAFrameWorthOfBitsStaysOnRate(void)
{
    // A realistic length: sixteen lead flags plus a short beacon is around 800
    // air bits, or two thirds of a second.
    uint8_t  air[110];
    uint32_t seed = 7u;

    for (size_t i = 0; i < sizeof air; i++) {
        seed   = seed * 1103515245u + 12345u;
        air[i] = (uint8_t)(seed >> 16);
    }

    Reset();

    const uint64_t start = sim_now;
    const uint32_t bits  = sizeof air * 8u;

    BK4819_AfskSendBits(air, bits);

    const uint64_t elapsed = sim_now - start;
    const uint64_t want    = (uint64_t)bits * BK4819_AFSK_BIT_TICKS;
    const double   error   = 100.0 * (double)(elapsed - want) / (double)want;

    CHECK(elapsed >= want, "finished early");
    CHECK(error < 0.1, "rate error %.3f%% over %u bits", error, bits);

    printf("      %u bits in %llu ticks (%.3fs simulated), rate error %.4f%%\n",
           bits, (unsigned long long)elapsed,
           (double)elapsed / (double)BK4819_AFSK_TICK_HZ, error);
}

int main(void)
{
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "feed, no wrap",                TestFeedNoWrap },
        { "feed across a wrap",           TestFeedAcrossWrap },
        { "feed ignores a repeat",        TestFeedIgnoresARepeatedReading },
        { "wait consumes what it asked",  TestWaitConsumesOnlyWhatItAsked },
        { "tone control words",           TestToneWords },
        { "bit order is MSB first",       TestBitOrderIsMsbFirst },
        { "repeated bits write nothing",  TestRepeatedBitsWriteNothing },
        { "no cumulative drift",          TestNoCumulativeDrift },
        { "transitions on boundaries",    TestTransitionsLandOnBitBoundaries },
        { "survives a slow poll",         TestSurvivesASlowPoll },
        { "a frame stays on rate",        TestAFrameWorthOfBitsStaysOnRate },
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before = failures;

        tests[i].fn();
        printf("%-32s %s\n", tests[i].name, (failures == before) ? "ok" : "FAILED");
    }

    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
