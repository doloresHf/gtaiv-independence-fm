#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace routeb {

// WinRT GlobalSystemMediaTransportControlsSessionPlaybackStatus values
inline constexpr uint32_t kSmtcStatusClosed   = 0u;
inline constexpr uint32_t kSmtcStatusOpened   = 1u;
inline constexpr uint32_t kSmtcStatusChanging = 2u;
inline constexpr uint32_t kSmtcStatusStopped  = 3u;
inline constexpr uint32_t kSmtcStatusPlaying  = 4u;
inline constexpr uint32_t kSmtcStatusPaused   = 5u;

struct SmtcSnapshot {
    bool connected               = false;
    uint32_t playbackStatus      = kSmtcStatusClosed;
    uint32_t trackSeq            = 0u;
    uint32_t naturalTrackEndSeq  = 0u;
    int64_t positionMs           = 0LL;
    int64_t durationMs           = 0LL;
    int64_t lastUpdateQpc        = 0LL;
    std::string titleUtf8;
    std::string artistUtf8;
};

class ISmtcTransport {
public:
    virtual ~ISmtcTransport() = default;

    virtual bool Start() = 0;
    virtual void Stop() noexcept = 0;
    virtual SmtcSnapshot GetSnapshot() const = 0;

    virtual void RequestPlay() = 0;
    virtual void RequestPause() = 0;
    // Natural track-end (DrainingSongEnd): pause and seek to 0:00 via TryChangePlaybackPositionAsync
    // only if supported; never calls TrySkipPreviousAsync().
    virtual void RequestPauseAndRewindToStart() = 0;
    // Mid-song entry into PausedForDj: seek to 0:00 if supported, otherwise restart current track
    // to 0:00 via TrySkipPreviousAsync() when safe (>= 5s pos, >= 4s since track change) before pausing.
    virtual void RequestPauseAndRestartTrack() = 0;
    virtual void RequestSkipNext() = 0;
    virtual void RequestSkipPrevious() = 0;
};

// Implemented by WinRT SMTC worker library (x64); callers require zero WinRT headers.
std::unique_ptr<ISmtcTransport> CreateWinRtAppleMusicSmtc();

} // namespace routeb
