/*
 * Copyright (C) 2018-2019 Apple Inc. All rights reserved.
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

#if HAVE(DISPLAY_LINK)

#include "DisplayLinkObserverID.h"
#include <WebCore/AnimationFrameRate.h>
#include <WebCore/DisplayUpdate.h>
#include <WebCore/PlatformScreen.h>
#include <wtf/CheckedPtr.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>

#if PLATFORM(MAC)
#include <CoreGraphics/CGDisplayConfiguration.h>
#include <WebCore/CoreVideoExtras.h>
#include <wtf/ThreadSafeRefCounted.h>
#endif

#if PLATFORM(GTK) || PLATFORM(WPE)
#include "DisplayVBlankMonitor.h"
#endif

#if USE(WPE_BACKEND_PLAYSTATION)
struct wpe_playstation_display;
#endif

namespace WebKit {

#if PLATFORM(MAC)
class DisplayLink;

// The platform display link that drives a DisplayLink. Ref-counted so that a backend with its own thread can keep
// itself alive across the work it dispatches there.
class DisplayLinkPlatformBackend : public ThreadSafeRefCounted<DisplayLinkPlatformBackend> {
public:
    virtual ~DisplayLinkPlatformBackend() = default;

    virtual WebCore::FramesPerSecond nominalFramesPerSecond() const = 0;
    virtual bool isRunning() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    // Called on the main thread before the DisplayLink is destroyed; no callback reaches the DisplayLink afterwards.
    virtual void invalidate() = 0;
    // Called on the main thread at the end of a reconfiguration of `displayID` (not necessarily the backend's display).
    // Returns whether the nominal rate may have changed.
    virtual bool displayConfigurationChanged(WebCore::PlatformDisplayID, CGDisplayChangeSummaryFlags) { return false; }

    // Whether the platform display link can fire below the display's rate. DisplayLink then asks it for the divisor of
    // the nominal rate its observers need, and the backend counts the updates at the rate the link fires at.
    virtual bool supportsFrameRateDivisor() const { return false; }
    // Called on the main thread: fire every `divisor` vsyncs, 1 meaning at the display's rate.
    virtual void setFrameRateDivisor(unsigned) { }

    struct StateForTesting {
        // Whether the backend is to run.
        bool isRunning { false };
        // The platform display link's callbacks since recording started.
        uint64_t tickCount { 0 };
        // The rate the backend counts updates at, which is the rate the platform display link fires at, as of its last
        // tick since recording started; 0 until then.
        WebCore::FramesPerSecond countedFramesPerSecond { 0 };
        // The divisor DisplayLink asked for.
        unsigned requestedDivisor { 0 };
        // The divisor the platform display link was asked for, 1 when it didn't get the requested one, as of its last
        // tick since recording started; 0 until then.
        unsigned appliedDivisor { 0 };
        // The requests the platform display link didn't grant since the backend was created.
        unsigned denialCount { 0 };
        // The notifications sent part way into a vsync, after a tick, since recording started, and the earliest of them,
        // in refresh intervals after the vsync.
        uint64_t delayedNotificationCount { 0 };
        double minimumDelayedNotificationPhase { 0 };
    };
    // For backends that count their updates. Ticks are recorded from the first call to startRecordingStateForTesting(),
    // so that until then, a tick only reads whether to record.
    virtual void startRecordingStateForTesting() { }
    virtual std::optional<StateForTesting> stateForTesting() const { return std::nullopt; }

protected:
    // Called by the backend on its display link thread, once per tick. A backend that supports a frame rate divisor
    // passes the update it counted.
    static void displayLinkFired(DisplayLink&);
    static void displayLinkFired(DisplayLink&, WebCore::DisplayUpdate);
};

// Returns null unless the WebKitDebugDisplayLinkBackend default is "CoreAnimation" and the display has a screen.
RefPtr<DisplayLinkPlatformBackend> createCoreAnimationDisplayLinkBackendIfEnabled(DisplayLink&, WebCore::PlatformDisplayID);
#endif

class DisplayLink {
    WTF_MAKE_TZONE_ALLOCATED(DisplayLink);
public:
    class Client : public CanMakeThreadSafeCheckedPtr<Client> {
        WTF_MAKE_TZONE_ALLOCATED(Client);
        WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(Client);
    friend class DisplayLink;
    public:
        virtual ~Client() = default;

    private:
        virtual void displayLinkFired(WebCore::PlatformDisplayID, WebCore::DisplayUpdate, bool wantsFullSpeedUpdates, bool anyObserverWantsCallback) = 0;
    };

    explicit DisplayLink(WebCore::PlatformDisplayID);
    ~DisplayLink();

    WebCore::PlatformDisplayID displayID() const { return m_displayID; }
    WebCore::FramesPerSecond nominalFramesPerSecond() const { return m_displayNominalFramesPerSecond; }

#if PLATFORM(MAC)
    // Called on the main thread for each reconfiguration callback of `displayID`: this DisplayLink's display, or any
    // display for display 0's DisplayLink, which follows the main display.
    void displayPropertiesChanged(WebCore::PlatformDisplayID, CGDisplayChangeSummaryFlags);
#endif

    void addObserver(Client&, DisplayLinkObserverID, WebCore::FramesPerSecond);
    void removeObserver(Client&, DisplayLinkObserverID);

    void removeClient(Client&);

    // FIXME: Maybe callers should just register a DisplayLinkObserverID with the appropriate fps.
    void incrementFullSpeedRequestClientCount(Client&);
    void decrementFullSpeedRequestClientCount(Client&);

    void setObserverPreferredFramesPerSecond(Client&, DisplayLinkObserverID, WebCore::FramesPerSecond);

#if PLATFORM(GTK) || PLATFORM(WPE)
    DisplayVBlankMonitor& vblankMonitor() const LIFETIME_BOUND { return *m_vblankMonitor; }
#endif

#if PLATFORM(MAC)
    // The state of a platform display link that counts its updates (CADisplayLink), whose ticks are recorded from the
    // first call; std::nullopt for CVDisplayLink.
    std::optional<DisplayLinkPlatformBackend::StateForTesting> platformStateForTesting();
#endif

private:
#if PLATFORM(MAC)
    friend class DisplayLinkPlatformBackend;
    // The rate each observer asks for; a client that wants full-speed updates counts as one more at the nominal rate.
    Vector<WebCore::FramesPerSecond, 4> observerFramesPerSecond() const WTF_REQUIRES_LOCK(m_clientsLock);
    void updatePlatformFrameRateDivisor() WTF_REQUIRES_LOCK(m_clientsLock);
#endif
    // `update` is passed by a platform display link that counts its updates itself, because it can fire below the
    // nominal rate.
    void notifyObserversDisplayDidRefresh(std::optional<WebCore::DisplayUpdate> = std::nullopt);

    void platformInitialize();
    void platformFinalize();
    bool platformIsRunning() const;
    void platformStart();
    void platformStop();

    bool removeInfoForClientIfUnused(Client&) WTF_REQUIRES_LOCK(m_clientsLock);

    struct ObserverInfo {
        DisplayLinkObserverID observerID;
        WebCore::FramesPerSecond preferredFramesPerSecond;
    };

    struct ClientInfo {
        unsigned fullSpeedUpdatesClientCount { 0 };
        Vector<ObserverInfo, 1> observers;
    };

#if PLATFORM(MAC)
    RefPtr<DisplayLinkPlatformBackend> m_platformBackend;
    bool m_platformSupportsFrameRateDivisor { false };
    // The divisor last asked of the backend; 0 until the first request.
    unsigned m_platformFrameRateDivisor WTF_GUARDED_BY_LOCK(m_clientsLock) { 0 };
#endif
#if PLATFORM(GTK) || PLATFORM(WPE)
    std::unique_ptr<DisplayVBlankMonitor> m_vblankMonitor;
    unsigned m_fpsThrottleRatio { 1 };
    unsigned m_fpsThrottleCallCounter { 0 };
#endif
#if USE(WPE_BACKEND_PLAYSTATION)
    struct wpe_playstation_display* m_display;
#endif
    Lock m_clientsLock;
    HashMap<CheckedRef<Client>, ClientInfo> m_clients WTF_GUARDED_BY_LOCK(m_clientsLock);
    const WebCore::PlatformDisplayID m_displayID;
    // With a backend that follows display reconfigurations, written on the main thread under m_clientsLock.
    WebCore::FramesPerSecond m_displayNominalFramesPerSecond { WebCore::FullSpeedFramesPerSecond };
    // Written under m_clientsLock on the display link thread, and without it by addObserver() on the main thread while
    // the display link isn't running, unless the platform display link counts its updates and passes each one.
    WebCore::DisplayUpdate m_currentUpdate;
    unsigned m_fireCountWithoutObservers { 0 };
};

class DisplayLinkCollection {
public:
    DisplayLink& displayLinkForDisplay(WebCore::PlatformDisplayID) LIFETIME_BOUND;
    DisplayLink* NODELETE existingDisplayLinkForDisplay(WebCore::PlatformDisplayID) const LIFETIME_BOUND;

    std::optional<unsigned> nominalFramesPerSecondForDisplay(WebCore::PlatformDisplayID);
    void startDisplayLink(DisplayLink::Client&, DisplayLinkObserverID, WebCore::PlatformDisplayID, WebCore::FramesPerSecond preferredFramesPerSecond);
    void stopDisplayLink(DisplayLink::Client&, DisplayLinkObserverID, WebCore::PlatformDisplayID);
    void stopDisplayLinks(DisplayLink::Client&);
    void setDisplayLinkPreferredFramesPerSecond(DisplayLink::Client&, DisplayLinkObserverID, WebCore::PlatformDisplayID, WebCore::FramesPerSecond preferredFramesPerSecond);
    void setDisplayLinkForDisplayWantsFullSpeedUpdates(DisplayLink::Client&, WebCore::PlatformDisplayID, bool wantsFullSpeedUpdates);

private:
    void add(std::unique_ptr<DisplayLink>&&);

    Vector<std::unique_ptr<DisplayLink>> m_displayLinks;
};

} // namespace WebKit

#endif // HAVE(DISPLAY_LINK)
