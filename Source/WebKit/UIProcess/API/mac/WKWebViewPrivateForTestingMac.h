/*
 * Copyright (C) 2014-2019 Apple Inc. All rights reserved.
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

#import <WebKit/WKBase.h>
#import <WebKit/WKWebView.h>

#if !TARGET_OS_IPHONE

@class _WKFrameHandle;

@interface WKWebView (WKTestingMac)

@property (nonatomic, readonly) BOOL _hasActiveVideoForControlsManager;
@property (nonatomic, readonly) BOOL _shouldRequestCandidates;
@property (nonatomic, readonly) BOOL _allowsInlinePredictions;
@property (nonatomic, readonly) NSMenu *_activeMenu;

- (void)_requestControlledElementID;
- (void)_handleControlledElementIDResponse:(NSString *)identifier;

- (void)_handleAcceptedCandidate:(NSTextCheckingResult *)candidate;
- (void)_didHandleAcceptedCandidate;

- (void)_forceRequestCandidates;
- (void)_didUpdateCandidateListVisibility:(BOOL)visible;

- (void)_insertText:(id)string replacementRange:(NSRange)replacementRange;
- (NSRect)_candidateRect;

- (NSSet<NSView *> *)_pdfHUDs;

- (void)_retrieveAccessibilityTreeData:(void (^)(NSData *, NSError *))completionHandler;

- (void)_setSelectedColorForColorPicker:(NSColor *)color;

@property (nonatomic, readonly) BOOL _secureEventInputEnabledForTesting;
@property (nonatomic, readonly) NSRect _windowRelativeBoundsForCustomSwipeViewsForTesting;

- (void)_createFlagsChangedEventMonitorForTesting;
- (BOOL)_hasFlagsChangedEventMonitorForTesting;

@property (nonatomic, readonly) CGFloat _fullScreenTitlebarOverlayHeightForTesting;

// The value NSRefreshControl reads through the NSRefreshControlHosting protocol to decide
// whether a pull may commit to a refresh. False during momentum so a flick cannot refresh.
@property (nonatomic, readonly) BOOL _refreshControlHostIsTrackingForTesting;

- (BOOL)isPointInScrollbar:(NSPoint)locationInView;

// The page's display link: "requestedFramesPerSecond" (the rate DisplayLink asks the platform display link for; 0 when
// it isn't running), "nominalFramesPerSecond", "supportsPreferredFramesPerSecond" (whether the platform display link
// runs at the requested rate), "observerFramesPerSecond" (the rate each observer asks for), and "isRunning" (whether the
// platform display link fires). nil when the page has no display link.
- (NSDictionary<NSString *, id> *)_displayLinkFrameRatesForTesting;
// Makes the page's display link, and the pages on its display, use this nominal rate, as if the display's refresh rate
// changed; 0 restores the display's rate.
- (void)_setDisplayLinkNominalFramesPerSecondForTesting:(NSUInteger)framesPerSecond;

@end

#endif // !TARGET_OS_IPHONE
