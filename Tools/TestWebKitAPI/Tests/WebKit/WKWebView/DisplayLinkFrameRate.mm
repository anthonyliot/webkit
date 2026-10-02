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

#if PLATFORM(MAC)

#import "DisplayLinkRateController.h"
#import "PlatformUtilities.h"
#import "Test.h"
#import "TestWKWebView.h"
#import "Utilities.h"
#import <WebCore/AnimationFrameRate.h>
#import <WebKit/WKPreferencesPrivate.h>
#import <WebKit/WKProcessPoolPrivate.h>
#import <WebKit/WKWebViewConfigurationPrivate.h>
#import <WebKit/WKWebViewPrivate.h>
#import <WebKit/WKWebViewPrivateForTesting.h>
#import <WebKit/_WKFeature.h>
#import <wtf/MonotonicTime.h>
#import <wtf/RetainPtr.h>

namespace TestWebKitAPI {

namespace {

NSDictionary *waitForDisplayLinkState(TestWKWebView *webView, bool (^isExpected)(NSDictionary *))
{
    NSDictionary *state = nil;
    for (unsigned i = 0; i < 100; ++i) {
        state = webView._displayLinkStateForTesting;
        if (state && isExpected(state))
            return state;
        Util::runFor(0.05_s);
    }
    return state;
}

struct MeasuredRates {
    double ticksPerSecond { 0 };
    double animationFramesPerSecond { 0 };
    double delayedNotificationsPerSecond { 0 };
};

MeasuredRates measureRates(TestWKWebView *webView, Seconds duration)
{
    auto frameCount = [&] {
        return [[webView objectByEvaluatingJavaScript:@"frameCount"] unsignedLongLongValue];
    };
    auto tickCount = [&] {
        return [webView._displayLinkStateForTesting[@"tickCount"] unsignedLongLongValue];
    };
    auto delayedNotificationCount = [&] {
        return [webView._displayLinkStateForTesting[@"delayedNotificationCount"] unsignedLongLongValue];
    };

    auto startFrames = frameCount();
    auto startTicks = tickCount();
    auto startDelayedNotifications = delayedNotificationCount();
    auto start = MonotonicTime::now();
    Util::runFor(duration);
    auto endTicks = tickCount();
    auto endDelayedNotifications = delayedNotificationCount();
    auto endFrames = frameCount();
    double elapsed = (MonotonicTime::now() - start).seconds();
    return { (endTicks - startTicks) / elapsed, (endFrames - startFrames) / elapsed, (endDelayedNotifications - startDelayedNotifications) / elapsed };
}

// The rate of the screen of `displayID`; display 0 follows the main display.
unsigned screenFramesPerSecond(uint32_t displayID)
{
    auto screenDisplayID = displayID ? displayID : CGMainDisplayID();
    for (NSScreen *screen in NSScreen.screens) {
        if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == screenDisplayID)
            return static_cast<unsigned>(screen.maximumFramesPerSecond);
    }
    return 0;
}

NSString * const animationPage = @"<div id='box' style='width: 50px; height: 50px; background: blue'></div><script>"
    "window.frameCount = 0;"
    "window.animating = true;"
    "function tick(timestamp) { if (!animating) return; box.style.transform = `translateX(${(timestamp / 5) % 300}px)`; ++frameCount; requestAnimationFrame(tick); }"
    "function startAnimating() { if (animating) return; animating = true; requestAnimationFrame(tick); }"
    "requestAnimationFrame(tick);"
    "</script>";

// A web view in its own process pool, whose DisplayLinks use the CADisplayLink backend: the backend is chosen when a
// DisplayLink is created, and each process pool has its own DisplayLinks.
class CoreAnimationDisplayLinkTest {
public:
    CoreAnimationDisplayLinkTest()
        : m_argumentDomain([NSUserDefaults.standardUserDefaults volatileDomainForName:NSArgumentDomain])
    {
        // The argument domain isn't saved, so a test that doesn't finish leaves no default behind.
        RetainPtr argumentDomainWithBackend = adoptNS(m_argumentDomain ? [m_argumentDomain mutableCopy] : [[NSMutableDictionary alloc] init]);
        [argumentDomainWithBackend setObject:@"CoreAnimation" forKey:@"WebKitDebugDisplayLinkBackend"];
        [NSUserDefaults.standardUserDefaults setVolatileDomain:argumentDomainWithBackend.get() forName:NSArgumentDomain];
    }

    ~CoreAnimationDisplayLinkTest()
    {
        if (m_argumentDomain)
            [NSUserDefaults.standardUserDefaults setVolatileDomain:m_argumentDomain.get() forName:NSArgumentDomain];
        else
            [NSUserDefaults.standardUserDefaults removeVolatileDomainForName:NSArgumentDomain];
    }

    // Loads `html`, and waits for the page's display link to run. Returns false, after logging why, when there is
    // nothing to test.
    bool load(NSString *html, const char* testName)
    {
        if (!NSScreen.screens.count) {
            // Without a screen, DisplayLink uses CVDisplayLink.
            // FIXME: Use GTEST_SKIP() once webkit.org/b/321271 is resolved, as in UnifiedPDFTests.mm.
            NSLog(@"DisplayLinkFrameRate.%s: skipped, no screen", testName);
            return false;
        }

        RetainPtr configuration = adoptNS([[WKWebViewConfiguration alloc] init]);
        configuration.get().processPool = adoptNS([[WKProcessPool alloc] init]).get();
        for (_WKFeature *feature in [WKPreferences _features]) {
            if ([feature.key isEqualToString:@"WebAnimationsCustomFrameRateEnabled"])
                [configuration.get().preferences _setEnabled:YES forFeature:feature];
        }
        m_webView = adoptNS([[TestWKWebView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300) configuration:configuration.get() addToWindow:YES]);
        // So that the page keeps its rendering rate whatever covers the window.
        [m_webView _setWindowOcclusionDetectionEnabled:NO];
        [m_webView synchronouslyLoadHTMLString:html];

        NSDictionary *state = waitForDisplayLinkState(m_webView.get(), ^(NSDictionary *state) {
            return [state[@"isRunning"] boolValue];
        });
        EXPECT_NOT_NULL(state);
        // Not a fallback to CVDisplayLink.
        EXPECT_WK_STREQ(@"CoreAnimation", state[@"backend"]);
        if (![state[@"backend"] isEqualToString:@"CoreAnimation"])
            return false;
        m_nominalFramesPerSecond = [state[@"nominalFramesPerSecond"] unsignedIntValue];
        m_displayID = [state[@"displayID"] unsignedIntValue];
        auto framesPerSecond = screenFramesPerSecond(m_displayID);
        EXPECT_EQ(m_nominalFramesPerSecond, framesPerSecond);
        if (framesPerSecond < 60) {
            NSLog(@"DisplayLinkFrameRate.%s: skipped, the display runs at %u Hz", testName, framesPerSecond);
            return false;
        }
        return true;
    }

    // Checks that the link fires at the rate of the divisor DisplayLink asks for a page at `pageFramesPerSecond`, and
    // that the page gets its requestAnimationFrame callbacks at that rate.
    void expectFramesPerSecond(WebCore::FramesPerSecond pageFramesPerSecond)
    {
        std::array demands { pageFramesPerSecond };
        auto divisor = WebKit::displayLinkFrameRateDivisor(m_nominalFramesPerSecond, demands);
        // The divisors of the usual display rates, independently of the function.
        if (auto expected = expectedDivisor(pageFramesPerSecond))
            EXPECT_EQ(divisor, *expected);
        double linkFramesPerSecond = static_cast<double>(m_nominalFramesPerSecond) / divisor;
        // Values of ticks since the wait started, not of the ones before a stop.
        auto startTickCount = [[m_webView _displayLinkStateForTesting][@"tickCount"] unsignedLongLongValue];
        NSDictionary *state = waitForDisplayLinkState(m_webView.get(), ^(NSDictionary *state) {
            return [state[@"tickCount"] unsignedLongLongValue] > startTickCount + 2
                && [state[@"requestedDivisor"] unsignedIntValue] == divisor
                && [state[@"countedFramesPerSecond"] unsignedIntValue] == m_nominalFramesPerSecond / divisor;
        });
        EXPECT_EQ([state[@"requestedDivisor"] unsignedIntValue], divisor);
        // The link fires at the requested rate: Core Animation granted it.
        EXPECT_EQ([state[@"appliedDivisor"] unsignedIntValue], divisor);
        EXPECT_EQ([state[@"countedFramesPerSecond"] unsignedIntValue], m_nominalFramesPerSecond / divisor);

        // On displays up to 240 Hz, the next faster rate is at least 14% higher (240 / 7 against 240 / 8). The page, and
        // on a busy machine the link, may miss frames, but neither gets more.
        auto rates = measureRates(m_webView.get(), 2_s);
        EXPECT_LT(rates.ticksPerSecond, 1.1 * linkFramesPerSecond);
        EXPECT_GT(rates.ticksPerSecond, 0.5 * linkFramesPerSecond);
        EXPECT_LT(rates.animationFramesPerSecond, 1.1 * linkFramesPerSecond);
        // DisplayLink is notified part way into the vsync, after the tick, unless the tick itself comes later than that,
        // which a busy machine makes more likely; never before that point of the vsync, and not always well after it.
        EXPECT_GT(rates.delayedNotificationsPerSecond, 0.5 * rates.ticksPerSecond);
        double minimumPhase = [[m_webView _displayLinkStateForTesting][@"minimumDelayedNotificationPhase"] doubleValue];
        EXPECT_GE(minimumPhase, WebKit::displayLinkNotificationPhase - 0.001);
        EXPECT_LT(minimumPhase, 0.25);
        // Rendering at the display's rate may be too much for a busy machine.
        if (divisor > 1)
            EXPECT_GT(rates.animationFramesPerSecond, 0.5 * linkFramesPerSecond);
    }

    // Waits for the display link to stop for want of rendering updates, and checks that it doesn't fire meanwhile.
    void expectStopped()
    {
        NSDictionary *state = waitForDisplayLinkState(m_webView.get(), ^(NSDictionary *state) {
            return ![state[@"isRunning"] boolValue];
        });
        EXPECT_FALSE([state[@"isRunning"] boolValue]);
        auto stoppedTickCount = [[m_webView _displayLinkStateForTesting][@"tickCount"] unsignedLongLongValue];
        Util::runFor(0.3_s);
        EXPECT_EQ([[m_webView _displayLinkStateForTesting][@"tickCount"] unsignedLongLongValue], stoppedTickCount);
    }

    // Waits for the page's display link to be the running one of `displayID`, with CADisplayLink at the screen's rate,
    // and makes it the one the checks are of.
    bool waitForDisplay(unsigned displayID)
    {
        NSDictionary *state = waitForDisplayLinkState(m_webView.get(), ^(NSDictionary *state) {
            return [state[@"displayID"] unsignedIntValue] == displayID && [state[@"isRunning"] boolValue];
        });
        EXPECT_EQ([state[@"displayID"] unsignedIntValue], displayID);
        EXPECT_WK_STREQ(@"CoreAnimation", state[@"backend"]);
        if ([state[@"displayID"] unsignedIntValue] != displayID || ![state[@"backend"] isEqualToString:@"CoreAnimation"])
            return false;
        m_displayID = displayID;
        m_nominalFramesPerSecond = [state[@"nominalFramesPerSecond"] unsignedIntValue];
        EXPECT_EQ(m_nominalFramesPerSecond, screenFramesPerSecond(displayID));
        return true;
    }

    TestWKWebView *webView() const { return m_webView.get(); }
    WebCore::FramesPerSecond nominalFramesPerSecond() const { return m_nominalFramesPerSecond; }
    unsigned displayID() const { return m_displayID; }

private:
    std::optional<unsigned> expectedDivisor(WebCore::FramesPerSecond pageFramesPerSecond) const
    {
        if (pageFramesPerSecond == m_nominalFramesPerSecond)
            return 1;
        if (m_nominalFramesPerSecond == 60 || m_nominalFramesPerSecond == 120 || m_nominalFramesPerSecond == 240) {
            if (pageFramesPerSecond == 60)
                return m_nominalFramesPerSecond / 60;
            if (pageFramesPerSecond == 30)
                return m_nominalFramesPerSecond / 30;
        }
        return std::nullopt;
    }

    RetainPtr<NSDictionary> m_argumentDomain;
    RetainPtr<TestWKWebView> m_webView;
    WebCore::FramesPerSecond m_nominalFramesPerSecond { 0 };
    unsigned m_displayID { 0 };
};

} // namespace

// With the CADisplayLink backend, the platform display link fires at the rate DisplayLink asks for, and a page gets its
// requestAnimationFrame callbacks at that rate: at the page's usual rate (the display's rate below 120 Hz), again after
// the link stopped without rendering updates and resumed, and at 30 fps, where the link has to fire below the display's
// rate on any display.
TEST(DisplayLinkFrameRate, CoreAnimationBackendFiresAtTheRequestedRate)
{
    CoreAnimationDisplayLinkTest test;
    if (!test.load(animationPage, "CoreAnimationBackendFiresAtTheRequestedRate"))
        return;

    auto pageFramesPerSecond = WebCore::framesPerSecondNearestFullSpeed(test.nominalFramesPerSecond());
    test.expectFramesPerSecond(pageFramesPerSecond);

    [test.webView() objectByEvaluatingJavaScript:@"animating = false; 0"];
    test.expectStopped();
    [test.webView() objectByEvaluatingJavaScript:@"startAnimating(); 0"];
    test.expectFramesPerSecond(pageFramesPerSecond);

    // The page renders at 30 fps, as for a 30 Hz display; the link fires at the slowest rate of its own display that is
    // at least 30 fps.
    [test.webView() _setDisplayForTesting:test.displayID() nominalFramesPerSecond:30];
    test.expectFramesPerSecond(30);
}

// An animation that asks for the highest frame rate makes the page render at the display's rate, and the running link
// then fires at the display's rate too, with the default frame rate range. Below 120 Hz, it already did.
// (After the animation, WebCore keeps the page at the display's rate, so the test doesn't slow it down again.)
TEST(DisplayLinkFrameRate, CoreAnimationBackendFiresAtTheDisplaysRate)
{
    CoreAnimationDisplayLinkTest test;
    if (!test.load([animationPage stringByAppendingString:@"<div id='other' style='position: relative; width: 10px; height: 10px'></div>"], "CoreAnimationBackendFiresAtTheDisplaysRate"))
        return;
    test.expectFramesPerSecond(WebCore::framesPerSecondNearestFullSpeed(test.nominalFramesPerSecond()));
    // A property that isn't accelerated, so that the animation needs rendering updates.
    [test.webView() objectByEvaluatingJavaScript:@"other.animate({ left: ['0px', '100px'] }, { duration: 1000, iterations: Infinity, frameRate: 'highest' }); 0"];
    test.expectFramesPerSecond(test.nominalFramesPerSecond());
}

// While a page scrolls, the scrolling thread observes the link at the display's rate, and the link fires at that rate,
// as CVDisplayLink does; afterwards, at the page's rate again. Below 120 Hz, pages render at the display's rate.
TEST(DisplayLinkFrameRate, CoreAnimationBackendFiresAtTheDisplaysRateWhileScrolling)
{
    CoreAnimationDisplayLinkTest test;
    if (!test.load([animationPage stringByAppendingString:@"<div style='height: 5000px'></div>"], "CoreAnimationBackendFiresAtTheDisplaysRateWhileScrolling"))
        return;
    auto pageFramesPerSecond = WebCore::framesPerSecondNearestFullSpeed(test.nominalFramesPerSecond());
    if (pageFramesPerSecond == test.nominalFramesPerSecond()) {
        NSLog(@"DisplayLinkFrameRate.CoreAnimationBackendFiresAtTheDisplaysRateWhileScrolling: skipped, the page renders at the display's rate");
        return;
    }
    test.expectFramesPerSecond(pageFramesPerSecond);

    // A scroll that lasts as long as the check: a wheel event every 50 ms.
    TestWKWebView *webView = test.webView();
    [webView wheelEventAtPoint:NSMakePoint(200, 150) wheelDelta:CGSizeMake(0, -1) phase:kCGScrollPhaseBegan momentumPhase:kCGMomentumScrollPhaseNone];
    RetainPtr timer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *) {
        [webView wheelEventAtPoint:NSMakePoint(200, 150) wheelDelta:CGSizeMake(0, -5) phase:kCGScrollPhaseChanged momentumPhase:kCGMomentumScrollPhaseNone];
    }];
    test.expectFramesPerSecond(test.nominalFramesPerSecond());
    [timer invalidate];
    [webView wheelEventAtPoint:NSMakePoint(200, 150) wheelDelta:CGSizeMake(0, 0) phase:kCGScrollPhaseEnded momentumPhase:kCGMomentumScrollPhaseNone];
    test.expectFramesPerSecond(pageFramesPerSecond);
}

// A page at the display's rate over 7 gets divisor 7, which Core Animation didn't grant where this was run (at 240 Hz,
// the link fired every 6 vsyncs): the link keeps firing at another rate, and falls back to the display's rate. When it
// resumes, it asks for divisor 7 again.
TEST(DisplayLinkFrameRate, CoreAnimationBackendFallsBackToTheDisplaysRate)
{
    CoreAnimationDisplayLinkTest test;
    if (!test.load(animationPage, "CoreAnimationBackendFallsBackToTheDisplaysRate"))
        return;
    auto nominalFramesPerSecond = test.nominalFramesPerSecond();
    [test.webView() _setDisplayForTesting:test.displayID() nominalFramesPerSecond:nominalFramesPerSecond / 7];
    // The link counts at the rate it fires at, so it counts at the display's rate over 7 only if Core Animation granted
    // divisor 7. It counts at the display's rate after a denial, and below 120 Hz, before the request too: the page's
    // usual rate is then the display's rate.
    auto wasGranted = [&](NSDictionary *state) {
        return ![state[@"denialCount"] unsignedIntValue] && [state[@"appliedDivisor"] unsignedIntValue] == 7
            && [state[@"countedFramesPerSecond"] unsignedIntValue] == nominalFramesPerSecond / 7;
    };
    auto fellBack = [&](NSDictionary *state) {
        return [state[@"denialCount"] unsignedIntValue] && [state[@"appliedDivisor"] unsignedIntValue] == 1
            && [state[@"countedFramesPerSecond"] unsignedIntValue] == nominalFramesPerSecond;
    };
    NSDictionary *state = waitForDisplayLinkState(test.webView(), ^(NSDictionary *state) {
        return [state[@"requestedDivisor"] unsignedIntValue] == 7 && (fellBack(state) || wasGranted(state));
    });
    EXPECT_EQ([state[@"requestedDivisor"] unsignedIntValue], 7u);
    if (wasGranted(state)) {
        NSLog(@"DisplayLinkFrameRate.CoreAnimationBackendFallsBackToTheDisplaysRate: Core Animation granted divisor 7; nothing to fall back from");
        return;
    }
    EXPECT_EQ([state[@"appliedDivisor"] unsignedIntValue], 1u);
    EXPECT_EQ([state[@"countedFramesPerSecond"] unsignedIntValue], nominalFramesPerSecond);
    auto denialCount = [state[@"denialCount"] unsignedIntValue];
    EXPECT_GE(denialCount, 1u);
    // At the display's rate, not at the rate Core Animation gave for divisor 7 (6 vsyncs at 240 Hz). As with
    // CVDisplayLink, the page, whose rate doesn't divide the display's, then gets every tick, so it renders faster than
    // that rate too, though maybe not at the display's rate on a busy machine.
    auto rates = measureRates(test.webView(), 1_s);
    EXPECT_LT(rates.ticksPerSecond, 1.1 * nominalFramesPerSecond);
    EXPECT_GT(rates.ticksPerSecond, 0.5 * nominalFramesPerSecond);
    EXPECT_GT(rates.animationFramesPerSecond, 0.25 * nominalFramesPerSecond);

    [test.webView() objectByEvaluatingJavaScript:@"animating = false; 0"];
    test.expectStopped();
    [test.webView() objectByEvaluatingJavaScript:@"startAnimating(); 0"];
    state = waitForDisplayLinkState(test.webView(), ^(NSDictionary *state) {
        return [state[@"denialCount"] unsignedIntValue] > denialCount;
    });
    EXPECT_GT([state[@"denialCount"] unsignedIntValue], denialCount);
}

// When its window moves to another display, the page gets that display's DisplayLink, which uses CADisplayLink too, at
// the rate the page needs there; and back. It needs a second screen.
TEST(DisplayLinkFrameRate, CoreAnimationBackendFollowsTheWindowToAnotherDisplay)
{
    CoreAnimationDisplayLinkTest test;
    if (!test.load(animationPage, "CoreAnimationBackendFollowsTheWindowToAnotherDisplay"))
        return;
    NSScreen *firstScreen = nil;
    NSScreen *otherScreen = nil;
    for (NSScreen *screen in NSScreen.screens) {
        if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == test.displayID())
            firstScreen = screen;
        else if (!otherScreen && screen.maximumFramesPerSecond >= 60)
            otherScreen = screen;
    }
    if (!firstScreen || !otherScreen) {
        NSLog(@"DisplayLinkFrameRate.CoreAnimationBackendFollowsTheWindowToAnotherDisplay: skipped, no other screen at 60 Hz or more");
        return;
    }
    test.expectFramesPerSecond(WebCore::framesPerSecondNearestFullSpeed(test.nominalFramesPerSecond()));

    auto moveWindowTo = [&](NSScreen *screen) {
        [test.webView().window setFrameOrigin:NSMakePoint(NSMinX(screen.frame) + 100, NSMinY(screen.frame) + 100)];
        return test.waitForDisplay([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
    };
    if (moveWindowTo(otherScreen))
        test.expectFramesPerSecond(WebCore::framesPerSecondNearestFullSpeed(test.nominalFramesPerSecond()));
    if (moveWindowTo(firstScreen))
        test.expectFramesPerSecond(WebCore::framesPerSecondNearestFullSpeed(test.nominalFramesPerSecond()));
}

// Without the WebKitDebugDisplayLinkBackend default, DisplayLink uses CVDisplayLink.
TEST(DisplayLinkFrameRate, CoreVideoIsTheDefault)
{
    if ([NSUserDefaults.standardUserDefaults objectForKey:@"WebKitDebugDisplayLinkBackend"]) {
        NSLog(@"DisplayLinkFrameRate.CoreVideoIsTheDefault: skipped, WebKitDebugDisplayLinkBackend is set");
        return;
    }
    if (!NSScreen.screens.count) {
        NSLog(@"DisplayLinkFrameRate.CoreVideoIsTheDefault: skipped, no screen");
        return;
    }

    RetainPtr configuration = adoptNS([[WKWebViewConfiguration alloc] init]);
    configuration.get().processPool = adoptNS([[WKProcessPool alloc] init]).get();
    RetainPtr webView = adoptNS([[TestWKWebView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300) configuration:configuration.get() addToWindow:YES]);
    [webView _setWindowOcclusionDetectionEnabled:NO];
    [webView synchronouslyLoadHTMLString:animationPage];
    NSDictionary *state = waitForDisplayLinkState(webView.get(), ^(NSDictionary *) {
        return true;
    });
    ASSERT_NOT_NULL(state);
    EXPECT_WK_STREQ(@"CoreVideo", state[@"backend"]);
    // The rate pages are told: CVDisplayLink's nominal rate, and with the CADisplayLink backend, the screen's.
    EXPECT_EQ([state[@"nominalFramesPerSecond"] unsignedIntValue], screenFramesPerSecond([state[@"displayID"] unsignedIntValue]));
}

} // namespace TestWebKitAPI

#endif // PLATFORM(MAC)
