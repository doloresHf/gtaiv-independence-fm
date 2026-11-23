#pragma once

#include <cstdint>
#include <string>

#include "route_b_ctrl_protocol.h"
#include "route_b_smtc.h"

namespace routeb {

struct RouteBCoordinatorConfig {
    uint32_t gameHeartbeatTimeoutMs = 3000u;
    uint32_t prerollLeadMs          = 9000u;  // 5.29s Block0+1 + 2.65s Burst2 + ~1.06s safety cushion
    uint32_t skipSuppressMs         = 2000u;  // Ignore SMTC track-end events caused by B/N skips
    uint32_t skipAckFallbackMs      = 350u;   // Acknowledge B same-track restart within 350ms
    uint32_t drainTimeoutMs         = 20000u; // Safety cap waiting for Game DJ transition (>16s max hook drain)
};

struct RouteBCoordinatorStatus {
    bool        bridgeEnabled        = false;
    bool        gameOnline           = false;
    bool        observeOnly          = false;
    uint32_t    gameRadioMode        = 0xFFFFFFFFu;
    bool        stationTuned         = false;
    bool        escPaused            = false;
    bool        smtcConnected        = false;
    uint32_t    smtcPlaybackStatus   = kSmtcStatusClosed;
    BridgeState state                = BridgeState::Inactive;
    bool        audioGateMuted       = false;
    bool        prerollActive        = false;
    int32_t     djRemainingMs        = 0;
    uint64_t    prerollFillCount     = 0ULL;
    std::string trackTitleUtf8;
    std::string trackArtistUtf8;
};

class RouteBCoordinator {
public:
    explicit RouteBCoordinator(ISmtcTransport& smtc,
                               const RouteBCoordinatorConfig& cfg = {}) noexcept;

    // Advances the Radio Mode synchronization state machine and publishes
    // Host -> Game fields into 'ctrl' (if non-null).
    RouteBCoordinatorStatus Tick(int64_t nowQpc,
                                 int64_t qpcFreq,
                                 bool appleMusicPresetActive,
                                 bool userEnabled,
                                 uint64_t ringCommitFrame,
                                 RadioBridgeCtrlBlock* ctrl,
                                 uint64_t lastAudibleCommitFrame = 0ULL,
                                 uint64_t runFirstAudibleCommitFrame = 0ULL);

    // Releases any pause holds owned by this coordinator and resets internal state.
    void ResetAndReleaseHolds(RadioBridgeCtrlBlock* ctrl = nullptr);

private:
    void PublishHostFieldsToCtrl(int64_t nowQpc,
                                 bool bridgeEnabled,
                                 const SmtcSnapshot& smtc,
                                 RadioBridgeCtrlBlock* ctrl) const noexcept;

    ISmtcTransport& smtc_;
    RouteBCoordinatorConfig cfg_{};

    BridgeState state_              = BridgeState::Inactive;
    BridgeState preEscState_        = BridgeState::PlayingSong;
    bool eligibleBaselineSynced_    = false;

    bool pausedByUsForEsc_          = false;
    bool pausedByUsForDj_           = false;
    bool audioGateMuted_            = false;
    bool prerollActive_             = false;

    uint32_t lastHandledEndSeq_     = 0u;
    uint32_t lastHandledSkipSeq_    = 0u;
    uint32_t requestTriggerDjSeq_   = 0u;
    uint32_t requestSkipAckSeq_     = 0u;

    uint64_t songEndCommitFrame_    = 0ULL;
    uint64_t prerollStartFrame_     = 0ULL;
    uint64_t skipAudioReadyFrame_   = 0ULL;

    int64_t  skipSuppressUntilQpc_  = 0;
    bool     skipInProgress_        = false;
    uint32_t activeSkipSeq_         = 0u;
    uint32_t skipBaseTrackSeq_      = 0u;
    int64_t  skipStartQpc_          = 0;

    int64_t  drainStartQpc_         = 0;
    uint32_t drainStartSlotSeq_     = 0u;
    uint32_t drainStartUserPlaySeq_ = 0u;
    bool     backToBackPending_     = false;
    uint32_t backToBackStartSlotSeq_ = 0u;

    uint32_t escSpliceSeq_          = 0u;
    uint64_t escSpliceStartFrame_   = 0ULL;
    uint64_t escSpliceEndFrame_     = 0ULL;
    int64_t  escPauseStartQpc_      = 0;
    uint64_t escPauseGapStartFrame_ = 0ULL;
    bool     escResumeAwaitingAudio_ = false;
    int64_t  escResumeStartQpc_     = 0;
    uint64_t escResumeFloorFrame_   = 0ULL;

    bool     prerollAwaitingAudio_  = false;
    uint64_t prerollFloorFrame_     = 0ULL;
    uint64_t skipFloorFrame_        = 0ULL;
};

} // namespace routeb
