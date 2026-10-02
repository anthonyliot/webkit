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

#import "config.h"
#import "DisplayLink.h"

#if HAVE(DISPLAY_LINK) && PLATFORM(MAC)

#import "Logging.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <WebCore/PlatformScreen.h>
#import <wtf/Lock.h>
#import <wtf/RetainPtr.h>
#import <wtf/RunLoop.h>
#import <wtf/TZoneMallocInlines.h>

// An experimental DisplayLink backend driven by CADisplayLink. It is selected when a DisplayLink is created, with the
// WebKitDebugDisplayLinkBackend default set to "CoreAnimation"; DisplayLink uses CVDisplayLink otherwise.

namespace WebKit {
class DisplayLinkCoreAnimationBackend;
}

@interface WKDisplayLinkBackendTarget : NSObject
- (instancetype)initWithBackend:(WebKit::DisplayLinkCoreAnimationBackend&)backend;
- (void)displayLinkFired:(CADisplayLink *)displayLink;
@end

namespace WebKit {

using namespace WebCore;

// Display 0 is used by windowless and offscreen views; it follows the main display.
static NSScreen *screenForDisplay(PlatformDisplayID displayID)
{
    auto targetDisplayID = displayID ? displayID : CGMainDisplayID();
    for (NSScreen *screen in NSScreen.screens) {
        if (WebCore::displayID(screen) == targetDisplayID)
            return screen;
    }
    return nil;
}

static FramesPerSecond nominalFramesPerSecondForScreen(NSScreen *screen)
{
    return screen.maximumFramesPerSecond > 0 ? static_cast<FramesPerSecond>(screen.maximumFramesPerSecond) : FullSpeedFramesPerSecond;
}

class DisplayLinkCoreAnimationBackend final : public DisplayLinkPlatformBackend {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DisplayLinkCoreAnimationBackend);
public:
    static Ref<DisplayLinkCoreAnimationBackend> create(DisplayLink& client, PlatformDisplayID displayID, FramesPerSecond nominalFramesPerSecond)
    {
        return adoptRef(*new DisplayLinkCoreAnimationBackend(client, displayID, nominalFramesPerSecond));
    }

    ~DisplayLinkCoreAnimationBackend()
    {
        ASSERT(!m_displayLink);
    }

    FramesPerSecond nominalFramesPerSecond() const final { return m_nominalFramesPerSecond; }
    bool isRunning() const final { return m_wantsRunning; }
    void start() final;
    void stop() final;
    void invalidate() final;

    // Called on the link thread for each tick of the CADisplayLink.
    void tick(CADisplayLink *);

private:
    DisplayLinkCoreAnimationBackend(DisplayLink& client, PlatformDisplayID displayID, FramesPerSecond nominalFramesPerSecond)
        : m_client(&client)
        , m_displayID(displayID)
        , m_nominalFramesPerSecond(nominalFramesPerSecond)
    {
    }

    bool createDisplayLink();
    void addDisplayLinkToRunLoop(RetainPtr<CADisplayLink>&&);
    void scheduleSynchronizePausedState();
    void synchronizePausedState();
    void invalidateOnLinkThread();

    Lock m_clientLock;
    // Cleared by invalidate(); no call into the DisplayLink can happen afterwards.
    DisplayLink* m_client WTF_GUARDED_BY_LOCK(m_clientLock);
    const PlatformDisplayID m_displayID;
    const FramesPerSecond m_nominalFramesPerSecond;
    // Written on both threads (by stop() on the link thread), read on both.
    std::atomic<bool> m_wantsRunning { false };

    // The link thread, created on the first start(), so that a DisplayLink that is only created to read the display's
    // rate doesn't create one. Written once, on the main thread, before anything is dispatched to it; read on both.
    RefPtr<RunLoop> m_runLoop;

    // Only accessed on the main thread.
    bool m_loggedMissingScreen { false };

    // Only accessed on the link thread.
    RetainPtr<CADisplayLink> m_displayLink;
};

void DisplayLinkCoreAnimationBackend::start()
{
    ASSERT(RunLoop::isMain());
    m_wantsRunning = true;
    if (!m_runLoop && !createDisplayLink()) {
        m_wantsRunning = false;
        return;
    }
    scheduleSynchronizePausedState();
}

void DisplayLinkCoreAnimationBackend::stop()
{
    // When DisplayLink stops after ticks without observers.
    ASSERT(protect(m_runLoop)->isCurrent());
    m_wantsRunning = false;
    scheduleSynchronizePausedState();
}

void DisplayLinkCoreAnimationBackend::invalidate()
{
    ASSERT(RunLoop::isMain());
    {
        // Waits for a tick in progress.
        Locker locker { m_clientLock };
        m_client = nullptr;
    }
    m_wantsRunning = false;
    if (RefPtr runLoop = m_runLoop) {
        runLoop->dispatch([protectedThis = Ref { *this }] {
            protectedThis->invalidateOnLinkThread();
        });
    }
}

bool DisplayLinkCoreAnimationBackend::createDisplayLink()
{
    ASSERT(RunLoop::isMain());
    NSScreen *screen = screenForDisplay(m_displayID);
    if (!screen) {
        // The display went away, and the pages on it haven't moved to another display yet.
        if (!std::exchange(m_loggedMissingScreen, true))
            RELEASE_LOG_ERROR(DisplayLink, "[UI ] CADisplayLink backend: display %u has no screen; not starting", m_displayID);
        return false;
    }
    m_loggedMissingScreen = false;

    // The link retains its target, which keeps this backend alive until the link is invalidated on the link thread.
    RetainPtr target = adoptNS([[WKDisplayLinkBackendTarget alloc] initWithBackend:*this]);
    RetainPtr<CADisplayLink> displayLink = [screen displayLinkWithTarget:target.get() selector:@selector(displayLinkFired:)];
    if (!m_runLoop)
        m_runLoop = RunLoop::create("WebKit: CADisplayLink"_s, ThreadType::Graphics, ThreadQOS::UserInteractive);

    protect(m_runLoop)->dispatch([protectedThis = Ref { *this }, displayLink = WTF::move(displayLink)] mutable {
        protectedThis->addDisplayLinkToRunLoop(WTF::move(displayLink));
    });
    return true;
}

void DisplayLinkCoreAnimationBackend::addDisplayLinkToRunLoop(RetainPtr<CADisplayLink>&& displayLink)
{
    ASSERT(protect(m_runLoop)->isCurrent());
    m_displayLink = WTF::move(displayLink);
    [m_displayLink setPaused:YES];
    [m_displayLink addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
    synchronizePausedState();
}

void DisplayLinkCoreAnimationBackend::scheduleSynchronizePausedState()
{
    RefPtr runLoop = m_runLoop;
    if (!runLoop)
        return;
    if (runLoop->isCurrent()) {
        synchronizePausedState();
        return;
    }
    runLoop->dispatch([protectedThis = Ref { *this }] {
        protectedThis->synchronizePausedState();
    });
}

void DisplayLinkCoreAnimationBackend::synchronizePausedState()
{
    ASSERT(protect(m_runLoop)->isCurrent());
    if (!m_displayLink)
        return;
    bool shouldPause = !m_wantsRunning;
    if ([m_displayLink isPaused] != shouldPause)
        [m_displayLink setPaused:shouldPause];
}

void DisplayLinkCoreAnimationBackend::invalidateOnLinkThread()
{
    ASSERT(protect(m_runLoop)->isCurrent());
    if (m_displayLink) {
        [m_displayLink invalidate];
        m_displayLink = nil;
    }
    RunLoop::currentSingleton().stop();
}

void DisplayLinkCoreAnimationBackend::tick(CADisplayLink *)
{
    ASSERT(protect(m_runLoop)->isCurrent());
    // The main thread may have cleared m_wantsRunning before the link thread paused the link.
    if (!m_wantsRunning)
        return;

    Locker locker { m_clientLock };
    if (auto* client = m_client)
        displayLinkFired(*client);
}

RefPtr<DisplayLinkPlatformBackend> createCoreAnimationDisplayLinkBackendIfEnabled(DisplayLink& client, PlatformDisplayID displayID)
{
    if (![[NSUserDefaults.standardUserDefaults stringForKey:@"WebKitDebugDisplayLinkBackend"] isEqualToString:@"CoreAnimation"])
        return nullptr;

    ASSERT(RunLoop::isMain());
    NSScreen *screen = screenForDisplay(displayID);
    if (!screen) {
        RELEASE_LOG_ERROR(DisplayLink, "[UI ] No screen for display %u; using CVDisplayLink", displayID);
        return nullptr;
    }

    auto nominalFramesPerSecond = nominalFramesPerSecondForScreen(screen);
    RELEASE_LOG(DisplayLink, "[UI ] Using CADisplayLink for display %u (nominal fps %u)", displayID, nominalFramesPerSecond);
    return DisplayLinkCoreAnimationBackend::create(client, displayID, nominalFramesPerSecond);
}

} // namespace WebKit

@implementation WKDisplayLinkBackendTarget {
    RefPtr<WebKit::DisplayLinkCoreAnimationBackend> _backend;
}

- (instancetype)initWithBackend:(WebKit::DisplayLinkCoreAnimationBackend&)backend
{
    if (!(self = [super init]))
        return nil;
    _backend = &backend;
    return self;
}

- (void)displayLinkFired:(CADisplayLink *)displayLink
{
    @autoreleasepool {
        if (RefPtr backend = _backend)
            backend->tick(displayLink);
    }
}

@end

#endif // HAVE(DISPLAY_LINK) && PLATFORM(MAC)
