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
#include <wtf/MonotonicTime.h>
#include <wtf/Seconds.h>
#include <wtf/TZoneMalloc.h>

#if PLATFORM(MAC)
#include <wtf/ThreadSafeRefCounted.h>
#endif

#if PLATFORM(GTK) || PLATFORM(WPE)
#include "DisplayVBlankMonitor.h"
#endif

#if USE(WPE_BACKEND_PLAYSTATION)
struct wpe_playstation_display;
#endif

namespace WebKit {

// Timing of a display link tick, provided by platform display links that know it.
struct DisplayLinkFrameTiming {
    WebCore::FramesPerSecond linkFramesPerSecond { 0 }; // The rate the platform display link currently fires at.
    Seconds linkInterval; // The time between two ticks of the platform display link.
    MonotonicTime vsyncTime; // The vsync this tick corresponds to.
    Seconds refreshInterval; // The display's refresh interval.
    uint64_t vsyncIndex { 0 }; // The vsync count of this tick (vsyncTime / refreshInterval).
    unsigned linkDivisor { 0 }; // The number of vsyncs between two ticks of the platform display link.
    // The vsync index, modulo the divisor, on which the platform display link fires when it runs below the
    // refresh rate, if known. Cadences kept on this phase stay steady when the link changes rate.
    std::optional<unsigned> vsyncPhase;
};

#if PLATFORM(MAC)
class DisplayLink;

// The platform display link driving a DisplayLink: CVDisplayLink (the default) or CADisplayLink.
class DisplayLinkPlatformBackend : public ThreadSafeRefCounted<DisplayLinkPlatformBackend> {
public:
    virtual ~DisplayLinkPlatformBackend() = default;

    virtual WebCore::FramesPerSecond nominalFramesPerSecond() const = 0;
    virtual bool isRunning() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    // The rate the observers need, a divisor of the nominal rate; 0 means the display's native rate.
    // Backends that can't change their rate ignore this.
    virtual void setPreferredFramesPerSecond(double) { }
    // Called on the main thread before the DisplayLink is destroyed; no callback reaches the DisplayLink afterwards.
    virtual void invalidate() = 0;
};

// Returns null unless the WebKitDebugDisplayLinkBackend default is "CoreAnimation".
RefPtr<DisplayLinkPlatformBackend> createCoreAnimationDisplayLinkBackendIfEnabled(DisplayLink&, WebCore::PlatformDisplayID);
// The WebKitDebugDisplayLinkLogStatistics default.
bool displayLinkStatisticsLoggingEnabled();
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

    void NODELETE displayPropertiesChanged();

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
    // Called by DisplayLinkPlatformBackend on its display link thread.
    void platformBackendDidFire(std::optional<DisplayLinkFrameTiming> timing = std::nullopt) { notifyObserversDisplayDidRefresh(timing); }
#endif

private:
    void notifyObserversDisplayDidRefresh(std::optional<DisplayLinkFrameTiming> = std::nullopt);
#if PLATFORM(MAC)
    void updatePlatformPreferredFramesPerSecond() WTF_REQUIRES_LOCK(m_clientsLock);
#endif

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
        // When the platform display link provides frame timing: the time the next update is due, the interval
        // it was computed with, and this client's own update count (so that clients which decimate the
        // DisplayUpdate again, like the WebProcess DisplayRefreshMonitor, agree with the time-based decision).
        std::optional<MonotonicTime> nextUpdateTime;
        Seconds updateInterval;
        unsigned updateIndex { 0 };
    };

#if PLATFORM(MAC)
    RefPtr<DisplayLinkPlatformBackend> m_platformBackend;
    // The divisor of the nominal rate last requested from the backend; 0 until the first request.
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
    WebCore::FramesPerSecond m_displayNominalFramesPerSecond { WebCore::FullSpeedFramesPerSecond };
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
