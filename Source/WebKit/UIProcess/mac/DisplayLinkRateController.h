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

#pragma once

#if PLATFORM(MAC)

#include <CoreGraphics/CGDirectDisplay.h>
#include <WebCore/AnimationFrameRate.h>
#include <WebCore/DisplayUpdate.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>

// The rate logic of the UI process's DisplayLink with a platform display link that can fire below the display's rate
// (the CADisplayLink backend). Mac only. It is all in this header, which TestWebKitAPI gets in WebKitTestSupport, so
// that it can be tested without exporting anything from WebKit.

namespace WebKit {

// The divisor k, in vsyncs of a display's nominal rate between ticks, at which a display link that can fire every k
// vsyncs serves every demand; k needn't divide the rate (240 Hz over 7 is 34.3 fps). Each demand gets
// max(1, floor(nominal / demand + 0.05)), the largest k whose rate, nominal / k, is at least demand / (1 + 0.05 * demand
// / nominal), and a demand of 0 gets 1 (every tick). The link runs at the greatest common divisor of those, of which each
// demand's divisor is a multiple.
unsigned displayLinkFrameRateDivisor(WebCore::FramesPerSecond nominalFramesPerSecond, std::span<const WebCore::FramesPerSecond> demands);

// Counts the updates of a display link that can fire below the display's rate, every `divisor` vsyncs, at the rate it
// actually fires at. DisplayUpdate::relevantForUpdateFrequency() then gives each observer every n-th tick of the link,
// and the WebProcess's DisplayRefreshMonitor agrees. An observer whose rate doesn't divide the link's gets every tick:
// at 165 Hz, a page at 82 fps gets 82.5 fps, where with CVDisplayLink, it gets every tick of the display.
class DisplayLinkUpdateCounter {
public:
    // The next tick starts the count again from index 0, and the detection of another divisor, when the display link
    // starts or resumes.
    void reset()
    {
        m_countingDivisor = 0;
        m_requestedDivisor = 0;
        m_vsyncsAtAnotherDivisor = 0;
        m_firesAtAnotherDivisor = false;
    }

    // The update for a tick of a display link that fires every `linkDivisor` vsyncs of a display at
    // `nominalFramesPerSecond`, `vsyncsSincePreviousTick` vsyncs after the previous tick, while it's asked to fire every
    // `requestedDivisor` vsyncs (0 when it isn't asked for a divisor of that rate).
    WebCore::DisplayUpdate tick(unsigned linkDivisor, unsigned requestedDivisor, WebCore::FramesPerSecond nominalFramesPerSecond, unsigned vsyncsSincePreviousTick);

    // Whether the link has kept firing at another divisor than the requested one for 100 ms since the request.
    bool firesAtAnotherDivisor() const { return m_firesAtAnotherDivisor; }

private:
    WebCore::DisplayUpdate m_update;
    WebCore::FramesPerSecond m_nominalFramesPerSecond { 0 };
    unsigned m_countingDivisor { 0 };
    unsigned m_candidateDivisor { 0 };
    unsigned m_requestedDivisor { 0 };
    unsigned m_vsyncsAtAnotherDivisor { 0 };
    bool m_firesAtAnotherDivisor { false };
};

// The divisors a display link didn't get from the system, which it doesn't ask for again until clear(): asking for
// one gives divisor 1, the display's rate. Divisors of 63 and more (below 4 fps at 240 Hz) are denied together.
class DisplayLinkDeniedDivisors {
public:
    void deny(unsigned divisor) { m_divisors |= bit(divisor); }
    bool isDenied(unsigned divisor) const { return m_divisors & bit(divisor); }
    unsigned divisorToApply(unsigned requestedDivisor) const { return isDenied(requestedDivisor) ? 1 : std::max(1u, requestedDivisor); }
    void clear() { m_divisors = 0; }

private:
    static uint64_t bit(unsigned divisor) { return uint64_t { 1 } << std::min(divisor, 63u); }

    uint64_t m_divisors { 0 };
};

// What the UI process's CADisplayLink backend asks its link for, and the updates of the link's ticks: DisplayLink asks
// for a divisor of the nominal rate, and Core Animation may fire the link at another one. Used on the link's thread.
class DisplayLinkRateController {
public:
    // The preferred rate to ask the link for, for a request of `requestedDivisor` of `nominalFramesPerSecond`: that
    // rate, or 0 for the display's rate, for divisor 1 or a divisor the link didn't get before. std::nullopt when the link
    // was already asked for it.
    std::optional<double> preferredFramesPerSecond(unsigned requestedDivisor, WebCore::FramesPerSecond nominalFramesPerSecond);

    struct Tick {
        WebCore::DisplayUpdate update;
        // The divisors of the display's rate the link fires at, and was asked for (0 when the request was for another
        // mode of the display).
        unsigned linkDivisor { 1 };
        unsigned requestedDivisor { 1 };
        // The link kept firing at another rate than the one asked for for 100 ms: the request is denied, and asking
        // again runs the link at the display's rate.
        bool requestDenied { false };
    };
    // A tick of the link at `timestamp`, reporting `targetTimestamp` for the next one and the display's refresh interval,
    // `duration` (0 when it doesn't know it). `nominalFramesPerSecond` is the rate requests divide.
    Tick tick(double timestamp, double targetTimestamp, double duration, WebCore::FramesPerSecond nominalFramesPerSecond);

    // The link resumes: the count starts again from 0, and the divisors it didn't get are asked for again.
    void linkResumed()
    {
        m_counter.reset();
        m_deniedDivisors.clear();
    }
    // The nominal rate changed: the divisors the link didn't get at the old rate are asked for again.
    void nominalRateChanged() { m_deniedDivisors.clear(); }

    unsigned appliedDivisor() const { return m_appliedDivisor; }

private:
    DisplayLinkUpdateCounter m_counter;
    DisplayLinkDeniedDivisors m_deniedDivisors;
    // The divisor the link was asked for, and the nominal rate it divides.
    unsigned m_appliedDivisor { 1 };
    WebCore::FramesPerSecond m_appliedNominalFramesPerSecond { 0 };
    double m_previousTimestamp { 0 };
};

// What the CADisplayLink backend of a display does at the end of a reconfiguration of `reconfiguredDisplayID`, given
// its display's screen, if it has one, and the display its link was made for.
enum class DisplayLinkReconfiguration : uint8_t {
    None,
    // The screen is gone: invalidate the link, and stop.
    RemoveLink,
    // The screen is back, and the backend was running or was started meanwhile: start again, with a new link.
    Restart,
    // The link was made for another screen (display 0 follows the main display), or its display was connected again:
    // make a new link for the screen.
    ReplaceLink,
    // The display's rate changed: DisplayLink asks for a divisor of the new rate.
    NominalRateChanged,
};
struct DisplayLinkReconfigurationState {
    bool hasLink { false };
    // The backend was running when its display went away, or was started while it had no screen.
    bool restartWhenScreenReturns { false };
    std::optional<CGDirectDisplayID> screenDisplayID;
    CGDirectDisplayID linkDisplayID { 0 };
    CGDirectDisplayID reconfiguredDisplayID { 0 };
    bool displayWasAdded { false };
    bool nominalRateChanged { false };
};
DisplayLinkReconfiguration displayLinkReconfiguration(const DisplayLinkReconfigurationState&);

inline unsigned displayLinkFrameRateDivisor(WebCore::FramesPerSecond nominalFramesPerSecond, std::span<const WebCore::FramesPerSecond> demands)
{
    if (!nominalFramesPerSecond)
        return 1;

    unsigned divisor = 0;
    for (auto demand : demands) {
        unsigned demandDivisor = demand ? std::max(1u, static_cast<unsigned>(std::floor(static_cast<double>(nominalFramesPerSecond) / demand + 0.05))) : 1;
        divisor = std::gcd(divisor, demandDivisor);
    }
    return divisor ?: 1;
}

inline WebCore::DisplayUpdate DisplayLinkUpdateCounter::tick(unsigned linkDivisor, unsigned requestedDivisor, WebCore::FramesPerSecond nominalFramesPerSecond, unsigned vsyncsSincePreviousTick)
{
    linkDivisor = std::max(1u, linkDivisor);
    vsyncsSincePreviousTick = std::max(1u, vsyncsSincePreviousTick);
    WebCore::FramesPerSecond framesPerSecond = std::max<WebCore::FramesPerSecond>(1, nominalFramesPerSecond / linkDivisor);

    if (requestedDivisor != m_requestedDivisor) {
        m_requestedDivisor = requestedDivisor;
        m_vsyncsAtAnotherDivisor = 0;
        m_firesAtAnotherDivisor = false;
    }

    if (!m_countingDivisor) {
        m_countingDivisor = linkDivisor;
        m_candidateDivisor = 0;
        m_nominalFramesPerSecond = nominalFramesPerSecond;
        m_update = { 0, framesPerSecond };
    } else if (linkDivisor == m_countingDivisor && nominalFramesPerSecond == m_nominalFramesPerSecond) {
        m_candidateDivisor = 0;
        m_update = m_update.nextUpdate();
    } else if (linkDivisor == requestedDivisor || linkDivisor == m_candidateDivisor || nominalFramesPerSecond != m_nominalFramesPerSecond) {
        // The link now fires at another rate: the requested one, or the same one twice in a row. Count on at the new
        // rate from the time of this tick, instead of starting again at 0, where every observer is due: the index
        // nearest to the previous tick's position plus the vsyncs since then, but after the previous tick's index,
        // which may have been due already, and not after the first index at or after the old rate's next tick, so that
        // no observer loses an update. When the first tick at the new rate comes on time, observers then keep their
        // cadence exactly if the link speeds up to the display's rate, and otherwise shift once by less than a tick of
        // the new rate, at most half a tick late when it speeds up. A late first tick makes them later by that much.
        auto previousUpdate = m_update;
        uint64_t index;
        if (nominalFramesPerSecond == m_nominalFramesPerSecond) {
            uint64_t previousVSync = static_cast<uint64_t>(previousUpdate.updateIndex) * m_countingDivisor;
            index = std::max(previousVSync / linkDivisor + 1, (previousVSync + vsyncsSincePreviousTick + linkDivisor / 2) / linkDivisor);
            index = std::min(index, (previousVSync + m_countingDivisor + linkDivisor - 1) / linkDivisor);
        } else {
            // The same, in ticks of the new rate: the previous index is at previousIndex * framesPerSecond / previousRate.
            uint64_t previousRate = previousUpdate.updatesPerSecond;
            uint64_t scaledIndex = static_cast<uint64_t>(previousUpdate.updateIndex) * framesPerSecond;
            uint64_t nearestIndex = (2 * (scaledIndex * linkDivisor + vsyncsSincePreviousTick * previousRate) + previousRate * linkDivisor) / (2 * previousRate * linkDivisor);
            index = std::max(scaledIndex / previousRate + 1, nearestIndex);
            index = std::min(index, (scaledIndex + framesPerSecond + previousRate - 1) / previousRate);
        }
        m_countingDivisor = linkDivisor;
        m_candidateDivisor = 0;
        m_nominalFramesPerSecond = nominalFramesPerSecond;
        m_update = { static_cast<unsigned>(index % framesPerSecond), framesPerSecond };
    } else {
        // Another divisor for a single tick, like the stray interval a CADisplayLink reports while the display
        // changes mode: count on at the current rate unless the next tick has it too.
        m_candidateDivisor = linkDivisor;
        m_update = m_update.nextUpdate();
    }

    if (m_countingDivisor == m_requestedDivisor)
        m_vsyncsAtAnotherDivisor = 0;
    else if ((m_vsyncsAtAnotherDivisor += linkDivisor) >= std::max(1u, m_nominalFramesPerSecond / 10))
        m_firesAtAnotherDivisor = true;

    return m_update;
}

inline std::optional<double> DisplayLinkRateController::preferredFramesPerSecond(unsigned requestedDivisor, WebCore::FramesPerSecond nominalFramesPerSecond)
{
    auto divisor = m_deniedDivisors.divisorToApply(requestedDivisor);
    if (divisor == m_appliedDivisor && (divisor == 1 || nominalFramesPerSecond == m_appliedNominalFramesPerSecond))
        return std::nullopt;
    m_appliedDivisor = divisor;
    m_appliedNominalFramesPerSecond = nominalFramesPerSecond;
    return divisor > 1 ? static_cast<double>(nominalFramesPerSecond) / divisor : 0;
}

inline auto DisplayLinkRateController::tick(double timestamp, double targetTimestamp, double duration, WebCore::FramesPerSecond nominalFramesPerSecond) -> Tick
{
    Tick result;
    // `duration` is the display's refresh interval whatever rate the link fires at: the link fires every `linkDivisor`
    // vsyncs, and the display's rate is the one it reports, from the first tick after it reports a new mode. The vsyncs
    // since the previous tick place this tick when the link changes rate.
    unsigned vsyncsSincePreviousTick = 1;
    WebCore::FramesPerSecond refreshFramesPerSecond = nominalFramesPerSecond;
    if (duration > 0) {
        result.linkDivisor = std::max(1l, std::lround((targetTimestamp - timestamp) / duration));
        vsyncsSincePreviousTick = m_previousTimestamp ? std::max(1l, std::lround((timestamp - m_previousTimestamp) / duration)) : result.linkDivisor;
        refreshFramesPerSecond = std::max(1l, std::lround(1 / duration));
    }
    m_previousTimestamp = timestamp;

    // A request made for another mode than the display's: until the link is asked again for this mode, the rate it
    // fires at is counted at, but isn't taken as a denial, since it needn't be a divisor of this mode's rate.
    bool requestIsForAnotherMode = m_appliedDivisor > 1 && refreshFramesPerSecond != m_appliedNominalFramesPerSecond;
    result.requestedDivisor = requestIsForAnotherMode ? 0 : m_appliedDivisor;
    result.update = m_counter.tick(result.linkDivisor, result.requestedDivisor, refreshFramesPerSecond, vsyncsSincePreviousTick);

    // Counting at the display's rate gives observers the same ticks as with CVDisplayLink, where counting at a rate Core
    // Animation chose would give them others: every tick of either rate, for a demand that doesn't divide it.
    if (m_counter.firesAtAnotherDivisor() && result.requestedDivisor > 1) {
        m_deniedDivisors.deny(m_appliedDivisor);
        result.requestDenied = true;
    }
    return result;
}

inline DisplayLinkReconfiguration displayLinkReconfiguration(const DisplayLinkReconfigurationState& state)
{
    if (!state.screenDisplayID)
        return state.hasLink ? DisplayLinkReconfiguration::RemoveLink : DisplayLinkReconfiguration::None;
    if (!state.hasLink)
        return state.restartWhenScreenReturns ? DisplayLinkReconfiguration::Restart : DisplayLinkReconfiguration::None;
    if (*state.screenDisplayID != state.linkDisplayID || (state.reconfiguredDisplayID == *state.screenDisplayID && state.displayWasAdded))
        return DisplayLinkReconfiguration::ReplaceLink;
    return state.nominalRateChanged ? DisplayLinkReconfiguration::NominalRateChanged : DisplayLinkReconfiguration::None;
}

} // namespace WebKit

#endif // PLATFORM(MAC)
