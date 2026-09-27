/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "config.h"
#include "AnimationFrameRate.h"
#include <numeric>
#include <wtf/Vector.h>
#include <wtf/text/TextStream.h>

namespace WebCore {

static constexpr OptionSet<ThrottlingReason> halfSpeedThrottlingReasons { ThrottlingReason::LowPowerMode, ThrottlingReason::NonInteractedCrossOriginFrame, ThrottlingReason::VisuallyIdle, ThrottlingReason::AggressiveThermalMitigation };

FramesPerSecond framesPerSecondNearestFullSpeed(FramesPerSecond nominalFramesPerSecond)
{
    if (nominalFramesPerSecond <= FullSpeedFramesPerSecond)
        return nominalFramesPerSecond;

    float fullSpeedRatio = nominalFramesPerSecond / FullSpeedFramesPerSecond;
    FramesPerSecond floorSpeed = nominalFramesPerSecond / std::floor(fullSpeedRatio);
    FramesPerSecond ceilSpeed = nominalFramesPerSecond / std::ceil(fullSpeedRatio);

    return fullSpeedRatio - std::floor(fullSpeedRatio) <= 0.5 ? floorSpeed : ceilSpeed;
}

std::optional<FramesPerSecond> preferredFramesPerSecond(OptionSet<ThrottlingReason> reasons, std::optional<FramesPerSecond> nominalFramesPerSecond, bool preferFrameRatesNear60FPS)
{
    if (reasons.contains(ThrottlingReason::OutsideViewport))
        return std::nullopt;

    if (!nominalFramesPerSecond || *nominalFramesPerSecond == FullSpeedFramesPerSecond) {
        // FIXME: handle ThrottlingReason::VisuallyIdle
        if (reasons.containsAny(halfSpeedThrottlingReasons))
            return HalfSpeedThrottlingFramesPerSecond;

        return FullSpeedFramesPerSecond;
    }

    auto framesPerSecond = preferFrameRatesNear60FPS ? framesPerSecondNearestFullSpeed(*nominalFramesPerSecond) : *nominalFramesPerSecond;
    if (reasons.containsAny(halfSpeedThrottlingReasons))
        framesPerSecond /= IntervalThrottlingFactor;

    return framesPerSecond;
}

Seconds preferredFrameInterval(OptionSet<ThrottlingReason> reasons, std::optional<FramesPerSecond> nominalFramesPerSecond, bool preferFrameRatesNear60FPS)
{
    if (reasons.contains(ThrottlingReason::OutsideViewport))
        return AggressiveThrottlingAnimationInterval;

    if (!nominalFramesPerSecond || *nominalFramesPerSecond == FullSpeedFramesPerSecond) {
        // FIXME: handle ThrottlingReason::VisuallyIdle
        if (reasons.containsAny(halfSpeedThrottlingReasons))
            return HalfSpeedThrottlingAnimationInterval;
        return FullSpeedAnimationInterval;
    }

    auto framesPerSecond = preferFrameRatesNear60FPS ? framesPerSecondNearestFullSpeed(*nominalFramesPerSecond) : *nominalFramesPerSecond;
    auto interval = Seconds(1.0 / framesPerSecond);

    if (reasons.containsAny(halfSpeedThrottlingReasons))
        interval *= IntervalThrottlingFactor;

    return interval;
}

FramesPerSecond preferredFramesPerSecondFromInterval(Seconds preferredFrameInterval)
{
    if (preferredFrameInterval == FullSpeedAnimationInterval)
        return FullSpeedFramesPerSecond;

    if (preferredFrameInterval == HalfSpeedThrottlingAnimationInterval)
        return HalfSpeedThrottlingFramesPerSecond;

    return std::round(1 / preferredFrameInterval.seconds());
}

// Returns the divisor K of the nominal rate (the platform display link runs at nominal / K) that serves every
// demand. Each demand d gets the largest divisor k whose rate nominal / k is at least d. The link runs at the greatest common
// divisor of those, so that every observer gets an exact cadence, unless more than half of the ticks at that
// rate would be unused; then it runs at the fastest demand, and slower observers get the nearest ticks.
// There is deliberately no cap: the link runs as fast as the content asks.
unsigned displayLinkFrameRateDivisor(FramesPerSecond nominalFramesPerSecond, std::span<const FramesPerSecond> demands)
{
    Vector<unsigned, 4> divisors;
    for (auto demand : demands) {
        // The slowest rung that is at least the demand; the tolerance maps rates rounded up to whole numbers
        // (165 / 2 = 82.5 sent as 83) to their own divisor, as rates rounded down (82) already do.
        if (demand)
            divisors.append(std::max(1u, static_cast<unsigned>(std::floor(static_cast<double>(nominalFramesPerSecond) / demand + 0.05))));
    }
    if (!nominalFramesPerSecond || divisors.isEmpty())
        return 1;

    unsigned greatestCommonDivisor = 0;
    for (auto divisor : divisors)
        greatestCommonDivisor = std::gcd(greatestCommonDivisor, divisor);
    unsigned fastestDivisor = *std::min_element(divisors.begin(), divisors.end());
    if (greatestCommonDivisor == fastestDivisor)
        return fastestDivisor;

    // Fraction of the ticks at the greatest common divisor that are due for at least one demand, over one cycle.
    constexpr unsigned maximumCycle = 10000;
    unsigned cycle = 1;
    for (auto divisor : divisors) {
        cycle = std::lcm(cycle, divisor);
        if (cycle > maximumCycle)
            break;
    }
    unsigned ticks = std::min(cycle, maximumCycle) / greatestCommonDivisor;
    unsigned dueTicks = 0;
    for (unsigned tick = 0; tick < ticks; ++tick) {
        unsigned vsync = tick * greatestCommonDivisor;
        if (std::ranges::any_of(divisors, [&](unsigned divisor) { return !(vsync % divisor); }))
            ++dueTicks;
    }
    return 2 * dueTicks >= ticks ? greatestCommonDivisor : fastestDivisor;
}

TextStream& operator<<(TextStream& ts, const OptionSet<ThrottlingReason>& reasons)
{
    bool didAppend = false;

    for (auto reason : reasons) {
        if (didAppend)
            ts << '|';
        switch (reason) {
        case ThrottlingReason::VisuallyIdle:
            ts << "VisuallyIdle"_s;
            break;
        case ThrottlingReason::OutsideViewport:
            ts << "OutsideViewport"_s;
            break;
        case ThrottlingReason::LowPowerMode:
            ts << "LowPowerMode"_s;
            break;
        case ThrottlingReason::NonInteractedCrossOriginFrame:
            ts << "NonInteractedCrossOriginFrame"_s;
            break;
        case ThrottlingReason::ThermalMitigation:
            ts << "ThermalMitigation"_s;
            break;
        case ThrottlingReason::AggressiveThermalMitigation:
            ts << "AggressiveThermalMitigation"_s;
            break;
        }
        didAppend = true;
    }

    if (reasons.isEmpty())
        ts << "[Unthrottled]"_s;
    return ts;
}
} // namespace WebCore
