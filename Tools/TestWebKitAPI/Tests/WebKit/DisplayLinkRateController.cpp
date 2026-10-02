/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if PLATFORM(MAC)

#include "DisplayLinkRateController.h"
#include "Test.h"
#include <wtf/Vector.h>

namespace WebKit {

// So that the tests name the reconfigurations they compare.
static void PrintTo(DisplayLinkReconfiguration reconfiguration, std::ostream* stream)
{
    switch (reconfiguration) {
    case DisplayLinkReconfiguration::None:
        *stream << "None";
        return;
    case DisplayLinkReconfiguration::RemoveLink:
        *stream << "RemoveLink";
        return;
    case DisplayLinkReconfiguration::Restart:
        *stream << "Restart";
        return;
    case DisplayLinkReconfiguration::ReplaceLink:
        *stream << "ReplaceLink";
        return;
    case DisplayLinkReconfiguration::NominalRateChanged:
        *stream << "NominalRateChanged";
        return;
    }
}

} // namespace WebKit

namespace TestWebKitAPI {

using namespace WebCore;
using namespace WebKit;

static unsigned divisor(FramesPerSecond nominalFramesPerSecond, std::initializer_list<FramesPerSecond> demands)
{
    Vector<FramesPerSecond> demandsVector(demands);
    return displayLinkFrameRateDivisor(nominalFramesPerSecond, demandsVector.span());
}

TEST(DisplayLinkRate, FrameRateDivisorSingleDemand)
{
    ASSERT_EQ(divisor(60, { 60 }), 1u);
    ASSERT_EQ(divisor(60, { 30 }), 2u);
    ASSERT_EQ(divisor(120, { 60 }), 2u);
    ASSERT_EQ(divisor(120, { 120 }), 1u);
    ASSERT_EQ(divisor(240, { 60 }), 4u);
    ASSERT_EQ(divisor(240, { 30 }), 8u);
    ASSERT_EQ(divisor(240, { 240 }), 1u);
    ASSERT_EQ(divisor(144, { 72 }), 2u);
    ASSERT_EQ(divisor(165, { 82 }), 2u);
    // Rounded up from 165 / 2 = 82.5: still the 82.5 fps rung.
    ASSERT_EQ(divisor(165, { 83 }), 2u);
    // 72 fps, 1.4% below the demand.
    ASSERT_EQ(divisor(144, { 73 }), 2u);
    // A demand above the nominal rate gets every tick.
    ASSERT_EQ(divisor(60, { 120 }), 1u);
}

TEST(DisplayLinkRate, FrameRateDivisorSeveralDemands)
{
    ASSERT_EQ(divisor(240, { 60, 30 }), 4u);
    ASSERT_EQ(divisor(240, { 60, 120 }), 2u);
    // The rungs of 60 and 48 (divisors 4 and 5) only share 240 fps, where both get an exact cadence.
    ASSERT_EQ(divisor(240, { 60, 48 }), 1u);
    ASSERT_EQ(divisor(165, { 82, 41 }), 2u);
    ASSERT_EQ(divisor(175, { 87, 43 }), 2u);
    // A demand of 0 asks for every tick.
    ASSERT_EQ(divisor(240, { 0 }), 1u);
    ASSERT_EQ(divisor(240, { 0, 60 }), 1u);
    ASSERT_EQ(divisor(240, { }), 1u);
    ASSERT_EQ(divisor(0, { 60 }), 1u);
}

// Which of `framesPerSecond` a tick is relevant for, as a string of 0s and 1s.
static std::string relevance(DisplayUpdate update, std::initializer_list<FramesPerSecond> framesPerSecond)
{
    std::string result;
    for (auto rate : framesPerSecond)
        result += update.relevantForUpdateFrequency(rate) ? '1' : '0';
    return result;
}

// Drives a DisplayLinkUpdateCounter with ticks, keeping the time in vsyncs.
struct DisplayLinkTicks {
    // A tick `vsyncs` vsyncs after the previous one; by default, the link's interval.
    DisplayUpdate tick(unsigned linkDivisor, unsigned requestedDivisor, FramesPerSecond nominalFramesPerSecond, unsigned vsyncs = 0)
    {
        vsyncs = vsyncs ? vsyncs : linkDivisor;
        vsync += vsyncs;
        return counter.tick(linkDivisor, requestedDivisor, nominalFramesPerSecond, vsyncs);
    }

    DisplayLinkUpdateCounter counter;
    uint64_t vsync { 0 };
};

TEST(DisplayLinkRate, UpdateCounterCountsAtTheLinkRate)
{
    DisplayLinkTicks ticks;
    // 240 Hz, the link firing at 60 fps as requested.
    for (unsigned i = 0; i < 8; ++i) {
        auto update = ticks.tick(4, 4, 240);
        ASSERT_EQ(update.updateIndex, i);
        ASSERT_EQ(update.updatesPerSecond, 60u);
        ASSERT_EQ(relevance(update, { 60, 30, 15 }), i % 4 ? (i % 2 ? "100" : "110") : "111");
    }
    ASSERT_FALSE(ticks.counter.firesAtAnotherDivisor());
}

TEST(DisplayLinkRate, UpdateCounterStartsAtZeroAfterReset)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 5; ++i)
        ticks.tick(4, 4, 240);
    ticks.counter.reset();
    auto update = ticks.tick(4, 4, 240);
    ASSERT_EQ(update.updateIndex, 0u);
    ASSERT_EQ(update.updatesPerSecond, 60u);
}

// Records, in vsyncs, the intervals between the updates a `framesPerSecond` observer gets.
struct ObserverUpdates {
    explicit ObserverUpdates(FramesPerSecond framesPerSecond)
        : framesPerSecond(framesPerSecond)
    {
    }

    void check(const DisplayLinkTicks& ticks, DisplayUpdate update)
    {
        if (!update.relevantForUpdateFrequency(framesPerSecond))
            return;
        if (previousVSync)
            intervals.append(ticks.vsync - *previousVSync);
        previousVSync = ticks.vsync;
    }

    FramesPerSecond framesPerSecond;
    std::optional<uint64_t> previousVSync;
    Vector<uint64_t> intervals;
};

// When the link speeds up, the first tick at the new rate comes one or more vsyncs after the last one at the old rate.
// Whenever it comes, a 60 fps observer's updates stay 4 vsyncs apart at 240 Hz (60 -> 240 fps, when a scroll starts),
// and 2 vsyncs apart at 120 Hz (60 -> 120 fps).
TEST(DisplayLinkRate, UpdateCounterKeepsCadenceWhenSpeedingUp)
{
    for (auto [nominalFramesPerSecond, slowDivisor] : std::initializer_list<std::pair<FramesPerSecond, unsigned>> { { 240, 4 }, { 120, 2 } }) {
        for (unsigned vsyncsToFirstFastTick = 1; vsyncsToFirstFastTick <= slowDivisor; ++vsyncsToFirstFastTick) {
            DisplayLinkTicks ticks;
            ObserverUpdates observer(60);
            for (unsigned i = 0; i < 10; ++i)
                observer.check(ticks, ticks.tick(slowDivisor, slowDivisor, nominalFramesPerSecond));
            observer.check(ticks, ticks.tick(1, 1, nominalFramesPerSecond, vsyncsToFirstFastTick));
            for (unsigned i = 0; i < 20; ++i)
                observer.check(ticks, ticks.tick(1, 1, nominalFramesPerSecond));
            for (auto interval : observer.intervals)
                EXPECT_EQ(interval, slowDivisor) << "at " << nominalFramesPerSecond << " Hz, first fast tick " << vsyncsToFirstFastTick << " vsyncs after the last slow one";
        }
    }
}

// When the link slows down to 60 fps at 240 Hz (when a scroll ends), every tick is a 60 fps observer's: it gets the
// first one, whenever Core Animation fires it, and then one every 4 vsyncs.
TEST(DisplayLinkRate, UpdateCounterSlowingDown)
{
    for (unsigned vsyncsToFirstSlowTick = 1; vsyncsToFirstSlowTick <= 4; ++vsyncsToFirstSlowTick) {
        DisplayLinkTicks ticks;
        ObserverUpdates observer(60);
        for (unsigned i = 0; i < 18; ++i)
            observer.check(ticks, ticks.tick(1, 1, 240));
        auto lastUpdateAtTheFastRate = *observer.previousVSync;
        auto firstSlowTick = ticks.tick(4, 4, 240, vsyncsToFirstSlowTick);
        EXPECT_TRUE(firstSlowTick.relevantForUpdateFrequency(60)) << vsyncsToFirstSlowTick;
        observer.check(ticks, firstSlowTick);
        // At most one tick of the new rate late, and never before the next vsync.
        EXPECT_GE(ticks.vsync - lastUpdateAtTheFastRate, 1u);
        EXPECT_LE(ticks.vsync - lastUpdateAtTheFastRate, 4u + vsyncsToFirstSlowTick);
        for (unsigned i = 0; i < 6; ++i)
            observer.check(ticks, ticks.tick(4, 4, 240));
        for (unsigned i = observer.intervals.size() - 6; i < observer.intervals.size(); ++i)
            EXPECT_EQ(observer.intervals[i], 4u);
    }
}

// An observer slower than the new rate: a 30 fps observer, every 8 vsyncs at 240 Hz. When the link goes from 30 to 60 fps,
// or from 240 to 60 fps after 18 ticks, whenever the first tick at the new rate comes, the observer's interval across the
// change is at most one tick of the new rate (4 vsyncs) early and half a tick late, then 8 vsyncs again. When the link
// goes from 30 to 240 fps, it stays exactly 8 vsyncs.
TEST(DisplayLinkRate, UpdateCounterKeepsCadenceOfSlowerObservers)
{
    struct Change {
        unsigned fromDivisor;
        unsigned toDivisor;
        unsigned slowestFirstTick;
        uint64_t earliestInterval;
        uint64_t latestInterval;
    };
    for (auto change : std::initializer_list<Change> { { 8, 4, 8, 4, 10 }, { 1, 4, 4, 4, 10 }, { 8, 1, 8, 8, 8 } }) {
        for (unsigned vsyncsToFirstTick = 1; vsyncsToFirstTick <= change.slowestFirstTick; ++vsyncsToFirstTick) {
            DisplayLinkTicks ticks;
            ObserverUpdates observer(30);
            for (unsigned i = 0; i < 18; ++i)
                observer.check(ticks, ticks.tick(change.fromDivisor, change.fromDivisor, 240));
            auto intervalsBeforeChange = observer.intervals.size();
            observer.check(ticks, ticks.tick(change.toDivisor, change.toDivisor, 240, vsyncsToFirstTick));
            // 64 vsyncs at the new rate.
            for (unsigned i = 0; i < 64 / change.toDivisor; ++i)
                observer.check(ticks, ticks.tick(change.toDivisor, change.toDivisor, 240));
            ASSERT_GT(observer.intervals.size(), intervalsBeforeChange + 2);
            auto interval = observer.intervals[intervalsBeforeChange];
            EXPECT_GE(interval, change.earliestInterval) << change.fromDivisor << " -> " << change.toDivisor << ", first tick after " << vsyncsToFirstTick;
            EXPECT_LE(interval, change.latestInterval) << change.fromDivisor << " -> " << change.toDivisor << ", first tick after " << vsyncsToFirstTick;
            for (auto i = intervalsBeforeChange + 1; i < observer.intervals.size(); ++i)
                EXPECT_EQ(observer.intervals[i], 8u) << change.fromDivisor << " -> " << change.toDivisor << ", first tick after " << vsyncsToFirstTick;
        }
    }
}

// When the link slows down, a re-base doesn't count past the index the old rate's next tick would have had, where an
// observer slower than the new rate can be due: whatever the phase of the change, a 30 fps observer's interval across it
// is less than a tick of the new rate away from its period, and it loses no update. At 240 Hz, the link goes from 240 to
// 60 fps after 16 to 23 ticks; at 120 Hz, from 120 to 60 fps after 8 to 11.
TEST(DisplayLinkRate, UpdateCounterSlowingDownAtEveryPhase)
{
    for (auto [nominalFramesPerSecond, slowDivisor] : std::initializer_list<std::pair<FramesPerSecond, unsigned>> { { 240, 4 }, { 120, 2 } }) {
        uint64_t period = nominalFramesPerSecond / 30;
        for (unsigned fastTicks = 2 * period; fastTicks < 3 * period; ++fastTicks) {
            for (unsigned vsyncsToFirstSlowTick = 1; vsyncsToFirstSlowTick <= slowDivisor; ++vsyncsToFirstSlowTick) {
                DisplayLinkTicks ticks;
                ObserverUpdates observer(30);
                for (unsigned i = 0; i < fastTicks; ++i)
                    observer.check(ticks, ticks.tick(1, 1, nominalFramesPerSecond));
                auto intervalsBeforeChange = observer.intervals.size();
                observer.check(ticks, ticks.tick(slowDivisor, slowDivisor, nominalFramesPerSecond, vsyncsToFirstSlowTick));
                for (unsigned i = 0; i < 64 / slowDivisor; ++i)
                    observer.check(ticks, ticks.tick(slowDivisor, slowDivisor, nominalFramesPerSecond));
                ASSERT_GT(observer.intervals.size(), intervalsBeforeChange + 2);
                auto interval = observer.intervals[intervalsBeforeChange];
                EXPECT_GT(interval, period - slowDivisor) << "at " << nominalFramesPerSecond << " Hz after " << fastTicks << " ticks, first slow tick after " << vsyncsToFirstSlowTick;
                EXPECT_LT(interval, period + slowDivisor) << "at " << nominalFramesPerSecond << " Hz after " << fastTicks << " ticks, first slow tick after " << vsyncsToFirstSlowTick;
                for (auto i = intervalsBeforeChange + 1; i < observer.intervals.size(); ++i)
                    EXPECT_EQ(observer.intervals[i], period) << "at " << nominalFramesPerSecond << " Hz after " << fastTicks << " ticks, first slow tick after " << vsyncsToFirstSlowTick;
            }
        }
    }
}

// When the first tick at the display's rate comes after the next slow tick would have, a 60 fps observer gets its update
// late by that much, once: the tick isn't counted past the observer's due index.
TEST(DisplayLinkRate, UpdateCounterLateFirstFastTick)
{
    for (auto [nominalFramesPerSecond, slowDivisor] : std::initializer_list<std::pair<FramesPerSecond, unsigned>> { { 240, 4 }, { 120, 2 } }) {
        DisplayLinkTicks ticks;
        ObserverUpdates observer(60);
        for (unsigned i = 0; i < 10; ++i)
            observer.check(ticks, ticks.tick(slowDivisor, slowDivisor, nominalFramesPerSecond));
        auto intervalsBeforeChange = observer.intervals.size();
        observer.check(ticks, ticks.tick(1, 1, nominalFramesPerSecond, slowDivisor + 1));
        for (unsigned i = 0; i < 20; ++i)
            observer.check(ticks, ticks.tick(1, 1, nominalFramesPerSecond));
        ASSERT_GT(observer.intervals.size(), intervalsBeforeChange + 2);
        EXPECT_EQ(observer.intervals[intervalsBeforeChange], slowDivisor + 1u) << "at " << nominalFramesPerSecond << " Hz";
        for (auto i = intervalsBeforeChange + 1; i < observer.intervals.size(); ++i)
            EXPECT_EQ(observer.intervals[i], slowDivisor) << "at " << nominalFramesPerSecond << " Hz";
    }
}

// While the display changes mode, a CADisplayLink can report a single stray interval: the count goes on at the current
// rate.
TEST(DisplayLinkRate, UpdateCounterIgnoresAStrayInterval)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 5; ++i)
        ticks.tick(1, 1, 60);
    auto stray = ticks.tick(4, 1, 60, 1);
    ASSERT_EQ(stray.updateIndex, 5u);
    ASSERT_EQ(stray.updatesPerSecond, 60u);
    auto next = ticks.tick(1, 1, 60);
    ASSERT_EQ(next.updateIndex, 6u);
    ASSERT_EQ(next.updatesPerSecond, 60u);
    ASSERT_FALSE(ticks.counter.firesAtAnotherDivisor());
}

// Without a requested divisor of the display's rate, as while a request made for another mode of the display is
// re-mapped, a stray interval isn't counted at either: the link's rate is followed when two ticks in a row have it.
TEST(DisplayLinkRate, UpdateCounterIgnoresAStrayIntervalWithoutARequest)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 5; ++i)
        ticks.tick(1, 0, 60);
    auto stray = ticks.tick(4, 0, 60, 1);
    ASSERT_EQ(stray.updateIndex, 5u);
    ASSERT_EQ(stray.updatesPerSecond, 60u);
    ASSERT_EQ(ticks.tick(1, 0, 60).updateIndex, 6u);
    ticks.tick(2, 0, 60);
    ASSERT_EQ(ticks.tick(2, 0, 60).updatesPerSecond, 30u);
}

// Without a requested divisor, a single tick at the display's rate isn't followed either, as it would be if it were the
// requested rate.
TEST(DisplayLinkRate, UpdateCounterFollowsTheDisplaysRateWithoutARequestAfterTwoTicks)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 3; ++i)
        ticks.tick(2, 0, 120);
    auto first = ticks.tick(1, 0, 120, 1);
    ASSERT_EQ(first.updateIndex, 3u);
    ASSERT_EQ(first.updatesPerSecond, 60u);
    ASSERT_EQ(ticks.tick(1, 0, 120).updatesPerSecond, 120u);
}

// A divisor that isn't the requested one is only counted at when two ticks in a row have it.
TEST(DisplayLinkRate, UpdateCounterFollowsAnUnrequestedRateAfterTwoTicks)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 5; ++i)
        ticks.tick(2, 2, 120);
    ASSERT_EQ(ticks.tick(4, 2, 120).updatesPerSecond, 60u);
    ASSERT_EQ(ticks.tick(4, 2, 120).updatesPerSecond, 30u);
}

// Rates are whole numbers of frames per second, as WebCore computes page rates: at 165 Hz, the half rate is 82, and
// its half, 41, gets every other tick.
TEST(DisplayLinkRate, UpdateCounterAt165Hz)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 6; ++i) {
        auto update = ticks.tick(2, 2, 165);
        ASSERT_EQ(update.updatesPerSecond, 82u);
        ASSERT_EQ(relevance(update, { 82, 41 }), i % 2 ? "10" : "11");
    }
}

// At 175 Hz, the half rate is 87, which 43 doesn't divide: the 43 fps observer gets every tick of the link, 87.5 fps.
// With CVDisplayLink, it gets every tick of the display, 175 fps.
TEST(DisplayLinkRate, UpdateCounterAt175Hz)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 4; ++i) {
        auto update = ticks.tick(2, 2, 175);
        ASSERT_EQ(update.updatesPerSecond, 87u);
        ASSERT_EQ(relevance(update, { 87, 43 }), "11");
    }
}

// Demands of 60 and 48 at 240 Hz: the link runs at 240 fps, and both get exact cadences.
TEST(DisplayLinkRate, UpdateCounterMixedDemands)
{
    DisplayLinkTicks ticks;
    unsigned relevantFor60 = 0;
    unsigned relevantFor48 = 0;
    for (unsigned i = 0; i < 240; ++i) {
        auto update = ticks.tick(1, divisor(240, { 60, 48 }), 240);
        relevantFor60 += update.relevantForUpdateFrequency(60);
        relevantFor48 += update.relevantForUpdateFrequency(48);
    }
    ASSERT_EQ(relevantFor60, 60u);
    ASSERT_EQ(relevantFor48, 48u);
}

// A tick that comes several vsyncs late, when the link thread is late: the count goes on by one tick, as a count of
// CVDisplayLink callbacks does.
TEST(DisplayLinkRate, UpdateCounterSkippedTick)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 3; ++i)
        ticks.tick(4, 4, 240);
    auto update = ticks.tick(4, 4, 240, 8);
    ASSERT_EQ(update.updateIndex, 3u);
    ASSERT_EQ(update.updatesPerSecond, 60u);
}

// If the link keeps firing at another divisor than the requested one for 100 ms, the counter says so; the backend then
// runs the link at the display's rate.
TEST(DisplayLinkRate, UpdateCounterDetectsAnotherGrantedDivisor)
{
    DisplayLinkTicks ticks;
    // 80 fps requested on a 240 Hz display (divisor 3), granted 120 fps (divisor 2): 24 vsyncs make 100 ms.
    for (unsigned i = 0; i < 11; ++i) {
        ticks.tick(2, 3, 240);
        ASSERT_FALSE(ticks.counter.firesAtAnotherDivisor());
    }
    ticks.tick(2, 3, 240);
    ASSERT_TRUE(ticks.counter.firesAtAnotherDivisor());

    // Asking for the display's rate: counted at right away, and no longer another divisor.
    auto update = ticks.tick(1, 1, 240);
    ASSERT_EQ(update.updatesPerSecond, 240u);
    ASSERT_FALSE(ticks.counter.firesAtAnotherDivisor());
}

// The display changes mode while the link keeps its divisor: counted at the new rate right away.
TEST(DisplayLinkRate, UpdateCounterFollowsANominalRateChange)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 3; ++i)
        ticks.tick(2, 2, 120);
    auto update = ticks.tick(2, 2, 240);
    ASSERT_EQ(update.updatesPerSecond, 120u);
}

// The display changes mode while the link keeps its rate, 60 fps from 240 to 120 Hz: the count goes on from the time
// of the first tick in the new mode, but never past the index the next tick at the old rate would have had, nor back
// to the previous tick's.
TEST(DisplayLinkRate, UpdateCounterReBasesAcrossAModeChange)
{
    {
        DisplayLinkTicks ticks;
        for (unsigned i = 0; i < 5; ++i)
            ticks.tick(4, 4, 240);
        auto update = ticks.tick(2, 2, 120);
        ASSERT_EQ(update.updateIndex, 5u);
        ASSERT_EQ(update.updatesPerSecond, 60u);
    }
    {
        // Two ticks of the new rate late: counted as the next index, where a 30 fps observer can be due.
        DisplayLinkTicks ticks;
        for (unsigned i = 0; i < 4; ++i)
            ticks.tick(4, 4, 240);
        ASSERT_EQ(ticks.tick(2, 2, 120, 4).updateIndex, 4u);
    }
    {
        // From 120 to 240 Hz, one vsync of the new rate after the previous tick.
        DisplayLinkTicks ticks;
        for (unsigned i = 0; i < 3; ++i)
            ticks.tick(2, 2, 120);
        ASSERT_EQ(ticks.tick(4, 4, 240, 1).updateIndex, 3u);
    }
}

// After reset(), the detection of another divisor starts again: a link that fell back on its last tick before it paused
// doesn't fall back again when it resumes at the requested divisor.
TEST(DisplayLinkRate, UpdateCounterResetStartsTheDetectionAgain)
{
    DisplayLinkTicks ticks;
    for (unsigned i = 0; i < 100 && !ticks.counter.firesAtAnotherDivisor(); ++i)
        ticks.tick(3, 4, 240);
    ASSERT_TRUE(ticks.counter.firesAtAnotherDivisor());
    ticks.counter.reset();
    for (unsigned i = 0; i < 10; ++i) {
        ticks.tick(4, 4, 240);
        ASSERT_FALSE(ticks.counter.firesAtAnotherDivisor());
    }
}

TEST(DisplayLinkRate, DeniedDivisors)
{
    DisplayLinkDeniedDivisors deniedDivisors;
    ASSERT_EQ(deniedDivisors.divisorToApply(0), 1u);
    ASSERT_EQ(deniedDivisors.divisorToApply(8), 8u);
    deniedDivisors.deny(8);
    deniedDivisors.deny(40);
    ASSERT_TRUE(deniedDivisors.isDenied(8));
    ASSERT_EQ(deniedDivisors.divisorToApply(8), 1u);
    ASSERT_EQ(deniedDivisors.divisorToApply(40), 1u);
    ASSERT_EQ(deniedDivisors.divisorToApply(4), 4u);
    // Divisors of 63 and more are denied together.
    ASSERT_EQ(deniedDivisors.divisorToApply(72), 72u);
    deniedDivisors.deny(72);
    ASSERT_EQ(deniedDivisors.divisorToApply(72), 1u);
    ASSERT_EQ(deniedDivisors.divisorToApply(90), 1u);
    ASSERT_EQ(deniedDivisors.divisorToApply(62), 62u);
    deniedDivisors.clear();
    ASSERT_EQ(deniedDivisors.divisorToApply(8), 8u);
    ASSERT_EQ(deniedDivisors.divisorToApply(72), 72u);
}

// Drives a DisplayLinkRateController with the ticks of a link that fires every `linkDivisor` vsyncs of a display at
// `refreshRate`, as a CADisplayLink reports them, while DisplayLink's nominal rate is `nominalFramesPerSecond`.
struct RateControllerTicks {
    DisplayLinkRateController::Tick tick(unsigned linkDivisor, double refreshRate, FramesPerSecond nominalFramesPerSecond, unsigned vsyncs = 0)
    {
        double duration = 1 / refreshRate;
        time += (vsyncs ? vsyncs : linkDivisor) * duration;
        return controller.tick(time, time + linkDivisor * duration, duration, nominalFramesPerSecond);
    }

    // Ticks for `seconds`, and returns whether any of them denied the request.
    bool tickFor(double seconds, unsigned linkDivisor, double refreshRate, FramesPerSecond nominalFramesPerSecond)
    {
        bool denied = false;
        for (double end = time + seconds; time < end;)
            denied |= tick(linkDivisor, refreshRate, nominalFramesPerSecond).requestDenied;
        return denied;
    }

    DisplayLinkRateController controller;
    double time { 1000 };
};

TEST(DisplayLinkRate, RateControllerAsksForTheRequestedRate)
{
    RateControllerTicks ticks;
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), std::nullopt);
    for (unsigned i = 0; i < 60; ++i) {
        auto tick = ticks.tick(4, 240, 240);
        ASSERT_EQ(tick.update.updatesPerSecond, 60u);
        ASSERT_FALSE(tick.requestDenied);
    }
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(1, 240), 0.0);
    ASSERT_EQ(ticks.tick(1, 240, 240).update.updatesPerSecond, 240u);
    // Of the nominal rate, on a mode that isn't a whole number of hertz.
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(2, 120), 60.0);
    ASSERT_EQ(ticks.tick(2, 119.98, 120).update.updatesPerSecond, 60u);
}

// The link keeps firing at another divisor: the request is denied once, and asking again gets the display's rate, which
// is counted at right away. Other divisors are still asked for.
TEST(DisplayLinkRate, RateControllerFallsBackToTheDisplaysRate)
{
    RateControllerTicks ticks;
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
    // Granted 80 fps instead of 60: 8 ticks make 100 ms.
    DisplayLinkRateController::Tick tick;
    unsigned tickCount = 0;
    while (!tick.requestDenied && tickCount < 20) {
        tick = ticks.tick(3, 240, 240);
        ++tickCount;
    }
    ASSERT_TRUE(tick.requestDenied);
    ASSERT_EQ(tickCount, 8u);
    ASSERT_EQ(tick.linkDivisor, 3u);
    ASSERT_EQ(tick.requestedDivisor, 4u);
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 0.0);
    ASSERT_EQ(ticks.controller.appliedDivisor(), 1u);
    ASSERT_EQ(ticks.tick(1, 240, 240).update.updatesPerSecond, 240u);
    ASSERT_FALSE(ticks.tickFor(1, 1, 240, 240));
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), std::nullopt);
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(2, 240), 120.0);
}

// When the link resumes, the divisor it didn't get is asked for again, whether or not it fell back on its last tick.
TEST(DisplayLinkRate, RateControllerAsksAgainWhenTheLinkResumes)
{
    for (bool tickAtTheDisplaysRateBeforePausing : { true, false }) {
        RateControllerTicks ticks;
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
        bool denied = false;
        for (unsigned i = 0; i < 20 && !denied; ++i)
            denied = ticks.tick(3, 240, 240).requestDenied;
        ASSERT_TRUE(denied);
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 0.0);
        if (tickAtTheDisplaysRateBeforePausing)
            ticks.tickFor(0.2, 1, 240, 240);

        ticks.time += 5;
        ticks.controller.linkResumed();
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
        auto tick = ticks.tick(4, 240, 240);
        ASSERT_EQ(tick.update.updateIndex, 0u);
        ASSERT_EQ(tick.update.updatesPerSecond, 60u);
        ASSERT_FALSE(tick.requestDenied);
        ASSERT_FALSE(ticks.tickFor(1, 4, 240, 240)) << tickAtTheDisplaysRateBeforePausing;
    }
}

// After a mode change, until the link is asked again at the new rate, it fires at the rate the old request maps to in
// the new mode: counted at, and not a denial. 60 fps from 240 to 120 Hz; 36 fps from 144 to 240 Hz, where the link can
// fire at 40 fps; and with a nominal rate DisplayLink hasn't updated yet.
TEST(DisplayLinkRate, RateControllerAcrossAModeChange)
{
    {
        RateControllerTicks ticks;
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
        ASSERT_FALSE(ticks.tickFor(0.5, 4, 240, 240));
        ASSERT_EQ(ticks.tick(2, 120, 240).update.updatesPerSecond, 60u);
        ASSERT_FALSE(ticks.tickFor(1, 2, 120, 240));
        ticks.controller.nominalRateChanged();
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(2, 120), 60.0);
        ASSERT_FALSE(ticks.tickFor(1, 2, 120, 120));
    }
    {
        RateControllerTicks ticks;
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 144), 36.0);
        ASSERT_FALSE(ticks.tickFor(0.5, 4, 144, 144));
        ASSERT_EQ(ticks.tick(6, 240, 144).update.updatesPerSecond, 40u);
        ASSERT_FALSE(ticks.tickFor(1, 6, 240, 144));
        ASSERT_FALSE(ticks.tickFor(1, 6, 240, 240));
        ticks.controller.nominalRateChanged();
        ASSERT_EQ(ticks.controller.preferredFramesPerSecond(6, 240), 40.0);
        ASSERT_FALSE(ticks.tickFor(1, 6, 240, 240));
    }
}

// A divisor of 63 or more that the link doesn't get runs it at the display's rate too (72 of 144 Hz, 2 fps, granted
// every 60 vsyncs).
TEST(DisplayLinkRate, RateControllerFallsBackForLargeDivisors)
{
    RateControllerTicks ticks;
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(72, 144), 2.0);
    bool denied = false;
    for (unsigned i = 0; i < 10 && !denied; ++i)
        denied = ticks.tick(60, 144, 144).requestDenied;
    ASSERT_TRUE(denied);
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(72, 144), 0.0);
    ASSERT_FALSE(ticks.tickFor(1, 1, 144, 144));
}

// When the nominal rate changes, the divisors the link didn't get are asked for again.
TEST(DisplayLinkRate, RateControllerAsksAgainAfterANominalRateChange)
{
    RateControllerTicks ticks;
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
    bool denied = false;
    for (unsigned i = 0; i < 20 && !denied; ++i)
        denied = ticks.tick(3, 240, 240).requestDenied;
    ASSERT_TRUE(denied);
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 0.0);
    ticks.tickFor(0.2, 1, 240, 240);
    ticks.controller.nominalRateChanged();
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(4, 240), 60.0);
}

// While a request made for another mode of the display is re-mapped (120 to 60 Hz with divisor 2 applied), a stray
// interval isn't counted at: the count goes on at the link's rate.
TEST(DisplayLinkRate, RateControllerIgnoresAStrayIntervalWhileARequestIsForAnotherMode)
{
    RateControllerTicks ticks;
    ASSERT_EQ(ticks.controller.preferredFramesPerSecond(2, 120), 60.0);
    for (unsigned i = 0; i < 20; ++i)
        ticks.tick(2, 120, 120);
    DisplayLinkRateController::Tick tick;
    for (unsigned i = 0; i < 4; ++i)
        tick = ticks.tick(1, 60, 120);
    ASSERT_EQ(tick.update.updatesPerSecond, 60u);
    auto index = tick.update.updateIndex;
    auto stray = ticks.tick(4, 60, 120, 1);
    ASSERT_EQ(stray.update.updatesPerSecond, 60u);
    ASSERT_EQ(stray.update.updateIndex, index + 1);
    ASSERT_EQ(ticks.tick(1, 60, 120).update.updateIndex, index + 2);
}

// What the CADisplayLink backend does at the end of a display reconfiguration, for a link made for display 5.
TEST(DisplayLinkRate, Reconfiguration)
{
    auto reconfiguration = [](DisplayLinkReconfigurationState state) {
        state.linkDisplayID = 5;
        return displayLinkReconfiguration(state);
    };
    // The display is gone.
    EXPECT_EQ(reconfiguration({ .hasLink = true, .reconfiguredDisplayID = 5 }), DisplayLinkReconfiguration::RemoveLink);
    EXPECT_EQ(reconfiguration({ .restartWhenScreenReturns = true, .reconfiguredDisplayID = 5 }), DisplayLinkReconfiguration::None);
    // It's back: start again if the backend was running, or was started meanwhile.
    EXPECT_EQ(reconfiguration({ .restartWhenScreenReturns = true, .screenDisplayID = 5, .reconfiguredDisplayID = 5, .displayWasAdded = true }), DisplayLinkReconfiguration::Restart);
    EXPECT_EQ(reconfiguration({ .screenDisplayID = 5, .reconfiguredDisplayID = 5, .displayWasAdded = true }), DisplayLinkReconfiguration::None);
    // Display 0's link was made for another main display, even if the rate changed too.
    EXPECT_EQ(reconfiguration({ .hasLink = true, .screenDisplayID = 7, .reconfiguredDisplayID = 7 }), DisplayLinkReconfiguration::ReplaceLink);
    EXPECT_EQ(reconfiguration({ .hasLink = true, .screenDisplayID = 7, .reconfiguredDisplayID = 7, .nominalRateChanged = true }), DisplayLinkReconfiguration::ReplaceLink);
    // The link's display was connected again; another display was.
    EXPECT_EQ(reconfiguration({ .hasLink = true, .screenDisplayID = 5, .reconfiguredDisplayID = 5, .displayWasAdded = true }), DisplayLinkReconfiguration::ReplaceLink);
    EXPECT_EQ(reconfiguration({ .hasLink = true, .screenDisplayID = 5, .reconfiguredDisplayID = 7, .displayWasAdded = true }), DisplayLinkReconfiguration::None);
    // A mode change.
    EXPECT_EQ(reconfiguration({ .hasLink = true, .screenDisplayID = 5, .reconfiguredDisplayID = 5, .nominalRateChanged = true }), DisplayLinkReconfiguration::NominalRateChanged);
    // Nothing that concerns the link.
    EXPECT_EQ(reconfiguration({ .hasLink = true, .screenDisplayID = 5, .reconfiguredDisplayID = 7 }), DisplayLinkReconfiguration::None);
}

} // namespace TestWebKitAPI

#endif // PLATFORM(MAC)
