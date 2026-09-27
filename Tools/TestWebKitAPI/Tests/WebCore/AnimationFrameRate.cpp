/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
#include <WebCore/AnimationFrameRate.h>
#include <WebCore/DisplayUpdate.h>
#include <wtf/Vector.h>

using namespace WebCore;

namespace TestWebKitAPI {

constexpr bool preferredFramesPerSecondTarget60FPSSetting = true;
constexpr bool preferredFramesPerSecondMatchNominalFrameRateSetting = false;

TEST(AnimationFrameRate, framesPerSecondNearestFullSpeed)
{
    ASSERT_EQ(framesPerSecondNearestFullSpeed(240), FramesPerSecond(60));
    ASSERT_EQ(framesPerSecondNearestFullSpeed(144), FramesPerSecond(72));
    ASSERT_EQ(framesPerSecondNearestFullSpeed(120), FramesPerSecond(60));
    ASSERT_EQ(framesPerSecondNearestFullSpeed(90), FramesPerSecond(90));
    ASSERT_EQ(framesPerSecondNearestFullSpeed(60), FramesPerSecond(60));
    ASSERT_EQ(framesPerSecondNearestFullSpeed(48), FramesPerSecond(48));
    ASSERT_EQ(framesPerSecondNearestFullSpeed(30), FramesPerSecond(30));
}

TEST(AnimationFrameRate, preferredFrameIntervalWithUnspecifiedNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;
    std::optional<FramesPerSecond> unspecifiedNominalFramesPerSecond;
    
    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), FullSpeedAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), FullSpeedAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, unspecifiedNominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFrameIntervalWithFullSpeedNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;

    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), FullSpeedAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), FullSpeedAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), HalfSpeedThrottlingAnimationInterval);
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, FullSpeedFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFrameIntervalWith144FPSNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;
    FramesPerSecond nominalFramesPerSecond = 144;

    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 144));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 144));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 144));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 144));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 144));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 72));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 72));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 72));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 72));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 72));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFrameIntervalWith120FPSNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;
    FramesPerSecond nominalFramesPerSecond = 120;

    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 120));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 120));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 120));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 120));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 120));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 60));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 60));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 60));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 60));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 60));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFrameIntervalWith90FPSNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;
    FramesPerSecond nominalFramesPerSecond = 90;

    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 90));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFrameIntervalWith48FPSNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;
    FramesPerSecond nominalFramesPerSecond = 48;

    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 48));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFrameIntervalWith30FPSNominalFramesPerSecond)
{
    OptionSet<ThrottlingReason> noThrottling;
    FramesPerSecond nominalFramesPerSecond = 30;

    bool preferredFramesPerSecondNear60FPS = preferredFramesPerSecondMatchNominalFrameRateSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);

    preferredFramesPerSecondNear60FPS = preferredFramesPerSecondTarget60FPSSetting;
    ASSERT_EQ(preferredFrameInterval(noThrottling, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(1.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::LowPowerMode }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::AggressiveThermalMitigation }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::VisuallyIdle }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::NonInteractedCrossOriginFrame }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), Seconds(2.0 / 30));
    ASSERT_EQ(preferredFrameInterval({ ThrottlingReason::OutsideViewport }, nominalFramesPerSecond, preferredFramesPerSecondNear60FPS), AggressiveThrottlingAnimationInterval);
}

TEST(AnimationFrameRate, preferredFramesPerSecondMatchNominalFrameRate)
{
    ASSERT_EQ(preferredFramesPerSecond({ }, FullSpeedFramesPerSecond, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FullSpeedFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecond({ }, 120, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(120));
    ASSERT_EQ(preferredFramesPerSecond({ }, 90, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(90));
    ASSERT_EQ(preferredFramesPerSecond({ }, 50, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(50));

    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, FullSpeedFramesPerSecond, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), HalfSpeedThrottlingFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, 120, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(60));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, 90, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(45));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, 50, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(25));

    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, FullSpeedFramesPerSecond, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), HalfSpeedThrottlingFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, 120, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(60));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, 90, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(45));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, 50, preferredFramesPerSecondMatchNominalFrameRateSetting).value(), FramesPerSecond(25));

    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::OutsideViewport }, FullSpeedFramesPerSecond, preferredFramesPerSecondMatchNominalFrameRateSetting), std::nullopt);
}

TEST(AnimationFrameRate, preferredFramesPerSecondTarget60FPS)
{
    ASSERT_EQ(preferredFramesPerSecond({ }, FullSpeedFramesPerSecond, preferredFramesPerSecondTarget60FPSSetting).value(), FullSpeedFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecond({ }, 120, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(60));
    ASSERT_EQ(preferredFramesPerSecond({ }, 90, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(90));
    ASSERT_EQ(preferredFramesPerSecond({ }, 50, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(50));

    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, FullSpeedFramesPerSecond, preferredFramesPerSecondTarget60FPSSetting).value(), HalfSpeedThrottlingFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, 120, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(30));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, 90, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(45));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::LowPowerMode }, 50, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(25));

    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, FullSpeedFramesPerSecond, preferredFramesPerSecondTarget60FPSSetting).value(), HalfSpeedThrottlingFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, 120, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(30));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, 90, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(45));
    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::AggressiveThermalMitigation }, 50, preferredFramesPerSecondTarget60FPSSetting).value(), FramesPerSecond(25));

    ASSERT_EQ(preferredFramesPerSecond({ ThrottlingReason::OutsideViewport }, FullSpeedFramesPerSecond, preferredFramesPerSecondTarget60FPSSetting), std::nullopt);
}

TEST(AnimationFrameRate, preferredFramesPerSecondFromInterval)
{
    ASSERT_EQ(preferredFramesPerSecondFromInterval(FullSpeedAnimationInterval), FullSpeedFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecondFromInterval(HalfSpeedThrottlingAnimationInterval), HalfSpeedThrottlingFramesPerSecond);
    ASSERT_EQ(preferredFramesPerSecondFromInterval(Seconds(1.0 / 60)), FramesPerSecond(60));
}

TEST(AnimationFrameRate, displayUpdateRelevancy)
{
    auto frame0 = DisplayUpdate { 0, FullSpeedFramesPerSecond };
    auto frame1 = DisplayUpdate { 1, FullSpeedFramesPerSecond };
    auto frame2 = DisplayUpdate { 2, FullSpeedFramesPerSecond };
    auto frame3 = DisplayUpdate { 3, FullSpeedFramesPerSecond };
    auto frame4 = DisplayUpdate { 4, FullSpeedFramesPerSecond };

    auto quarterSpeedFrameRate = HalfSpeedThrottlingFramesPerSecond / 2;

    ASSERT_TRUE(frame0.relevantForUpdateFrequency(FullSpeedFramesPerSecond));
    ASSERT_TRUE(frame0.relevantForUpdateFrequency(HalfSpeedThrottlingFramesPerSecond));
    ASSERT_TRUE(frame0.relevantForUpdateFrequency(quarterSpeedFrameRate));

    ASSERT_TRUE(frame1.relevantForUpdateFrequency(FullSpeedFramesPerSecond));
    ASSERT_FALSE(frame1.relevantForUpdateFrequency(HalfSpeedThrottlingFramesPerSecond));
    ASSERT_FALSE(frame1.relevantForUpdateFrequency(quarterSpeedFrameRate));

    ASSERT_TRUE(frame2.relevantForUpdateFrequency(FullSpeedFramesPerSecond));
    ASSERT_TRUE(frame2.relevantForUpdateFrequency(HalfSpeedThrottlingFramesPerSecond));
    ASSERT_FALSE(frame2.relevantForUpdateFrequency(quarterSpeedFrameRate));

    ASSERT_TRUE(frame3.relevantForUpdateFrequency(FullSpeedFramesPerSecond));
    ASSERT_FALSE(frame3.relevantForUpdateFrequency(HalfSpeedThrottlingFramesPerSecond));
    ASSERT_FALSE(frame3.relevantForUpdateFrequency(quarterSpeedFrameRate));

    ASSERT_TRUE(frame4.relevantForUpdateFrequency(FullSpeedFramesPerSecond));
    ASSERT_TRUE(frame4.relevantForUpdateFrequency(HalfSpeedThrottlingFramesPerSecond));
    ASSERT_TRUE(frame4.relevantForUpdateFrequency(quarterSpeedFrameRate));
}

static unsigned divisor(FramesPerSecond nominalFramesPerSecond, Vector<FramesPerSecond> demands)
{
    return displayLinkFrameRateDivisor(nominalFramesPerSecond, demands.span());
}

TEST(AnimationFrameRate, displayLinkFrameRateDivisorSingleDemand)
{
    // A page rendering at 60 fps doesn't need the display link to fire faster than 60 fps, whatever the display's rate.
    ASSERT_EQ(divisor(60, { 60 }), 1u);
    ASSERT_EQ(divisor(120, { 60 }), 2u);
    ASSERT_EQ(divisor(240, { 60 }), 4u);
    // The slowest rate that is at least the demand.
    ASSERT_EQ(divisor(144, { 60 }), 2u); // 72 fps
    ASSERT_EQ(divisor(100, { 60 }), 1u); // 50 fps would be too slow.
    ASSERT_EQ(divisor(165, { 60 }), 2u); // 82.5 fps
    // Throttled pages.
    ASSERT_EQ(divisor(60, { 30 }), 2u);
    ASSERT_EQ(divisor(120, { 30 }), 4u);
    ASSERT_EQ(divisor(240, { 30 }), 8u);
    ASSERT_EQ(divisor(120, { 10 }), 12u);
}

TEST(AnimationFrameRate, displayLinkFrameRateDivisorNoCap)
{
    // Content that asks for the display's full rate gets it: there is no fixed cap.
    ASSERT_EQ(divisor(240, { 240 }), 1u);
    ASSERT_EQ(divisor(240, { 120 }), 2u);
    ASSERT_EQ(divisor(120, { 120 }), 1u);
    ASSERT_EQ(divisor(240, { 1000 }), 1u);
}

TEST(AnimationFrameRate, displayLinkFrameRateDivisorWholeNumberDemands)
{
    // Rates sent as whole numbers map to their own divisor, whether they were rounded down or up.
    ASSERT_EQ(divisor(165, { 82 }), 2u); // 82.5 fps
    ASSERT_EQ(divisor(165, { 83 }), 2u);
    ASSERT_EQ(divisor(175, { 58 }), 3u); // 58.33 fps
    ASSERT_EQ(divisor(175, { 59 }), 3u);
    ASSERT_EQ(divisor(144, { 72 }), 2u);
    ASSERT_EQ(divisor(144, { 73 }), 2u);
    // Beyond the tolerance, the next faster rate.
    ASSERT_EQ(divisor(144, { 76 }), 1u);
}

TEST(AnimationFrameRate, displayLinkFrameRateDivisorSeveralDemands)
{
    // The display link fires at a rate that gives every demand an exact cadence.
    ASSERT_EQ(divisor(240, { 60, 240 }), 1u); // A page and live scrolling.
    ASSERT_EQ(divisor(240, { 60, 30 }), 4u);
    ASSERT_EQ(divisor(120, { 60, 30, 10 }), 2u);
    // Unless most of its ticks would be unused; then it fires at the fastest demand.
    ASSERT_EQ(divisor(240, { 60, 48 }), 4u);
    // 60 and 40 on 120 Hz: 120 fps uses two thirds of the ticks.
    ASSERT_EQ(divisor(120, { 60, 40 }), 1u);
    // 40 and 30 on 120 Hz: 120 fps uses exactly half of the ticks, which is still enough.
    ASSERT_EQ(divisor(120, { 40, 30 }), 1u);
}

TEST(AnimationFrameRate, displayLinkFrameRateDivisorNoPreference)
{
    // A demand of 0 has no rate preference and doesn't make the display link fire faster.
    ASSERT_EQ(divisor(120, { 0, 60 }), 2u);
    ASSERT_EQ(divisor(120, { 0, 30 }), 4u);
    // With no preference at all, the full rate; DisplayLink substitutes its idle rate before asking.
    ASSERT_EQ(divisor(120, { 0 }), 1u);
    ASSERT_EQ(divisor(120, { }), 1u);
    ASSERT_EQ(divisor(0, { 60 }), 1u);
}

} // namespace TestWebKitAPI
