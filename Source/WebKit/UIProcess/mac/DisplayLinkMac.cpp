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
#include <wtf/ProcessPrivilege.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebKit {

using namespace WebCore;

// The DisplayLink backend driven by CVDisplayLink.
class DisplayLinkCoreVideoBackend final : public DisplayLinkPlatformBackend {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DisplayLinkCoreVideoBackend);
public:
    static Ref<DisplayLinkCoreVideoBackend> create(DisplayLink& displayLink, PlatformDisplayID displayID)
    {
        Ref backend = adoptRef(*new DisplayLinkCoreVideoBackend);
        backend->initialize(displayLink, displayID);
        return backend;
    }

    FramesPerSecond nominalFramesPerSecond() const final { return m_nominalFramesPerSecond; }
    bool isRunning() const final;
    void start() final;
    void stop() final;
    void invalidate() final;

private:
    DisplayLinkCoreVideoBackend() = default;

    void initialize(DisplayLink&, PlatformDisplayID);

    static CVReturn displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* data);
    static FramesPerSecond nominalFramesPerSecondFromDisplayLink(CVDisplayLinkRef);

    RefPtr<__CVDisplayLink> m_displayLink;
    FramesPerSecond m_nominalFramesPerSecond { FullSpeedFramesPerSecond };
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

void DisplayLinkCoreVideoBackend::initialize(DisplayLink& displayLink, PlatformDisplayID displayID)
{
    // FIXME: We can get here with displayID == 0 (webkit.org/b/212120), in which case CVDisplayLinkCreateWithCGDisplay()
    // probably defaults to the main screen.
    m_displayLink = createDisplayLinkWithDisplay(displayID);
    if (!m_displayLink)
        return;

    // The callback's context is the DisplayLink: invalidate() stops the CVDisplayLink first, and, as DisplayLink always
    // has, relies on CVDisplayLinkStop() not to return while a callback runs.
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    auto error = CVDisplayLinkSetOutputCallback(m_displayLink.get(), displayLinkCallback, &displayLink);
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error) {
        RELEASE_LOG_FAULT(DisplayLink, "DisplayLink: Could not set the display link output callback for display %u: error %d", displayID, error);
        return;
    }

    m_nominalFramesPerSecond = nominalFramesPerSecondFromDisplayLink(m_displayLink.get());
}

void DisplayLinkCoreVideoBackend::invalidate()
{
    ASSERT(m_displayLink);
    if (!m_displayLink)
        return;

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVDisplayLinkStop(m_displayLink.get());
    m_displayLink = nullptr;
ALLOW_DEPRECATED_DECLARATIONS_END
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

bool DisplayLinkCoreVideoBackend::isRunning() const
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return CVDisplayLinkIsRunning(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
}

void DisplayLinkCoreVideoBackend::start()
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVReturn error = CVDisplayLinkStart(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
    if (error)
        RELEASE_LOG_FAULT(DisplayLink, "DisplayLink: Could not start the display link: %d", error);
}

void DisplayLinkCoreVideoBackend::stop()
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    CVDisplayLinkStop(m_displayLink.get());
ALLOW_DEPRECATED_DECLARATIONS_END
}

CVReturn DisplayLinkCoreVideoBackend::displayLinkCallback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void* data)
{
    displayLinkFired(*static_cast<DisplayLink*>(data));
    return kCVReturnSuccess;
}

void DisplayLinkPlatformBackend::displayLinkFired(DisplayLink& displayLink)
{
    displayLink.notifyObserversDisplayDidRefresh();
}

void DisplayLink::platformInitialize()
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanCommunicateWithWindowServer));
    Ref platformBackend = DisplayLinkCoreVideoBackend::create(*this, m_displayID);
    m_displayNominalFramesPerSecond = platformBackend->nominalFramesPerSecond();
    m_platformBackend = WTF::move(platformBackend);
}

void DisplayLink::platformFinalize()
{
    ASSERT(hasProcessPrivilege(ProcessPrivilege::CanCommunicateWithWindowServer));
    protect(m_platformBackend)->invalidate();
    m_platformBackend = nullptr;
}

bool DisplayLink::platformIsRunning() const
{
    return protect(m_platformBackend)->isRunning();
}

void DisplayLink::platformStart()
{
    protect(m_platformBackend)->start();
}

void DisplayLink::platformStop()
{
    protect(m_platformBackend)->stop();
}

} // namespace WebKit

#endif // HAVE(DISPLAY_LINK)
