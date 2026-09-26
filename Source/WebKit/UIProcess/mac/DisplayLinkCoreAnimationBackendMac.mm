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
#import <mach/mach.h>
#import <mach/mach_time.h>
#import <mach/thread_policy.h>
#import <wtf/Lock.h>
#import <wtf/RetainPtr.h>
#import <wtf/RunLoop.h>
#import <wtf/StdLibExtras.h>
#import <wtf/TZoneMallocInlines.h>
#import <wtf/Vector.h>

// Experimental CADisplayLink-based implementation of the UI-process DisplayLink on macOS.
// Selected at DisplayLink creation time with the WebKitDebugDisplayLinkBackend default
// (CoreVideo, the default, or CoreAnimation). Knobs, all read at creation time:
//   WebKitDebugDisplayLinkCallbackDelayFraction (double, fraction of the display refresh interval, default 0)
//   WebKitDebugDisplayLinkThreadPolicy (qos | fixed | timeConstraint, default qos)
//   WebKitDebugDisplayLinkLogStatistics (bool, default NO)

namespace WebKit {
class DisplayLinkCoreAnimationBackend;
}

@interface WKDisplayLinkBackendTarget : NSObject
- (instancetype)initWithBackend:(WebKit::DisplayLinkCoreAnimationBackend&)backend;
- (void)displayLinkFired:(CADisplayLink *)displayLink;
@end

namespace WebKit {

using namespace WebCore;

enum class DisplayLinkThreadPolicy : uint8_t { QoS, Fixed, TimeConstraint };

struct DisplayLinkCoreAnimationBackendOptions {
    double callbackDelayFraction { 0 };
    DisplayLinkThreadPolicy threadPolicy { DisplayLinkThreadPolicy::QoS };
    bool logStatistics { false };
};

static constexpr Seconds statisticsInterval { 5_s };
// Beyond this, timer slop makes a delayed notification land on the next vsync anyway.
static constexpr double maximumCallbackDelayFraction { 0.8 };

class DisplayLinkCoreAnimationBackend final : public DisplayLinkPlatformBackend {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DisplayLinkCoreAnimationBackend);
public:
    static Ref<DisplayLinkCoreAnimationBackend> create(DisplayLink& client, PlatformDisplayID displayID, NSScreen *screen, const DisplayLinkCoreAnimationBackendOptions& options)
    {
        Ref backend = adoptRef(*new DisplayLinkCoreAnimationBackend(client, displayID, screen, options));
        backend->initialize(screen);
        return backend;
    }

    ~DisplayLinkCoreAnimationBackend()
    {
        ASSERT(!m_displayLink);
        ASSERT(!m_delayTimer);
    }

    FramesPerSecond nominalFramesPerSecond() const final { return m_nominalFramesPerSecond; }
    bool isRunning() const final { return m_wantsRunning; }

    void start() final
    {
        m_wantsRunning = true;
        scheduleSynchronizePausedState();
    }

    void stop() final
    {
        m_wantsRunning = false;
        scheduleSynchronizePausedState();
    }

    void invalidate() final
    {
        ASSERT(RunLoop::isMain());
        {
            // Waits for any in-flight notification; no call into the DisplayLink can happen after this.
            Locker locker { m_clientLock };
            m_client = nullptr;
        }
        m_wantsRunning = false;
        m_runLoop->dispatch([protectedThis = Ref { *this }] {
            protectedThis->invalidateOnLinkThread();
        });
    }

    void displayLinkFired(CADisplayLink *);

private:
    DisplayLinkCoreAnimationBackend(DisplayLink& client, PlatformDisplayID displayID, NSScreen *screen, const DisplayLinkCoreAnimationBackendOptions& options)
        : m_client(&client)
        , m_displayID(displayID)
        , m_nominalFramesPerSecond(screen.maximumFramesPerSecond > 0 ? static_cast<FramesPerSecond>(screen.maximumFramesPerSecond) : FullSpeedFramesPerSecond)
        , m_options(options)
        , m_runLoop(RunLoop::create("WebKit: CADisplayLink"_s, ThreadType::Graphics, ThreadQOS::UserInteractive))
    {
    }

    void initialize(NSScreen *);
    void configureThread();
    void scheduleSynchronizePausedState();
    void synchronizePausedState();
    void invalidateOnLinkThread();
    void notifyClient();
    void armDelayTimer(CFTimeInterval fireMediaTime);
    void disarmDelayTimer();
    void recordStatistics(CADisplayLink *);
    void flushStatistics(CFTimeInterval now);

    static void delayTimerFired(CFRunLoopTimerRef, void* info)
    {
        auto& backend = *static_cast<DisplayLinkCoreAnimationBackend*>(info);
        backend.m_delayedNotificationPending = false;
        backend.notifyClient();
    }

    Lock m_clientLock;
    DisplayLink* m_client WTF_GUARDED_BY_LOCK(m_clientLock);
    const PlatformDisplayID m_displayID;
    const FramesPerSecond m_nominalFramesPerSecond;
    const DisplayLinkCoreAnimationBackendOptions m_options;
    const Ref<RunLoop> m_runLoop;
    std::atomic<bool> m_wantsRunning { false };

    // Only accessed on the link thread.
    RetainPtr<CADisplayLink> m_displayLink;
    RetainPtr<CFRunLoopTimerRef> m_delayTimer;
    bool m_delayedNotificationPending { false };
    struct Statistics {
        CFTimeInterval windowStart { 0 };
        CFTimeInterval lastTimestamp { 0 };
        unsigned ticks { 0 };
        unsigned notifications { 0 };
        unsigned lateNotifications { 0 };
        Vector<double> callbackLatencies;
        Vector<double> notificationLatencies;
        Vector<double> tickIntervals;
        double lastDuration { 0 };
        double lastPeriod { 0 };
    } m_statistics;
};

void DisplayLinkCoreAnimationBackend::initialize(NSScreen *screen)
{
    ASSERT(RunLoop::isMain());

    // The link retains its target, which keeps this backend alive until the link is invalidated on the link thread.
    RetainPtr target = adoptNS([[WKDisplayLinkBackendTarget alloc] initWithBackend:*this]);
    RetainPtr<CADisplayLink> displayLink = [screen displayLinkWithTarget:target.get() selector:@selector(displayLinkFired:)];

    RELEASE_LOG(DisplayLink, "[UI ] CADisplayLink backend created for display %u (screen %u), nominal fps %u, delay fraction %.3f, thread policy %u, link %p",
        m_displayID, WebCore::displayID(screen), m_nominalFramesPerSecond, m_options.callbackDelayFraction, static_cast<unsigned>(m_options.threadPolicy), displayLink.get());

    m_runLoop->dispatch([protectedThis = Ref { *this }, displayLink = WTF::move(displayLink)] mutable {
        protectedThis->configureThread();
        protectedThis->m_displayLink = WTF::move(displayLink);
        [protectedThis->m_displayLink setPaused:YES];
        [protectedThis->m_displayLink addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
        protectedThis->synchronizePausedState();
    });
}

void DisplayLinkCoreAnimationBackend::configureThread()
{
    ASSERT(m_runLoop->isCurrent());

    switch (m_options.threadPolicy) {
    case DisplayLinkThreadPolicy::QoS:
        return;
    case DisplayLinkThreadPolicy::Fixed: {
        // Matches the CVDisplayLink thread: fixed (non-timeshare) priority 54.
        thread_extended_policy_data_t extendedPolicy { .timeshare = 0 };
        auto extendedResult = thread_policy_set(mach_thread_self(), THREAD_EXTENDED_POLICY, reinterpret_cast<thread_policy_t>(&extendedPolicy), THREAD_EXTENDED_POLICY_COUNT);
        thread_precedence_policy_data_t precedencePolicy { .importance = 23 };
        auto precedenceResult = thread_policy_set(mach_thread_self(), THREAD_PRECEDENCE_POLICY, reinterpret_cast<thread_policy_t>(&precedencePolicy), THREAD_PRECEDENCE_POLICY_COUNT);
        RELEASE_LOG(DisplayLink, "[UI ] CADisplayLink thread policy fixed: extended %d precedence %d", extendedResult, precedenceResult);
        return;
    }
    case DisplayLinkThreadPolicy::TimeConstraint: {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        auto secondsToAbsolute = [&](double seconds) {
            return static_cast<uint32_t>(seconds * 1e9 * timebase.denom / timebase.numer);
        };
        double period = 1.0 / m_nominalFramesPerSecond;
        thread_time_constraint_policy_data_t policy {
            .period = secondsToAbsolute(period),
            .computation = secondsToAbsolute(std::min(0.001, period / 4)),
            .constraint = secondsToAbsolute(period / 2),
            .preemptible = 1,
        };
        auto result = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        RELEASE_LOG(DisplayLink, "[UI ] CADisplayLink thread policy timeConstraint: %d", result);
        return;
    }
    }
}

void DisplayLinkCoreAnimationBackend::scheduleSynchronizePausedState()
{
    if (m_runLoop->isCurrent()) {
        synchronizePausedState();
        return;
    }
    m_runLoop->dispatch([protectedThis = Ref { *this }] {
        protectedThis->synchronizePausedState();
    });
}

void DisplayLinkCoreAnimationBackend::synchronizePausedState()
{
    ASSERT(m_runLoop->isCurrent());
    if (!m_displayLink)
        return;

    bool shouldPause = !m_wantsRunning;
    if (shouldPause)
        disarmDelayTimer();
    if ([m_displayLink isPaused] == shouldPause)
        return;

    [m_displayLink setPaused:shouldPause];
    if (shouldPause && m_options.logStatistics) {
        // Paused time must not count toward the statistics window or the tick intervals.
        flushStatistics(CACurrentMediaTime());
        m_statistics.lastTimestamp = 0;
    }
}

void DisplayLinkCoreAnimationBackend::invalidateOnLinkThread()
{
    ASSERT(m_runLoop->isCurrent());

    if (m_delayTimer) {
        CFRunLoopTimerInvalidate(m_delayTimer.get());
        m_delayTimer = nullptr;
    }
    m_delayedNotificationPending = false;
    if (m_displayLink) {
        [m_displayLink invalidate];
        m_displayLink = nil;
    }
    RunLoop::currentSingleton().stop();
}

void DisplayLinkCoreAnimationBackend::displayLinkFired(CADisplayLink *displayLink)
{
    ASSERT(m_runLoop->isCurrent());
    if (!m_wantsRunning)
        return;

    if (std::exchange(m_delayedNotificationPending, false)) {
        // The previous tick's delayed notification has not fired yet (timer slop or thread starvation);
        // deliver it now rather than dropping it when re-arming the timer below.
        if (m_options.logStatistics)
            ++m_statistics.lateNotifications;
        notifyClient();
        // The notification may have stopped the display link.
        if (!m_wantsRunning)
            return;
    }

    if (m_options.logStatistics)
        recordStatistics(displayLink);

    if (m_options.callbackDelayFraction > 0) {
        m_delayedNotificationPending = true;
        armDelayTimer(displayLink.timestamp + m_options.callbackDelayFraction * displayLink.duration);
        return;
    }
    notifyClient();
}

void DisplayLinkCoreAnimationBackend::notifyClient()
{
    ASSERT(m_runLoop->isCurrent());
    // A delayed notification can race with a stop() that has not been synchronized to this thread yet.
    if (!m_wantsRunning)
        return;
    if (m_options.logStatistics) {
        ++m_statistics.notifications;
        if (m_statistics.lastTimestamp)
            m_statistics.notificationLatencies.append((CACurrentMediaTime() - m_statistics.lastTimestamp) * 1000);
    }

    Locker locker { m_clientLock };
    if (auto* client = m_client)
        client->platformBackendDidFire();
}

void DisplayLinkCoreAnimationBackend::armDelayTimer(CFTimeInterval fireMediaTime)
{
    ASSERT(m_runLoop->isCurrent());
    if (!m_delayTimer) {
        CFRunLoopTimerContext context { 0, this, nullptr, nullptr, nullptr };
        // Long-lived timer, re-armed with CFRunLoopTimerSetNextFireDate() on every tick.
        m_delayTimer = adoptCF(CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0, delayTimerFired, &context));
        RetainPtr runLoop = CFRunLoopGetCurrent();
        CFRunLoopAddTimer(runLoop.get(), m_delayTimer.get(), kCFRunLoopCommonModes);
    }
    CFTimeInterval delay = std::max<CFTimeInterval>(0, fireMediaTime - CACurrentMediaTime());
    // On non-realtime threads the kernel fires normal-urgency timers late by about min(delay / 8, 1ms); compensate.
    if (m_options.threadPolicy != DisplayLinkThreadPolicy::TimeConstraint)
        delay -= std::min(delay / 9, 0.001);
    CFRunLoopTimerSetNextFireDate(m_delayTimer.get(), CFAbsoluteTimeGetCurrent() + delay);
}

void DisplayLinkCoreAnimationBackend::disarmDelayTimer()
{
    m_delayedNotificationPending = false;
    if (m_delayTimer)
        CFRunLoopTimerSetNextFireDate(m_delayTimer.get(), CFAbsoluteTimeGetCurrent() + 1e9);
}

void DisplayLinkCoreAnimationBackend::recordStatistics(CADisplayLink *displayLink)
{
    auto now = CACurrentMediaTime();
    auto timestamp = displayLink.timestamp;
    auto& statistics = m_statistics;

    if (!statistics.windowStart)
        statistics.windowStart = now;
    ++statistics.ticks;
    statistics.callbackLatencies.append((now - timestamp) * 1000);
    if (statistics.lastTimestamp)
        statistics.tickIntervals.append((timestamp - statistics.lastTimestamp) * 1000);
    statistics.lastTimestamp = timestamp;
    statistics.lastDuration = displayLink.duration * 1000;
    statistics.lastPeriod = (displayLink.targetTimestamp - timestamp) * 1000;

    if (now - statistics.windowStart >= statisticsInterval.seconds())
        flushStatistics(now);
}

void DisplayLinkCoreAnimationBackend::flushStatistics(CFTimeInterval now)
{
    auto& statistics = m_statistics;
    auto elapsed = now - statistics.windowStart;
    if (statistics.windowStart && statistics.ticks && elapsed > 0) {
        auto percentile = [](Vector<double>& values, double fraction) -> double {
            if (values.isEmpty())
                return 0;
            std::sort(values.begin(), values.end());
            return values[std::min<size_t>(values.size() - 1, static_cast<size_t>(fraction * values.size()))];
        };

        RELEASE_LOG(DisplayLink, "[UI ] CADisplayLink stats display %u: %.1f ticks/s %.1f notifications/s (%u late); callback-vsync ms p50 %.3f p95 %.3f max %.3f; notify-vsync ms p50 %.3f p95 %.3f max %.3f; tick interval ms p50 %.3f p95 %.3f; duration %.3f period %.3f",
            m_displayID, statistics.ticks / elapsed, statistics.notifications / elapsed, statistics.lateNotifications,
            percentile(statistics.callbackLatencies, 0.5), percentile(statistics.callbackLatencies, 0.95), percentile(statistics.callbackLatencies, 1),
            percentile(statistics.notificationLatencies, 0.5), percentile(statistics.notificationLatencies, 0.95), percentile(statistics.notificationLatencies, 1),
            percentile(statistics.tickIntervals, 0.5), percentile(statistics.tickIntervals, 0.95),
            statistics.lastDuration, statistics.lastPeriod);
    }

    statistics.windowStart = 0;
    statistics.ticks = 0;
    statistics.notifications = 0;
    statistics.lateNotifications = 0;
    statistics.callbackLatencies.shrink(0);
    statistics.notificationLatencies.shrink(0);
    statistics.tickIntervals.shrink(0);
}

static NSScreen *screenForDisplay(PlatformDisplayID displayID)
{
    // Display 0 is used by windowless and offscreen views; CVDisplayLink maps it to the main display, so do the same.
    PlatformDisplayID targetDisplayID = displayID ? displayID : CGMainDisplayID();
    for (NSScreen *screen in NSScreen.screens) {
        if (WebCore::displayID(screen) == targetDisplayID)
            return screen;
    }
    for (NSScreen *screen in NSScreen.screens) {
        if (WebCore::displayID(screen) == CGMainDisplayID())
            return screen;
    }
    return NSScreen.screens.firstObject;
}

static DisplayLinkThreadPolicy threadPolicyFromDefaults(NSUserDefaults *defaults)
{
    NSString *value = [defaults stringForKey:@"WebKitDebugDisplayLinkThreadPolicy"];
    if ([value isEqualToString:@"fixed"])
        return DisplayLinkThreadPolicy::Fixed;
    if ([value isEqualToString:@"timeConstraint"])
        return DisplayLinkThreadPolicy::TimeConstraint;
    return DisplayLinkThreadPolicy::QoS;
}

bool displayLinkStatisticsLoggingEnabled()
{
    return [NSUserDefaults.standardUserDefaults boolForKey:@"WebKitDebugDisplayLinkLogStatistics"];
}

RefPtr<DisplayLinkPlatformBackend> createCoreAnimationDisplayLinkBackendIfEnabled(DisplayLink& client, PlatformDisplayID displayID)
{
    ASSERT(RunLoop::isMain());

    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if (![[defaults stringForKey:@"WebKitDebugDisplayLinkBackend"] isEqualToString:@"CoreAnimation"])
        return nullptr;

    NSScreen *screen = screenForDisplay(displayID);
    if (!screen) {
        RELEASE_LOG_ERROR(DisplayLink, "[UI ] No NSScreen for display %u; falling back to CVDisplayLink", displayID);
        return nullptr;
    }

    DisplayLinkCoreAnimationBackendOptions options;
    options.callbackDelayFraction = std::clamp([defaults doubleForKey:@"WebKitDebugDisplayLinkCallbackDelayFraction"], 0.0, maximumCallbackDelayFraction);
    options.threadPolicy = threadPolicyFromDefaults(defaults);
    options.logStatistics = displayLinkStatisticsLoggingEnabled();

    return DisplayLinkCoreAnimationBackend::create(client, displayID, screen, options);
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
            backend->displayLinkFired(displayLink);
    }
}

@end

#endif // HAVE(DISPLAY_LINK) && PLATFORM(MAC)
