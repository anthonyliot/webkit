/*
 * Copyright (C) 2018-2026 Apple Inc. All rights reserved.
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
#include <WebCore/CoreVideoExtras.h>
#include <mach/mach_time.h>
#include <wtf/Lock.h>
#include <wtf/ProcessPrivilege.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

namespace WebKit {

using namespace WebCore;

// The default DisplayLink backend on macOS, driven by CVDisplayLink.
class DisplayLinkCoreVideoBackend final : public DisplayLinkPlatformBackend {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DisplayLinkCoreVideoBackend);
public:
    static Ref<DisplayLinkCoreVideoBackend> create(DisplayLink& client, PlatformDisplayID displayID)
    {
        Ref backend = adoptRef(*new DisplayLinkCoreVideoBackend(client, displayID));
        backend->initialize();
        return backend;
    }

    FramesPerSecond nominalFramesPerSecond() const final { return m_nominalFramesPerSecond; }
    void displayConfigurationChanged(bool displayWasAdded) final;

    bool isRunning() const final;
    void start() final;
    void stop() final;
    void invalidate() final;

private:
    DisplayLinkCoreVideoBackend(DisplayLink& client, PlatformDisplayID displayID)
        : m_client(&client)
        , m_displayID(displayID)
    {
    }

    void initialize();
    void displayLinkFired(const CVTimeStamp* outputTime);
    void recordStatistics(const CVTimeStamp& outputTime);

    static CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp* outputTime, CVOptionFlags, CVOptionFlags*, void* data)
    {
        static_cast<DisplayLinkCoreVideoBackend*>(data)->displayLinkFired(outputTime);
        return kCVReturnSuccess;
    }

    static FramesPerSecond nominalFramesPerSecondFromDisplayLink(CVDisplayLinkRef);

    Lock m_clientLock;
    DisplayLink* m_client WTF_GUARDED_BY_LOCK(m_clientLock);
    const PlatformDisplayID m_displayID;
    FramesPerSecond m_nominalFramesPerSecond { FullSpeedFramesPerSecond };
    RefPtr<__CVDisplayLink> m_displayLink;

    // Only used with the WebKitDebugDisplayLinkLogStatistics default; accessed on the CVDisplayLink thread.
    bool m_logStatistics { false };
    Seconds m_statisticsInterval { 5_s };
    struct Statistics {
        double windowStart { 0 };
        double lastTick { 0 };
        unsigned ticks { 0 };
        Vector<double> phases;
    } m_statistics;
};

static RefPtr<__CVDisplayLink> createDisplayLinkWithDisplay(CGDirectDisplayID displayID)
{
    CVDisplayLinkRef displayLink = nullptr;
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVReturn error = CVDisplayLinkCreateWithCGDisplay(displayID, &displayLink);
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error) {
        RELEASE_LOG_FAULT(DisplayLink, "Could not create a display link for display %u: error %d", displayID, error);
        return nullptr;
    }
    return adoptRef(displayLink);
}

void DisplayLinkCoreVideoBackend::initialize()
{
    // We can get here with displayID == 0 (webkit.org/b/212120), for windowless and offscreen views. Use the main display:
    // a CVDisplayLink created for display 0 keeps a stale rate after the main display changes refresh rate (after
    // 120 -> 60 -> 120 Hz, it fires about 65 times per second).
    m_displayLink = createDisplayLinkWithDisplay(m_displayID ? m_displayID : CGMainDisplayID());
    if (!m_displayLink)
        return;

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    auto error = CVDisplayLinkSetOutputCallback(m_displayLink.get(), displayLinkCallback, this);
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error) {
        RELEASE_LOG_FAULT(DisplayLink, "DisplayLink: Could not set the display link output callback for display %u: error %d", m_displayID, error);
        return;
    }

    m_nominalFramesPerSecond = nominalFramesPerSecondFromDisplayLink(m_displayLink.get());
    m_logStatistics = displayLinkStatisticsLoggingEnabled();
    m_statisticsInterval = displayLinkStatisticsInterval();
}

void DisplayLinkCoreVideoBackend::displayConfigurationChanged(bool)
{
    // CVDisplayLink follows the display's mode on its own; only the nominal rate captured at creation needs updating.
    if (m_displayLink)
        m_nominalFramesPerSecond = nominalFramesPerSecondFromDisplayLink(m_displayLink.get());
}

bool DisplayLinkCoreVideoBackend::isRunning() const
{
    if (!m_displayLink)
        return false;
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return CVDisplayLinkIsRunning(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
}

void DisplayLinkCoreVideoBackend::start()
{
    if (!m_displayLink)
        return;
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVReturn error = CVDisplayLinkStart(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error)
        RELEASE_LOG_FAULT(DisplayLink, "DisplayLink: Could not start the display link: %d", error);
}

void DisplayLinkCoreVideoBackend::stop()
{
    if (!m_displayLink)
        return;
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVDisplayLinkStop(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
}

void DisplayLinkCoreVideoBackend::invalidate()
{
    {
        // Waits for any in-flight callback; no call into the DisplayLink can happen after this.
        Locker locker { m_clientLock };
        m_client = nullptr;
    }
    if (!m_displayLink)
        return;
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    // CVDisplayLinkStop() also waits for an in-flight callback, so the callback can't outlive this object.
    CVDisplayLinkStop(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
    m_displayLink = nullptr;
}

FramesPerSecond DisplayLinkCoreVideoBackend::nominalFramesPerSecondFromDisplayLink(CVDisplayLinkRef displayLink)
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVTime refreshPeriod = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(displayLink);
ALLOW_DEPRECATED_DECLARATIONS_END
    if (!refreshPeriod.timeValue)
        return FullSpeedFramesPerSecond;

    FramesPerSecond result = round((double)refreshPeriod.timeScale / (double)refreshPeriod.timeValue);
    return result ?: FullSpeedFramesPerSecond;
}

void DisplayLinkCoreVideoBackend::displayLinkFired(const CVTimeStamp* outputTime)
{
    if (m_logStatistics && outputTime) [[unlikely]]
        recordStatistics(*outputTime);

    Locker locker { m_clientLock };
    if (auto* client = m_client)
        client->platformBackendDidFire();
}

void DisplayLinkCoreVideoBackend::recordStatistics(const CVTimeStamp& outputTime)
{
    if (!(outputTime.flags & kCVTimeStampHostTimeValid) || !outputTime.videoTimeScale || !outputTime.videoRefreshPeriod)
        return;

    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return info;
    }();
    auto hostTimeToSeconds = [](uint64_t hostTime) {
        return static_cast<double>(hostTime) * timebase.numer / timebase.denom / 1e9;
    };

    // outputTime is on the vsync grid, so the callback phase is the distance from the grid.
    double now = hostTimeToSeconds(mach_absolute_time());
    double period = static_cast<double>(outputTime.videoRefreshPeriod) / outputTime.videoTimeScale;
    double phase = fmod(now - hostTimeToSeconds(outputTime.hostTime), period);
    if (phase < 0)
        phase += period;

    auto& statistics = m_statistics;
    // Restart the window after a pause so that paused time does not count.
    if (!statistics.windowStart || now - statistics.lastTick > 1) {
        statistics.windowStart = now;
        statistics.ticks = 0;
        statistics.phases.shrink(0);
    }
    statistics.lastTick = now;
    ++statistics.ticks;
    statistics.phases.append(phase * 1000);

    double elapsed = now - statistics.windowStart;
    if (elapsed < m_statisticsInterval.seconds())
        return;

    std::sort(statistics.phases.begin(), statistics.phases.end());
    auto percentile = [&](double fraction) {
        return statistics.phases[std::min<size_t>(statistics.phases.size() - 1, static_cast<size_t>(fraction * statistics.phases.size()))];
    };
    RELEASE_LOG(DisplayLink, "[UI ] CVDisplayLink stats display %u: %.1f ticks/s over %.3f s; callback-vsync ms p50 %.3f p95 %.3f (p50 %.1f%% of the %.3f ms refresh interval)",
        m_displayID, statistics.ticks / elapsed, elapsed, percentile(0.5), percentile(0.95), percentile(0.5) / (period * 10), period * 1000);

    statistics.windowStart = 0;
}

void DisplayLink::platformInitialize()
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanCommunicateWithWindowServer));
    RefPtr platformBackend = createCoreAnimationDisplayLinkBackendIfEnabled(*this, m_displayID);
    if (!platformBackend)
        platformBackend = DisplayLinkCoreVideoBackend::create(*this, m_displayID);
    m_displayNominalFramesPerSecond = platformBackend->nominalFramesPerSecond();
    m_platformBackend = WTF::move(platformBackend);
}

void DisplayLink::platformFinalize()
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanCommunicateWithWindowServer));
    ASSERT(m_platformBackend);
    // invalidate() waits for any in-flight notification, which may read m_platformBackend in platformStop(),
    // so only clear the member afterwards.
    if (RefPtr platformBackend = m_platformBackend) {
        platformBackend->invalidate();
        m_platformBackend = nullptr;
    }
}

bool DisplayLink::platformSupportsPreferredFramesPerSecond() const
{
    RefPtr platformBackend = m_platformBackend;
    return platformBackend && platformBackend->supportsPreferredFramesPerSecond();
}

double DisplayLink::requestedFramesPerSecondForTesting()
{
    if (!platformIsRunning())
        return 0;

    Locker locker { m_clientsLock };
    if (!m_platformFrameRateDivisor)
        return 0;
    // With no observers, the divisor is the last one requested; the display link stops on its own.
    for (auto& clientInfo : m_clients.values()) {
        if (!clientInfo.observers.isEmpty())
            return static_cast<double>(m_displayNominalFramesPerSecond) / m_platformFrameRateDivisor;
    }
    return 0;
}

Vector<FramesPerSecond> DisplayLink::observerFramesPerSecondForTesting()
{
    Locker locker { m_clientsLock };
    Vector<FramesPerSecond> framesPerSecond;
    for (auto& clientInfo : m_clients.values()) {
        if (clientInfo.observers.isEmpty())
            continue;
        if (clientInfo.fullSpeedUpdatesClientCount)
            framesPerSecond.append(m_displayNominalFramesPerSecond);
        for (auto& observer : clientInfo.observers)
            framesPerSecond.append(observer.preferredFramesPerSecond);
    }
    std::ranges::sort(framesPerSecond);
    return framesPerSecond;
}

bool DisplayLink::platformIsRunning() const
{
    RefPtr platformBackend = m_platformBackend;
    return platformBackend && platformBackend->isRunning();
}

void DisplayLink::platformStart()
{
    if (RefPtr platformBackend = m_platformBackend)
        platformBackend->start();
}

void DisplayLink::platformStop()
{
    if (RefPtr platformBackend = m_platformBackend)
        platformBackend->stop();
}

} // namespace WebKit

#endif // HAVE(DISPLAY_LINK)
