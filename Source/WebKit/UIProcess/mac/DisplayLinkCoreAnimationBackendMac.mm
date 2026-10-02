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

#import "DisplayLinkRateController.h"
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

// When the CADisplayLink backend notifies DisplayLink of a tick: this fraction of the display's refresh interval after
// the vsync the tick is for. The link calls back at the vsync; a CVDisplayLink calls back from a quarter to over half of
// the interval after it, depending on the app.
constexpr double displayLinkNotificationPhase = 0.15;

// How long after `now` DisplayLink is notified of a tick of a link for the vsync at `timestamp`, given the display's
// refresh interval, `duration`: at that phase, or at once if that's past.
static Seconds displayLinkNotificationDelay(double timestamp, double duration, double now)
{
    return Seconds { std::max(0.0, timestamp + displayLinkNotificationPhase * duration - now) };
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
        // The notification timer's context is this backend: invalidateOnLinkThread() invalidated it.
        ASSERT(!m_notificationTimer);
        ASSERT(!m_pendingUpdate);
    }

    FramesPerSecond nominalFramesPerSecond() const final { return m_nominalFramesPerSecond; }
    bool isRunning() const final { return m_wantsRunning; }
    void start() final;
    void stop() final;
    void invalidate() final;
    bool supportsFrameRateDivisor() const final { return true; }
    void setFrameRateDivisor(unsigned) final;

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
    void applyFrameRateDivisor(unsigned);
    void notifyAfterVSync(CFTimeInterval timestamp, CFTimeInterval duration, DisplayUpdate);
    // On the link thread, when it's time to notify DisplayLink of the tick that waits for it.
    static void notificationTimerCallback(CFRunLoopTimerRef, void* backend);
    void notifyPendingUpdate();

    Lock m_clientLock;
    // Cleared by invalidate(); no call into the DisplayLink can happen afterwards.
    DisplayLink* m_client WTF_GUARDED_BY_LOCK(m_clientLock);
    const PlatformDisplayID m_displayID;
    const FramesPerSecond m_nominalFramesPerSecond;
    // Written on both threads (by stop() on the link thread), read on both.
    std::atomic<bool> m_wantsRunning { false };
    // The divisor of the nominal rate DisplayLink asks for.
    std::atomic<unsigned> m_requestedDivisor { 1 };

    // The link thread, created on the first start(), so that a DisplayLink that is only created to read the display's
    // rate doesn't create one. Written once, on the main thread, before anything is dispatched to it; read on both.
    RefPtr<RunLoop> m_runLoop;

    // Only accessed on the main thread.
    bool m_loggedMissingScreen { false };

    // Only accessed on the link thread.
    RetainPtr<CADisplayLink> m_displayLink;
    DisplayLinkRateController m_rateController;
    // The denied divisors this link logged.
    DisplayLinkDeniedDivisors m_loggedDenials;
    // DisplayLink is notified part way into each vsync, not at the vsync, where CADisplayLink calls back: the update of
    // the tick waiting for it, and the timer that sends it, made on the first tick and rescheduled at each one. (A
    // RunLoop::Timer that fired would make a new CFRunLoopTimer each time.)
    std::optional<DisplayUpdate> m_pendingUpdate;
    RetainPtr<CFRunLoopTimerRef> m_notificationTimer;
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

void DisplayLinkCoreAnimationBackend::setFrameRateDivisor(unsigned divisor)
{
    ASSERT(RunLoop::isMain());
    m_requestedDivisor = divisor;
    if (RefPtr runLoop = m_runLoop) {
        // The latest request, when several are in flight.
        runLoop->dispatch([protectedThis = Ref { *this }] {
            protectedThis->applyFrameRateDivisor(protectedThis->m_requestedDivisor);
        });
    }
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
    applyFrameRateDivisor(m_requestedDivisor);
    synchronizePausedState();
}

void DisplayLinkCoreAnimationBackend::applyFrameRateDivisor(unsigned requestedDivisor)
{
    ASSERT(protect(m_runLoop)->isCurrent());
    if (!m_displayLink)
        return;
    auto framesPerSecond = m_rateController.preferredFramesPerSecond(requestedDivisor, m_nominalFramesPerSecond);
    if (!framesPerSecond)
        return;

    // Core Animation fires the link every N vsyncs, N nearest to the display's actual rate over the preferred rate: the
    // nominal rate DisplayLink divided, over the divisor, gets that divisor, also on a 119.98 Hz mode. It doesn't fire
    // the link at every N: asked for every 7 vsyncs, it fires it every 6, and the link falls back.
    auto range = *framesPerSecond ? CAFrameRateRangeMake(*framesPerSecond, *framesPerSecond, *framesPerSecond) : CAFrameRateRangeDefault;
    [m_displayLink setPreferredFrameRateRange:range];
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
    if ([m_displayLink isPaused] == shouldPause)
        return;
    if (!shouldPause) {
        // Core Animation may grant a divisor it didn't before, like after a slow grant under load: ask for it again.
        m_rateController.linkResumed();
        applyFrameRateDivisor(m_requestedDivisor);
    }
    [m_displayLink setPaused:shouldPause];
}

void DisplayLinkCoreAnimationBackend::invalidateOnLinkThread()
{
    ASSERT(protect(m_runLoop)->isCurrent());
    if (m_displayLink) {
        [m_displayLink invalidate];
        m_displayLink = nil;
    }
    // The notification of the link's last tick, if it's still pending, isn't sent.
    m_pendingUpdate = std::nullopt;
    if (m_notificationTimer) {
        CFRunLoopTimerInvalidate(m_notificationTimer.get());
        m_notificationTimer = nullptr;
    }
    RunLoop::currentSingleton().stop();
}

void DisplayLinkCoreAnimationBackend::tick(CADisplayLink *displayLink)
{
    ASSERT(protect(m_runLoop)->isCurrent());
    // The main thread may have cleared m_wantsRunning before the link thread paused the link.
    if (!m_wantsRunning)
        return;

    auto timestamp = displayLink.timestamp;
    auto duration = displayLink.duration;
    auto result = m_rateController.tick(timestamp, displayLink.targetTimestamp, duration, m_nominalFramesPerSecond);

    // The divisor isn't asked of this link again until it resumes.
    if (result.requestDenied) {
        // Once per divisor for this link, whatever the mode: the link is asked for it again each time it resumes.
        if (!m_loggedDenials.isDenied(result.requestedDivisor)) {
            m_loggedDenials.deny(result.requestedDivisor);
            RELEASE_LOG(DisplayLink, "[UI ] CADisplayLink for display %u fires every %u vsyncs instead of %u; running it at the display's rate", m_displayID, result.linkDivisor, result.requestedDivisor);
        }
        applyFrameRateDivisor(m_requestedDivisor);
    }

    notifyAfterVSync(timestamp, duration, result.update);
}

void DisplayLinkCoreAnimationBackend::notificationTimerCallback(CFRunLoopTimerRef, void* backend)
{
    // As for a tick of the link.
    @autoreleasepool {
        Ref { *static_cast<DisplayLinkCoreAnimationBackend*>(backend) }->notifyPendingUpdate();
    }
}

void DisplayLinkCoreAnimationBackend::notifyAfterVSync(CFTimeInterval timestamp, CFTimeInterval duration, DisplayUpdate update)
{
    ASSERT(protect(m_runLoop)->isCurrent());
    // The previous tick's, if its timer hasn't fired yet, so that updates keep their order.
    notifyPendingUpdate();
    // That notification may have stopped the backend.
    if (!m_wantsRunning)
        return;
    m_pendingUpdate = update;
    auto delay = displayLinkNotificationDelay(timestamp, duration, CACurrentMediaTime());
    if (!delay) {
        notifyPendingUpdate();
        return;
    }
    auto fireDate = CFAbsoluteTimeGetCurrent() + delay.seconds();
    if (m_notificationTimer) {
        CFRunLoopTimerSetNextFireDate(m_notificationTimer.get(), fireDate);
        return;
    }
    // A timer that is only rescheduled, never released while scheduled: invalidateOnLinkThread() invalidates it on this
    // thread, before this backend can be destroyed.
    CFRunLoopTimerContext context { 0, this, nullptr, nullptr, nullptr };
    m_notificationTimer = adoptCF(CFRunLoopTimerCreate(kCFAllocatorDefault, fireDate, std::numeric_limits<CFTimeInterval>::max(), 0, 0, notificationTimerCallback, &context));
    CFRunLoopAddTimer(protect(CFRunLoopGetCurrent()).get(), m_notificationTimer.get(), kCFRunLoopCommonModes);
}

void DisplayLinkCoreAnimationBackend::notifyPendingUpdate()
{
    ASSERT(protect(m_runLoop)->isCurrent());
    auto update = std::exchange(m_pendingUpdate, std::nullopt);
    // As at a tick, the main thread may have cleared m_wantsRunning meanwhile.
    if (!update || !m_wantsRunning)
        return;
    Locker locker { m_clientLock };
    if (auto* client = m_client)
        displayLinkFired(*client, *update);
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
