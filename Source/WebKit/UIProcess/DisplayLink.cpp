/*
 * Copyright (C) 2018-2024 Apple Inc. All rights reserved.
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
#include "DisplayLink.h"

#if HAVE(DISPLAY_LINK)

#include "Logging.h"
#include <WebCore/AnimationFrameRate.h>
#include <numeric>
#include <wtf/RunLoop.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebKit {

using namespace WebCore;

constexpr unsigned maxFireCountWithoutObservers { 20 };
// With frame timing, also stop after this long without observers, since 20 ticks take long at low rates.
constexpr Seconds maxDurationWithoutObservers { 333_ms };
#if PLATFORM(MAC)
// The rate when observers are registered but none has a rate preference.
constexpr FramesPerSecond idleFramesPerSecond { 10 };
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(DisplayLink);
WTF_MAKE_TZONE_ALLOCATED_IMPL(DisplayLink::Client);

DisplayLink::DisplayLink(PlatformDisplayID displayID)
    : m_displayID(displayID)
{
    platformInitialize();

    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] Created DisplayLink " << this << " for display " << displayID << " with nominal fps " << m_displayNominalFramesPerSecond);
}

DisplayLink::~DisplayLink()
{
    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] Destroying DisplayLink " << this << " for display " << m_displayID);

    platformFinalize();
}

void DisplayLink::addObserver(Client& client, DisplayLinkObserverID observerID, FramesPerSecond preferredFramesPerSecond)
{
    ASSERT(RunLoop::isMain());

    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink " << this << " for display display " << m_displayID << " add observer " << observerID << " fps " << preferredFramesPerSecond);

    {
        Locker locker { m_clientsLock };
        m_clients.ensure(client, [] {
            return ClientInfo { };
        }).iterator->value.observers.append({ observerID, preferredFramesPerSecond });
#if PLATFORM(MAC)
        updatePlatformPreferredFramesPerSecond();
#endif
    }

    if (!platformIsRunning()) {
        LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink for display " << m_displayID << " starting DisplayLink with fps " << m_displayNominalFramesPerSecond);

        m_currentUpdate = { 0, m_displayNominalFramesPerSecond };

        platformStart();
    }
}

void DisplayLink::removeObserver(Client& client, DisplayLinkObserverID observerID)
{
    ASSERT(RunLoop::isMain());

    Locker locker { m_clientsLock };

    auto it = m_clients.find(client);
    if (it == m_clients.end())
        return;

    auto& clientInfo = it->value;

    bool removed = clientInfo.observers.removeFirstMatching([observerID](const auto& value) {
        return value.observerID == observerID;
    });
    ASSERT_UNUSED(removed, removed);

    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink " << this << " for display " << m_displayID << " remove observer " << observerID);

    removeInfoForClientIfUnused(client);
#if PLATFORM(MAC)
    updatePlatformPreferredFramesPerSecond();
#endif

    // We do not stop the display link right away when |m_clients| becomes empty. Instead, we
    // let the display link fire up to |maxFireCountWithoutObservers| times without observers to avoid
    // killing & restarting too many threads when observers gets removed & added in quick succession.
}

void DisplayLink::removeClient(Client& client)
{
    ASSERT(RunLoop::isMain());

    Locker locker { m_clientsLock };
    m_clients.remove(client);
#if PLATFORM(MAC)
    updatePlatformPreferredFramesPerSecond();
#endif

    // We do not stop the display link right away when |m_clients| becomes empty. Instead, we
    // let the display link fire up to |maxFireCountWithoutObservers| times without observers to avoid
    // killing & restarting too many threads when observers gets removed & added in quick succession.
}

bool DisplayLink::removeInfoForClientIfUnused(Client& client)
{
    auto it = m_clients.find(client);
    if (it == m_clients.end())
        return false;

    auto& clientInfo = it->value;
    if (clientInfo.observers.isEmpty() && !clientInfo.fullSpeedUpdatesClientCount) {
        m_clients.remove(it);
        return true;
    }
    return false;
}

void DisplayLink::incrementFullSpeedRequestClientCount(Client& client)
{
    Locker locker { m_clientsLock };

    auto& clientInfo = m_clients.ensure(client, [] {
        return ClientInfo { };
    }).iterator->value;

    ++clientInfo.fullSpeedUpdatesClientCount;
#if PLATFORM(MAC)
    updatePlatformPreferredFramesPerSecond();
#endif
}

void DisplayLink::decrementFullSpeedRequestClientCount(Client& client)
{
    Locker locker { m_clientsLock };

    auto it = m_clients.find(client);
    if (it == m_clients.end())
        return;

    auto& clientInfo = it->value;
    ASSERT(clientInfo.fullSpeedUpdatesClientCount);
    --clientInfo.fullSpeedUpdatesClientCount;
    removeInfoForClientIfUnused(client);
#if PLATFORM(MAC)
    updatePlatformPreferredFramesPerSecond();
#endif
}

bool DisplayLink::displayPropertiesChanged(bool displayWasAdded)
{
    ASSERT(RunLoop::isMain());
#if PLATFORM(MAC)
    RefPtr platformBackend = m_platformBackend;
    if (!platformBackend)
        return false;

    platformBackend->displayConfigurationChanged(displayWasAdded);
    auto nominalFramesPerSecond = platformBackend->nominalFramesPerSecond();

    Locker locker { m_clientsLock };
    if (nominalFramesPerSecond == m_displayNominalFramesPerSecond)
        return false;

    RELEASE_LOG(DisplayLink, "[UI ] DisplayLink for display %u nominal fps changed from %u to %u", m_displayID, m_displayNominalFramesPerSecond, nominalFramesPerSecond);
    m_displayNominalFramesPerSecond = nominalFramesPerSecond;
    // Without frame timing, updates are counted at the nominal rate.
    m_currentUpdate = { 0, nominalFramesPerSecond };
    // The same divisor is a different rate now.
    m_platformFrameRateDivisor = 0;
    updatePlatformPreferredFramesPerSecond();
    return true;
#else
    UNUSED_PARAM(displayWasAdded);
    return false;
#endif
}

void DisplayLink::setObserverPreferredFramesPerSecond(Client& client, DisplayLinkObserverID observerID, FramesPerSecond preferredFramesPerSecond)
{
    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink " << this << " setPreferredFramesPerSecond - display " << m_displayID << " observer " << observerID << " fps " << preferredFramesPerSecond);

    Locker locker { m_clientsLock };

    auto it = m_clients.find(client);
    if (it == m_clients.end())
        return;

    auto& clientInfo = it->value;
    auto index = clientInfo.observers.findIf([observerID](const auto& observer) {
        return observer.observerID == observerID;
    });

    if (index == notFound || clientInfo.observers[index].preferredFramesPerSecond == preferredFramesPerSecond)
        return;

    clientInfo.observers[index].preferredFramesPerSecond = preferredFramesPerSecond;
#if PLATFORM(MAC)
    updatePlatformPreferredFramesPerSecond();
#endif
}

// With frame timing, an observer at a given rate is due once the vsync time reaches its next update time
// (within half a tick of the display link). Its next update time then advances by one interval, so the cadence
// stays steady and in phase when the display link changes rate.
static bool isDueForUpdate(std::optional<MonotonicTime>& nextUpdateTime, Seconds& updateInterval, const DisplayLinkFrameTiming& timing, FramesPerSecond framesPerSecond)
{
    if (!framesPerSecond)
        return true;

    auto interval = 1_s / framesPerSecond;
    // Rates are whole numbers, often truncated from divisors of the refresh rate (165 / 2 = 82.5 is sent as 82),
    // and panels may be slightly off their nominal rate. When the interval is about a whole number of vsyncs, pace
    // on exactly those vsyncs, locked to the tick that fired, so that the due time doesn't drift against the ticks.
    // Other rates keep their exact interval and are met on average.
    unsigned vsyncsPerUpdate = 0;
    if (timing.refreshInterval > 0_s) {
        auto vsyncs = std::max(1.0, std::floor(interval / timing.refreshInterval + 0.05));
        auto vsyncInterval = timing.refreshInterval * vsyncs;
        if (std::abs((vsyncInterval - interval) / interval) < 0.05) {
            interval = vsyncInterval;
            vsyncsPerUpdate = static_cast<unsigned>(vsyncs);
        }
    }

    // When the rate went up, pull in the due time that was computed with the previous, longer interval.
    if (nextUpdateTime && interval < updateInterval)
        nextUpdateTime = *nextUpdateTime - (updateInterval - interval);
    updateInterval = interval;

    // Tolerate half a tick of the display link: divisors of the link rate still land on exact ticks, and when
    // the link slows down, the first tick of its new cadence isn't skipped just because it came slightly early.
    auto linkInterval = timing.linkInterval > 0_s ? timing.linkInterval : timing.refreshInterval;

    // Keep whole-vsync cadences on the vsync phase the platform display link uses when it runs at that cadence, so
    // that when it changes rate (at the end of a scroll, for example) its ticks keep landing on this cadence. Only
    // when that phase is known and consistent with the ticks the link fires now.
    std::optional<unsigned> vsyncPhase;
    if (vsyncsPerUpdate >= 2 && vsyncsPerUpdate <= DisplayLinkFrameTiming::maximumVSyncPhaseDivisor && timing.linkDivisor && !(vsyncsPerUpdate % timing.linkDivisor)) {
        if (auto phase = timing.vsyncPhases[vsyncsPerUpdate]; phase && *phase % timing.linkDivisor == timing.vsyncIndex % timing.linkDivisor)
            vsyncPhase = *phase;
    }
    if (vsyncPhase) {
        // Due times stay on the phase. A due tick fires even off the phase (when the link thread was late, Core
        // Animation skips the vsyncs it missed, including the on-phase one), and the next update goes back to the
        // phase, 0.5 to 1.5 intervals later. A new observer starts on the phase.
        auto vsyncsSincePhase = static_cast<unsigned>((timing.vsyncIndex + vsyncsPerUpdate - *vsyncPhase) % vsyncsPerUpdate);
        auto vsyncsToPhase = vsyncsPerUpdate - vsyncsSincePhase;
        if (!nextUpdateTime && vsyncsSincePhase) {
            nextUpdateTime = timing.vsyncTime + timing.refreshInterval * static_cast<double>(vsyncsToPhase);
            return false;
        }
        if (nextUpdateTime && timing.vsyncTime < *nextUpdateTime - linkInterval / 2)
            return false;
        if (2 * vsyncsToPhase <= vsyncsPerUpdate)
            vsyncsToPhase += vsyncsPerUpdate;
        nextUpdateTime = timing.vsyncTime + timing.refreshInterval * static_cast<double>(vsyncsToPhase);
        return true;
    }

    if (nextUpdateTime && timing.vsyncTime < *nextUpdateTime - linkInterval / 2)
        return false;

    // Don't try to catch up after a pause or a long frame.
    if (nextUpdateTime && !vsyncsPerUpdate && timing.vsyncTime - *nextUpdateTime < interval)
        nextUpdateTime = *nextUpdateTime + interval;
    else
        nextUpdateTime = timing.vsyncTime + interval;
    return true;
}

void DisplayLink::notifyObserversDisplayDidRefresh(std::optional<DisplayLinkFrameTiming> timing)
{
    ASSERT(!RunLoop::isMain());

    Locker locker { m_clientsLock };

    tracePoint(DisplayLinkUpdate);

    // When the platform display link runs below the nominal rate, count updates at the rate it actually fires.
    if (timing && timing->linkFramesPerSecond && timing->linkFramesPerSecond != m_currentUpdate.updatesPerSecond) {
        LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink " << this << " for display " << m_displayID << " now firing at " << timing->linkFramesPerSecond << " fps");
        m_currentUpdate = { 0, timing->linkFramesPerSecond };
    }

    auto maxFramesPerSecond = [](const Vector<ObserverInfo, 1>& observers) {
        std::optional<FramesPerSecond> observersMaxFramesPerSecond;
        for (const auto& observer : observers)
            observersMaxFramesPerSecond = std::max(observersMaxFramesPerSecond.value_or(0), observer.preferredFramesPerSecond);
        return observersMaxFramesPerSecond;
    };

    bool anyConnectionHadObservers = false;
    for (auto& [client, clientInfo] : m_clients) {
        if (clientInfo.observers.isEmpty())
            continue;

        anyConnectionHadObservers = true;

        auto observersMaxFramesPerSecond = maxFramesPerSecond(clientInfo.observers);
        auto clientFramesPerSecond = observersMaxFramesPerSecond.value_or(FullSpeedFramesPerSecond);
        bool anyObserverWantsCallback;
        auto displayUpdate = m_currentUpdate;
        if (timing) {
            anyObserverWantsCallback = isDueForUpdate(clientInfo.nextUpdateTime, clientInfo.updateInterval, *timing, clientFramesPerSecond);
            // Count this client's updates at its own rate, so that clients that decimate the update again with
            // DisplayUpdate::relevantForUpdateFrequency() (the WebProcess DisplayRefreshMonitor) agree with the decision.
            if (clientFramesPerSecond) {
                displayUpdate = { clientInfo.updateIndex % clientFramesPerSecond, clientFramesPerSecond };
                if (anyObserverWantsCallback)
                    clientInfo.updateIndex = displayUpdate.nextUpdate().updateIndex;
            }
        } else
            anyObserverWantsCallback = m_currentUpdate.relevantForUpdateFrequency(clientFramesPerSecond);

        LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink " << this << " for display " << m_displayID << " (display fps " << m_displayNominalFramesPerSecond << ") update " << m_currentUpdate << " " << clientInfo.observers.size()
            << " observers, maxFramesPerSecond " << observersMaxFramesPerSecond << " full speed client count " << clientInfo.fullSpeedUpdatesClientCount << " relevant " << anyObserverWantsCallback);

        if (clientInfo.fullSpeedUpdatesClientCount || anyObserverWantsCallback)
            CheckedRef { client }->displayLinkFired(m_displayID, displayUpdate, clientInfo.fullSpeedUpdatesClientCount, anyObserverWantsCallback);
    }

    m_currentUpdate = m_currentUpdate.nextUpdate();

    if (!anyConnectionHadObservers) {
        bool withoutObserversForLong = false;
        if (timing) {
            if (!m_firstTickWithoutObserversTime)
                m_firstTickWithoutObserversTime = timing->vsyncTime;
            withoutObserversForLong = timing->vsyncTime - *m_firstTickWithoutObserversTime >= maxDurationWithoutObservers;
        }
        if (++m_fireCountWithoutObservers >= maxFireCountWithoutObservers || withoutObserversForLong) {
            LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink for display " << m_displayID << " fired " << m_fireCountWithoutObservers << " times with no observers; stopping DisplayLink");
            m_firstTickWithoutObserversTime = std::nullopt;
            platformStop();
        }
        return;
    }
    m_fireCountWithoutObservers = 0;
    m_firstTickWithoutObserversTime = std::nullopt;
}

#if PLATFORM(MAC)
// Returns the divisor K of the nominal rate (the platform display link runs at nominal / K) that serves every
// demand. Each demand d gets the largest divisor k whose rate nominal / k is at least d. The link runs at the greatest common
// divisor of those, so that every observer gets an exact cadence, unless more than half of the ticks at that
// rate would be unused; then it runs at the fastest demand, and slower observers get the nearest ticks.
// There is deliberately no cap: the link runs as fast as the content asks.
static unsigned displayLinkFrameRateDivisor(FramesPerSecond nominalFramesPerSecond, std::span<const FramesPerSecond> demands)
{
    Vector<unsigned, 4> divisors;
    for (auto demand : demands) {
        // The slowest rung that is at least the demand; the tolerance maps rates truncated to whole numbers
        // (165 / 2 = 82.5 sent as 82) to their own divisor.
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

void DisplayLink::updatePlatformPreferredFramesPerSecond()
{
    ASSERT(RunLoop::isMain());

    Vector<FramesPerSecond, 4> demands;
    for (auto& [client, clientInfo] : m_clients) {
        if (clientInfo.observers.isEmpty())
            continue;
        if (clientInfo.fullSpeedUpdatesClientCount)
            demands.append(m_displayNominalFramesPerSecond);
        for (const auto& observer : clientInfo.observers)
            demands.append(observer.preferredFramesPerSecond);
    }

    // With no observers, keep the current rate: the display link stops on its own after a few idle ticks.
    if (demands.isEmpty())
        return;

    // Observers that have no rate preference don't need the display link to run fast.
    if (!std::ranges::any_of(demands, [](auto demand) { return demand > 0; }))
        demands = { idleFramesPerSecond };

    auto divisor = displayLinkFrameRateDivisor(m_displayNominalFramesPerSecond, demands.span());
    if (divisor == m_platformFrameRateDivisor)
        return;
    m_platformFrameRateDivisor = divisor;

    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] DisplayLink " << this << " for display " << m_displayID << " requesting " << static_cast<double>(m_displayNominalFramesPerSecond) / divisor << " fps (divisor " << divisor << ") for demands " << demands);
    if (RefPtr platformBackend = m_platformBackend)
        platformBackend->setPreferredFramesPerSecond(divisor == 1 ? 0 : static_cast<double>(m_displayNominalFramesPerSecond) / divisor);
}
#endif // PLATFORM(MAC)

DisplayLink& DisplayLinkCollection::displayLinkForDisplay(PlatformDisplayID displayID)
{
    if (auto* displayLink = existingDisplayLinkForDisplay(displayID))
        return *displayLink;

    auto displayLink = makeUnique<DisplayLink>(displayID);
    auto displayLinkPtr = displayLink.get();
    add(WTF::move(displayLink));
    return *displayLinkPtr;
}

DisplayLink* DisplayLinkCollection::existingDisplayLinkForDisplay(PlatformDisplayID displayID) const
{
    for (auto& displayLink : m_displayLinks) {
        if (displayLink->displayID() == displayID)
            return displayLink.get();
    }

    return nullptr;
}

void DisplayLinkCollection::add(std::unique_ptr<DisplayLink>&& displayLink)
{
    ASSERT(!m_displayLinks.containsIf([&](auto &entry) { return entry->displayID() == displayLink->displayID(); }));
    m_displayLinks.append(WTF::move(displayLink));
}

std::optional<unsigned> DisplayLinkCollection::nominalFramesPerSecondForDisplay(PlatformDisplayID displayID)
{
    // Note that this may create a DisplayLink with no observers, but it's highly likely that we'll soon call startDisplayLink() for it.
    auto& displayLink = displayLinkForDisplay(displayID);
    return displayLink.nominalFramesPerSecond();
}

void DisplayLinkCollection::startDisplayLink(DisplayLink::Client& client, DisplayLinkObserverID observerID, PlatformDisplayID displayID, FramesPerSecond preferredFramesPerSecond)
{
    auto& displayLink = displayLinkForDisplay(displayID);
    displayLink.addObserver(client, observerID, preferredFramesPerSecond);
}

void DisplayLinkCollection::stopDisplayLink(DisplayLink::Client& client, DisplayLinkObserverID observerID, PlatformDisplayID displayID)
{
    if (auto* displayLink = existingDisplayLinkForDisplay(displayID))
        displayLink->removeObserver(client, observerID);

    // FIXME: Remove unused display links?
}

void DisplayLinkCollection::stopDisplayLinks(DisplayLink::Client& client)
{
    for (auto& displayLink : m_displayLinks)
        displayLink->removeClient(client);

    // FIXME: Remove unused display links?
}

void DisplayLinkCollection::setDisplayLinkPreferredFramesPerSecond(DisplayLink::Client& client, DisplayLinkObserverID observerID, PlatformDisplayID displayID, FramesPerSecond preferredFramesPerSecond)
{
    LOG_WITH_STREAM(DisplayLink, stream << "[UI ] WebProcessPool::setDisplayLinkPreferredFramesPerSecond - display " << displayID << " observer " << observerID << " fps " << preferredFramesPerSecond);

    if (auto* displayLink = existingDisplayLinkForDisplay(displayID))
        displayLink->setObserverPreferredFramesPerSecond(client, observerID, preferredFramesPerSecond);
}

void DisplayLinkCollection::setDisplayLinkForDisplayWantsFullSpeedUpdates(DisplayLink::Client& client, PlatformDisplayID displayID, bool wantsFullSpeedUpdates)
{
    if (auto* displayLink = existingDisplayLinkForDisplay(displayID)) {
        if (wantsFullSpeedUpdates)
            displayLink->incrementFullSpeedRequestClientCount(client);
        else
            displayLink->decrementFullSpeedRequestClientCount(client);
    }
}

} // namespace WebKit

#endif // HAVE(DISPLAY_LINK)
