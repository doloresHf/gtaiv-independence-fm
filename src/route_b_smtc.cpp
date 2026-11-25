#include "route_b_smtc.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cwctype>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#pragma warning(push, 3)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#pragma warning(pop)

namespace routeb {

namespace winrt_ctrl = winrt::Windows::Media::Control;

namespace {

std::string WideViewToUtf8(std::wstring_view ws) {
    if (ws.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

bool ContainsCaseInsensitive(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(
        hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    return it != hay.end();
}

int64_t QpcDeltaToMillis(int64_t deltaTicks, int64_t qpcFreq) noexcept {
    if (qpcFreq <= 0 || deltaTicks <= 0) {
        return 0;
    }
    const int64_t sec = deltaTicks / qpcFreq;
    const int64_t rem = deltaTicks % qpcFreq;
    return sec * 1000LL + (rem * 1000LL) / qpcFreq;
}

enum class SmtcCmd : uint8_t {
    Play = 0,
    Pause = 1,
    SkipNext = 2,
    SkipPrev = 3,
    PauseAndRewindToStart = 4,
    PauseAndRestartTrack = 5,
};

} // namespace

class WinRtAppleMusicSmtc final : public ISmtcTransport {
public:
    WinRtAppleMusicSmtc() {
        QueryPerformanceFrequency(&qpcFreq_);
    }

    ~WinRtAppleMusicSmtc() override {
        Stop();
    }

    bool Start() override {
        std::lock_guard<std::mutex> lock(lifecycleMu_);
        if (running_.load(std::memory_order_acquire)) {
            return true;
        }
        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wakeEvent_) {
            return false;
        }
        stopRequested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        worker_ = CreateThread(nullptr, 0, &WinRtAppleMusicSmtc::WorkerThreadProc, this, 0, nullptr);
        if (!worker_) {
            running_.store(false, std::memory_order_release);
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
            return false;
        }
        return true;
    }

    void Stop() noexcept override {
        std::lock_guard<std::mutex> lock(lifecycleMu_);
        stopRequested_.store(true, std::memory_order_release);
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
        if (worker_) {
            WaitForSingleObject(worker_, 4000);
            CloseHandle(worker_);
            worker_ = nullptr;
        }
        running_.store(false, std::memory_order_release);
        if (wakeEvent_) {
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> cmdLock(cmdMu_);
            cmds_.clear();
        }
        {
            std::lock_guard<std::mutex> snapLock(snapMu_);
            snapshot_ = SmtcSnapshot{};
        }
    }

    SmtcSnapshot GetSnapshot() const override {
        std::lock_guard<std::mutex> lock(snapMu_);
        return snapshot_;
    }

    void RequestPlay() override        { EnqueueCmd(SmtcCmd::Play); }
    void RequestPause() override       { EnqueueCmd(SmtcCmd::Pause); }
    void RequestPauseAndRewindToStart() override { EnqueueCmd(SmtcCmd::PauseAndRewindToStart); }
    void RequestPauseAndRestartTrack() override { EnqueueCmd(SmtcCmd::PauseAndRestartTrack); }
    void RequestSkipNext() override    { EnqueueCmd(SmtcCmd::SkipNext); }
    void RequestSkipPrevious() override{ EnqueueCmd(SmtcCmd::SkipPrev); }

private:
    void EnqueueCmd(SmtcCmd cmd) {
        if (!running_.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(cmdMu_);
            if (cmds_.size() >= 16u) {
                cmds_.pop_front();
            }
            cmds_.push_back(cmd);
        }
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
    }

    static DWORD WINAPI WorkerThreadProc(LPVOID param) {
        auto* self = static_cast<WinRtAppleMusicSmtc*>(param);
        self->WorkerMain();
        return 0u;
    }

    void SignalRefresh(bool sessionsDirty, bool mediaDirty) {
        if (sessionsDirty) {
            sessionsDirty_.store(true, std::memory_order_release);
        }
        if (mediaDirty) {
            mediaDirty_.store(true, std::memory_order_release);
        }
        stateDirty_.store(true, std::memory_order_release);
        if (wakeEvent_) {
            SetEvent(wakeEvent_);
        }
    }

    void WorkerMain() {
        bool comInit = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            comInit = true;
        } catch (...) {
            comInit = false;
        }

        if (comInit) {
            try {
                RunMtaLoop();
            } catch (...) {
            }
            try {
                winrt::uninit_apartment();
            } catch (...) {
            }
        }
    }

    void DetachSession(winrt_ctrl::GlobalSystemMediaTransportControlsSession& session,
                       winrt::event_token& tokMedia,
                       winrt::event_token& tokPlayback,
                       winrt::event_token& tokTimeline) noexcept {
        if (!session) {
            return;
        }
        try {
            if (tokMedia.value != 0)    session.MediaPropertiesChanged(tokMedia);
            if (tokPlayback.value != 0) session.PlaybackInfoChanged(tokPlayback);
            if (tokTimeline.value != 0) session.TimelinePropertiesChanged(tokTimeline);
        } catch (...) {
        }
        tokMedia = {};
        tokPlayback = {};
        tokTimeline = {};
        session = nullptr;
    }

    winrt_ctrl::GlobalSystemMediaTransportControlsSession FindAppleMusicSession(
        const winrt_ctrl::GlobalSystemMediaTransportControlsSessionManager& mgr) {
        if (!mgr) {
            return nullptr;
        }
        auto sessions = mgr.GetSessions();
        for (uint32_t i = 0u; i < sessions.Size(); ++i) {
            auto s = sessions.GetAt(i);
            const auto aumid = s.SourceAppUserModelId();
            if (ContainsCaseInsensitive(std::wstring_view(aumid), L"AppleInc.AppleMusicWin")) {
                return s;
            }
        }
        return nullptr;
    }

    void RunMtaLoop() {
        winrt_ctrl::GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
        winrt::event_token tokSessions{};
        winrt_ctrl::GlobalSystemMediaTransportControlsSession session{ nullptr };
        winrt::event_token tokMedia{};
        winrt::event_token tokPlayback{};
        winrt::event_token tokTimeline{};

        bool hasPrevMedia = false;
        std::string prevTitle;
        std::string prevArtist;
        uint32_t prevStatus = kSmtcStatusClosed;
        int64_t prevRawPosMs = 0LL;
        int64_t prevDurMs = 0LL;
        int64_t lastTrackEndQpc = 0;
        int64_t lastTrackChangeQpc = 0;

        auto bindManagerIfNeeded = [&]() {
            if (mgr) {
                return true;
            }
            try {
                mgr = winrt_ctrl::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                if (mgr) {
                    tokSessions = mgr.SessionsChanged([this](auto&&, auto&&) {
                        this->SignalRefresh(true, true);
                    });
                    sessionsDirty_.store(true, std::memory_order_release);
                    return true;
                }
            } catch (...) {
                mgr = nullptr;
            }
            return false;
        };

        auto bindSessionIfNeeded = [&]() {
            if (!mgr) {
                return;
            }
            const bool needRebind = !session || sessionsDirty_.exchange(false, std::memory_order_acq_rel);
            if (!needRebind) {
                return;
            }
            winrt_ctrl::GlobalSystemMediaTransportControlsSession found{ nullptr };
            try {
                found = FindAppleMusicSession(mgr);
            } catch (...) {
                found = nullptr;
            }

            if (found != session) {
                DetachSession(session, tokMedia, tokPlayback, tokTimeline);
                hasPrevMedia = false;
                prevTitle.clear();
                prevArtist.clear();
                prevStatus = kSmtcStatusClosed;
                prevRawPosMs = 0LL;
                prevDurMs = 0LL;
                lastTrackChangeQpc = 0;

                if (found) {
                    session = found;
                    try {
                        tokMedia = session.MediaPropertiesChanged([this](auto&&, auto&&) {
                            this->SignalRefresh(false, true);
                        });
                        tokPlayback = session.PlaybackInfoChanged([this](auto&&, auto&&) {
                            this->SignalRefresh(false, false);
                        });
                        tokTimeline = session.TimelinePropertiesChanged([this](auto&&, auto&&) {
                            this->SignalRefresh(false, false);
                        });
                        mediaDirty_.store(true, std::memory_order_release);
                        stateDirty_.store(true, std::memory_order_release);
                    } catch (...) {
                        DetachSession(session, tokMedia, tokPlayback, tokTimeline);
                    }
                }
            }
        };

        auto refreshSnapshot = [&]() {
            LARGE_INTEGER nowLi{};
            QueryPerformanceCounter(&nowLi);
            const int64_t nowQpc = nowLi.QuadPart;

            if (!session) {
                std::lock_guard<std::mutex> lock(snapMu_);
                const uint32_t keepTrackSeq = snapshot_.trackSeq;
                const uint32_t keepEndSeq   = snapshot_.naturalTrackEndSeq;
                snapshot_ = SmtcSnapshot{};
                snapshot_.trackSeq = keepTrackSeq;
                snapshot_.naturalTrackEndSeq = keepEndSeq;
                snapshot_.lastUpdateQpc = nowQpc;
                return;
            }

            try {
                auto pb = session.GetPlaybackInfo();
                const uint32_t curStatus = static_cast<uint32_t>(pb.PlaybackStatus());

                auto tl = session.GetTimelineProperties();
                const int64_t endMs = tl.EndTime().count() / 10000LL;
                const int64_t rawPosMs = tl.Position().count() / 10000LL;
                int64_t effectivePosMs = rawPosMs;
                const int64_t tlUpdatedTicks = tl.LastUpdatedTime().time_since_epoch().count();
                if (curStatus == kSmtcStatusPlaying && tlUpdatedTicks > 0) {
                    const int64_t ageMs = (winrt::clock::now().time_since_epoch().count() - tlUpdatedTicks) / 10000LL;
                    if (ageMs > 0) {
                        effectivePosMs = rawPosMs + ageMs;
                        if (endMs > 0 && effectivePosMs > endMs) {
                            effectivePosMs = endMs;
                        }
                    }
                }

                std::string curTitle;
                std::string curArtist;
                {
                    std::lock_guard<std::mutex> lock(snapMu_);
                    curTitle  = snapshot_.titleUtf8;
                    curArtist = snapshot_.artistUtf8;
                }

                if (mediaDirty_.exchange(false, std::memory_order_acq_rel) || !hasPrevMedia) {
                    try {
                        auto mp = session.TryGetMediaPropertiesAsync().get();
                        if (mp) {
                            curTitle  = WideViewToUtf8(std::wstring_view(mp.Title()));
                            curArtist = WideViewToUtf8(std::wstring_view(mp.Artist()));
                        }
                    } catch (...) {
                    }
                }

                bool trackChanged = false;
                bool naturalTrackEnd = false;

                if (!curTitle.empty()) {
                    if (hasPrevMedia && (curTitle != prevTitle || curArtist != prevArtist)) {
                        trackChanged = true;
                        lastTrackChangeQpc = nowQpc;
                        if (prevStatus == kSmtcStatusPlaying || curStatus == kSmtcStatusPlaying) {
                            naturalTrackEnd = true;
                        }
                    }
                    hasPrevMedia = true;
                    prevTitle    = curTitle;
                    prevArtist   = curArtist;
                }

                // Signal 2: same-track timeline wrap (handles Repeat-One / identically titled songs)
                if (prevDurMs >= 10000LL &&
                    prevRawPosMs >= (prevDurMs - 5000LL) &&
                    rawPosMs <= 2000LL &&
                    curStatus == kSmtcStatusPlaying) {
                    naturalTrackEnd = true;
                }

                if (naturalTrackEnd) {
                    const int64_t sinceLastEndMs = (lastTrackEndQpc > 0)
                        ? QpcDeltaToMillis(nowQpc - lastTrackEndQpc, qpcFreq_.QuadPart)
                        : 10000LL;
                    if (sinceLastEndMs < 500LL) {
                        naturalTrackEnd = false;
                    } else {
                        lastTrackEndQpc = nowQpc;
                    }
                }

                prevStatus   = curStatus;
                prevRawPosMs = rawPosMs;
                prevDurMs    = endMs;

                {
                    std::lock_guard<std::mutex> lock(snapMu_);
                    snapshot_.connected      = true;
                    snapshot_.playbackStatus = curStatus;
                    snapshot_.positionMs     = effectivePosMs;
                    snapshot_.durationMs     = endMs;
                    snapshot_.lastUpdateQpc  = nowQpc;
                    snapshot_.titleUtf8      = curTitle;
                    snapshot_.artistUtf8     = curArtist;
                    if (trackChanged) {
                        ++snapshot_.trackSeq;
                    }
                    if (naturalTrackEnd) {
                        ++snapshot_.naturalTrackEndSeq;
                    }
                }
            } catch (...) {
                DetachSession(session, tokMedia, tokPlayback, tokTimeline);
                sessionsDirty_.store(true, std::memory_order_release);
            }
        };

        auto drainCommands = [&]() {
            std::deque<SmtcCmd> local;
            {
                std::lock_guard<std::mutex> lock(cmdMu_);
                local.swap(cmds_);
            }
            for (SmtcCmd cmd : local) {
                if (stopRequested_.load(std::memory_order_acquire) || !session) {
                    continue;
                }
                try {
                    switch (cmd) {
                    case SmtcCmd::Play:
                        session.TryPlayAsync().get();
                        break;
                    case SmtcCmd::Pause:
                        session.TryPauseAsync().get();
                        break;
                    case SmtcCmd::PauseAndRewindToStart:
                        session.TryPauseAsync().get();
                        try {
                            auto pb = session.GetPlaybackInfo();
                            if (pb && pb.Controls().IsPlaybackPositionEnabled()) {
                                session.TryChangePlaybackPositionAsync(0LL).get();
                            }
                        } catch (...) {
                        }
                        break;
                    case SmtcCmd::PauseAndRestartTrack: {
                        LARGE_INTEGER nowLi{};
                        QueryPerformanceCounter(&nowLi);
                        const int64_t nowQpc = nowLi.QuadPart;
                        auto pb = session.GetPlaybackInfo();
                        auto tl = session.GetTimelineProperties();
                        const int64_t rawPosMs = tl.Position().count() / 10000LL;
                        if (pb && pb.Controls().IsPlaybackPositionEnabled()) {
                            session.TryPauseAsync().get();
                            try {
                                session.TryChangePlaybackPositionAsync(0LL).get();
                            } catch (...) {
                            }
                            prevRawPosMs = 0LL;
                            break;
                        }
                        const bool canSkipPrev = (pb && pb.Controls().IsPreviousEnabled());
                        const bool trackStable = (lastTrackChangeQpc == 0) ||
                            (QpcDeltaToMillis(nowQpc - lastTrackChangeQpc, qpcFreq_.QuadPart) >= 4000LL);
                        if (canSkipPrev && trackStable && rawPosMs >= 5000LL) {
                            session.TrySkipPreviousAsync().get();
                            for (int i = 0; i < 6; ++i) {
                                if (stopRequested_.load(std::memory_order_acquire)) {
                                    break;
                                }
                                Sleep(25u);
                                try {
                                    const auto tlNow = session.GetTimelineProperties();
                                    const int64_t posNowMs = tlNow.Position().count() / 10000LL;
                                    if (posNowMs <= 2500LL) {
                                        break;
                                    }
                                } catch (...) {
                                }
                            }
                            prevRawPosMs = 0LL;
                        }
                        session.TryPauseAsync().get();
                        break;
                    }
                    case SmtcCmd::SkipNext: {
                        session.TrySkipNextAsync().get();
                        LARGE_INTEGER nowLi{};
                        QueryPerformanceCounter(&nowLi);
                        lastTrackChangeQpc = nowLi.QuadPart;
                        mediaDirty_.store(true, std::memory_order_release);
                        break;
                    }
                    case SmtcCmd::SkipPrev: {
                        session.TrySkipPreviousAsync().get();
                        LARGE_INTEGER nowLi{};
                        QueryPerformanceCounter(&nowLi);
                        lastTrackChangeQpc = nowLi.QuadPart;
                        mediaDirty_.store(true, std::memory_order_release);
                        break;
                    }
                    }
                } catch (...) {
                    sessionsDirty_.store(true, std::memory_order_release);
                }
                stateDirty_.store(true, std::memory_order_release);
            }
        };

        while (!stopRequested_.load(std::memory_order_acquire)) {
            if (bindManagerIfNeeded()) {
                bindSessionIfNeeded();
                drainCommands();
                stateDirty_.store(false, std::memory_order_release);
                refreshSnapshot();
            } else {
                refreshSnapshot();
            }

            if (stopRequested_.load(std::memory_order_acquire)) {
                break;
            }
            // Wait up to 400ms (or wake immediately on SMTC event / Skip / Play / Pause / Stop)
            WaitForSingleObject(wakeEvent_, session ? 400u : 750u);
        }

        DetachSession(session, tokMedia, tokPlayback, tokTimeline);
        if (mgr && tokSessions.value != 0) {
            try {
                mgr.SessionsChanged(tokSessions);
            } catch (...) {
            }
            tokSessions = {};
        }
        mgr = nullptr;
    }

    mutable std::mutex lifecycleMu_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> sessionsDirty_{ false };
    std::atomic<bool> mediaDirty_{ false };
    std::atomic<bool> stateDirty_{ false };
    HANDLE wakeEvent_ = nullptr;
    HANDLE worker_ = nullptr;

    mutable std::mutex cmdMu_;
    std::deque<SmtcCmd> cmds_;

    mutable std::mutex snapMu_;
    SmtcSnapshot snapshot_{};
    LARGE_INTEGER qpcFreq_{};
};

// Factory function implemented cleanly inside route_b_smtc.cpp so callers need zero WinRT headers
std::unique_ptr<ISmtcTransport> CreateWinRtAppleMusicSmtc() {
    return std::make_unique<WinRtAppleMusicSmtc>();
}

} // namespace routeb
