/*
 * Copyright (C) 2018-2022 Apple Inc. All rights reserved.
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
#include <mach/mach_time.h>
#include <wtf/ProcessPrivilege.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebKit {

using namespace WebCore;

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(DisplayLink::CoreVideoStatistics);

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

void DisplayLink::platformInitialize()
{
    // FIXME: We can get here with displayID == 0 (webkit.org/b/212120), in which case CVDisplayLinkCreateWithCGDisplay()
    // probably defaults to the main screen.
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanCommunicateWithWindowServer));
    if ((m_platformBackend = createCoreAnimationDisplayLinkBackendIfEnabled(*this, m_displayID))) {
        m_displayNominalFramesPerSecond = m_platformBackend->nominalFramesPerSecond();
        return;
    }

    m_displayLink = createDisplayLinkWithDisplay(m_displayID);
    if (!m_displayLink)
        return;

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    auto error = CVDisplayLinkSetOutputCallback(m_displayLink.get(), displayLinkCallback, this);
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error) {
        RELEASE_LOG_FAULT(DisplayLink, "DisplayLink: Could not set the display link output callback for display %u: error %d", m_displayID, error);
        return;
    }

    m_displayNominalFramesPerSecond = nominalFramesPerSecondFromDisplayLink(m_displayLink.get());

    if (displayLinkStatisticsLoggingEnabled())
        m_coreVideoStatistics = makeUnique<CoreVideoStatistics>();
}

void DisplayLink::platformFinalize()
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanCommunicateWithWindowServer));
    if (RefPtr platformBackend = m_platformBackend) {
        // invalidate() waits for any in-flight notification, which may read m_platformBackend in platformStop(),
        // so only clear the member afterwards.
        platformBackend->invalidate();
        m_platformBackend = nullptr;
        return;
    }

    ASSERT(m_displayLink);
    if (!m_displayLink)
        return;

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVDisplayLinkStop(m_displayLink.get());
    m_displayLink = nullptr;
ALLOW_DEPRECATED_DECLARATIONS_END
}

FramesPerSecond DisplayLink::nominalFramesPerSecondFromDisplayLink(CVDisplayLinkRef displayLink)
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVTime refreshPeriod = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(displayLink);
ALLOW_DEPRECATED_DECLARATIONS_END
    if (!refreshPeriod.timeValue)
        return FullSpeedFramesPerSecond;

    FramesPerSecond result = round((double)refreshPeriod.timeScale / (double)refreshPeriod.timeValue);
    return result ?: FullSpeedFramesPerSecond;
}

bool DisplayLink::platformIsRunning() const
{
    if (m_platformBackend)
        return m_platformBackend->isRunning();

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return CVDisplayLinkIsRunning(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
}

void DisplayLink::platformStart()
{
    if (RefPtr platformBackend = m_platformBackend) {
        platformBackend->start();
        return;
    }

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVReturn error = CVDisplayLinkStart(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error)
        RELEASE_LOG_FAULT(DisplayLink, "DisplayLink: Could not start the display link: %d", error);
}

void DisplayLink::platformStop()
{
    if (RefPtr platformBackend = m_platformBackend) {
        platformBackend->stop();
        return;
    }

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVDisplayLinkStop(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
}

CVReturn DisplayLink::displayLinkCallback(CVDisplayLinkRef displayLinkRef, const CVTimeStamp*, const CVTimeStamp* outputTime, CVOptionFlags, CVOptionFlags*, void* data)
{
    auto* displayLink = static_cast<DisplayLink*>(data);
    if (displayLink->m_coreVideoStatistics && outputTime) [[unlikely]]
        displayLink->recordCoreVideoStatistics(*outputTime);
    displayLink->notifyObserversDisplayDidRefresh();
    return kCVReturnSuccess;
}

void DisplayLink::recordCoreVideoStatistics(const CVTimeStamp& outputTime)
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

    auto& statistics = *m_coreVideoStatistics;
    // Restart the window after a pause so that paused time does not count.
    if (!statistics.windowStart || now - statistics.lastTick > 1) {
        statistics.windowStart = now;
        statistics.ticks = 0;
        statistics.phases.shrink(0);
    }
    statistics.lastTick = now;
    statistics.lastPeriod = period;
    ++statistics.ticks;
    statistics.phases.append(phase * 1000);

    double elapsed = now - statistics.windowStart;
    if (elapsed < 5)
        return;

    std::sort(statistics.phases.begin(), statistics.phases.end());
    auto percentile = [&](double fraction) {
        return statistics.phases[std::min<size_t>(statistics.phases.size() - 1, static_cast<size_t>(fraction * statistics.phases.size()))];
    };
    RELEASE_LOG(DisplayLink, "[UI ] CVDisplayLink stats display %u: %.1f ticks/s; callback-vsync ms p50 %.3f p95 %.3f (p50 %.1f%% of the %.3f ms refresh interval)",
        m_displayID, statistics.ticks / elapsed, percentile(0.5), percentile(0.95), percentile(0.5) / (period * 10), period * 1000);

    statistics.windowStart = 0;
}

} // namespace WebKit

#endif // HAVE(DISPLAY_LINK)
