#include "route_b_coordinator.h"

#include <algorithm>

namespace routeb {

namespace {

int64_t QpcDeltaToMs(int64_t deltaTicks, int64_t qpcFreq) noexcept {
    if (qpcFreq <= 0 || deltaTicks <= 0) {
        return 0;
    }
    const int64_t sec = deltaTicks / qpcFreq;
    const int64_t rem = deltaTicks % qpcFreq;
    return sec * 1000LL + (rem * 1000LL) / qpcFreq;
}

int64_t MsToQpcTicks(uint32_t ms, int64_t qpcFreq) noexcept {
    if (qpcFreq <= 0 || ms == 0u) {
        return 0;
    }
    const uint64_t uMs = static_cast<uint64_t>(ms);
    const uint64_t uf  = static_cast<uint64_t>(qpcFreq);
    const uint64_t sec = uMs / 1000ULL;
    const uint64_t rem = uMs % 1000ULL;
    return static_cast<int64_t>(sec * uf + (rem * uf) / 1000ULL);
}

} // namespace

RouteBCoordinator::RouteBCoordinator(ISmtcTransport& smtc,
                                     const RouteBCoordinatorConfig& cfg) noexcept
    : smtc_(smtc), cfg_(cfg) {}

void RouteBCoordinator::ResetAndReleaseHolds(RadioBridgeCtrlBlock* ctrl) {
    if (pausedByUsForEsc_ || pausedByUsForDj_) {
        const SmtcSnapshot smtc = smtc_.GetSnapshot();
        if (smtc.connected) {
            smtc_.RequestPlay();
        }
    }
    state_                   = BridgeState::Inactive;
    preEscState_             = BridgeState::PlayingSong;
    eligibleBaselineSynced_  = false;
    pausedByUsForEsc_        = false;
    pausedByUsForDj_         = false;
    audioGateMuted_          = false;
    prerollActive_           = false;
    skipSuppressUntilQpc_    = 0;
    skipInProgress_          = false;
    activeSkipSeq_           = 0u;
    drainStartUserPlaySeq_   = 0u;
    backToBackPending_       = false;
    escSpliceSeq_            = 0u;
    escSpliceStartFrame_     = 0ULL;
    escSpliceEndFrame_       = 0ULL;
    escRawStartQpc_          = 0;
    escPauseStartQpc_        = 0;
    escPauseGapStartFrame_   = 0ULL;
    escResumeAwaitingAudio_  = false;
    escResumeStartQpc_       = 0;
    escResumeFloorFrame_     = 0ULL;
    djPauseStartQpc_         = 0;
    prerollAwaitingAudio_    = false;
    prerollFloorFrame_       = 0ULL;
    skipFloorFrame_          = 0ULL;
    ClearSmtcVerify();

    if (ctrl && AtomicLoad32(&ctrl->magic) == kCtrlMagic && AtomicLoad32(&ctrl->total_bytes) >= kCtrlBytes) {
        AtomicStore32(&ctrl->bridge_enabled, 0u);
        AtomicStore32(&ctrl->bridge_state, static_cast<uint32_t>(BridgeState::Inactive));
        AtomicStore32(&ctrl->preroll_active, 0u);
        AtomicStore32(&ctrl->audio_gate_muted, 0u);
        AtomicStore32(&ctrl->esc_splice_seq, 0u);
        AtomicStoreU64(&ctrl->esc_splice_start_frame, 0ULL);
        AtomicStoreU64(&ctrl->esc_splice_end_frame, 0ULL);
        AtomicStoreI64(&ctrl->host_heartbeat_qpc, 0LL);
        AtomicStore32(&ctrl->host_pid, 0u);
    }
}

RouteBCoordinatorStatus RouteBCoordinator::Tick(int64_t nowQpc,
                                                int64_t qpcFreq,
                                                bool appleMusicPresetActive,
                                                bool userEnabled,
                                                uint64_t ringCommitFrame,
                                                RadioBridgeCtrlBlock* ctrl,
                                                uint64_t lastAudibleCommitFrame,
                                                uint64_t runFirstAudibleCommitFrame) {
    RouteBCoordinatorStatus status{};
    const SmtcSnapshot smtc = smtc_.GetSnapshot();

    const bool bridgeEnabledSetting = userEnabled && appleMusicPresetActive;
    bool gameOnline        = false;
    bool observeOnly       = false;
    uint32_t radioMode     = 0xFFFFFFFFu;
    bool stationTuned      = false;
    bool escPaused         = false;
    bool actActive         = false;
    bool actIsUser         = false;
    int32_t actPlayMs      = 0;
    int32_t actDurMs       = 0;
    bool stbyActive        = false;
    bool stbyIsUser        = false;
    bool stbyPreloadReady  = false;
    uint32_t slotTransSeq  = 0u;
    uint32_t userSlotPlaySeq = 0u;
    bool stationCrossfading = false;
    uint32_t pendingSkipSeq = 0u;
    int32_t pendingSkipDir = 0;
    uint64_t prerollFills  = 0ULL;

    if (ctrl && AtomicLoad32(&ctrl->magic) == kCtrlMagic && AtomicLoad32(&ctrl->total_bytes) >= kCtrlBytes) {
        const uint32_t flags   = AtomicLoad32(&ctrl->flags);
        const uint32_t gamePid = AtomicLoad32(&ctrl->game_pid);
        const int64_t  gameHb  = AtomicLoadI64(&ctrl->game_heartbeat_qpc);
        observeOnly = (flags & kCtrlFlagObserveOnly) != 0u;
        if (gamePid != 0u && gameHb > 0) {
            const int64_t ageMs = (nowQpc > gameHb) ? QpcDeltaToMs(nowQpc - gameHb, qpcFreq) : 0LL;
            gameOnline = (ageMs <= static_cast<int64_t>(cfg_.gameHeartbeatTimeoutMs));
        }
        radioMode          = AtomicLoad32(&ctrl->radio_mode);
        stationTuned       = (AtomicLoad32(&ctrl->station_tuned) != 0u);
        escPaused          = (AtomicLoad32(&ctrl->esc_paused) != 0u);
        actActive          = (AtomicLoad32(&ctrl->active_slot_active) != 0u);
        actIsUser          = (AtomicLoad32(&ctrl->active_slot_is_user) != 0u);
        actPlayMs          = AtomicLoadI32(&ctrl->active_slot_play_ms);
        actDurMs           = AtomicLoadI32(&ctrl->active_slot_duration_ms);
        stbyActive         = (AtomicLoad32(&ctrl->standby_slot_active) != 0u);
        stbyIsUser         = (AtomicLoad32(&ctrl->standby_slot_is_user) != 0u);
        stbyPreloadReady   = (AtomicLoad32(&ctrl->standby_preloaded_ready) != 0u);
        slotTransSeq       = AtomicLoad32(&ctrl->slot_transition_seq);
        userSlotPlaySeq    = AtomicLoad32(&ctrl->user_slot_play_seq);
        stationCrossfading = (AtomicLoad32(&ctrl->station_crossfading) != 0u);
        pendingSkipSeq     = AtomicLoad32(&ctrl->pending_skip_seq);
        pendingSkipDir     = AtomicLoadI32(&ctrl->pending_skip_dir);
        prerollFills       = AtomicLoadU64(&ctrl->preroll_fill_count);
    }

    const bool djCrossfadingIn   = stationCrossfading && stbyActive && !stbyIsUser;
    const bool djSlotOrCrossfade = (actActive && !actIsUser) || djCrossfadingIn;
    const bool hasValidDjDur = (actDurMs > 0);
    const int32_t djRemainingMs = (gameOnline && stationTuned && actActive && !actIsUser && actDurMs > actPlayMs)
        ? (actDurMs - actPlayMs)
        : 0;

    const bool activeEligible = bridgeEnabledSetting && gameOnline && (radioMode == 0u) && !observeOnly;

    if (!activeEligible) {
        if (pausedByUsForEsc_ || pausedByUsForDj_) {
            if (smtc.connected) {
                smtc_.RequestPlay();
            }
            pausedByUsForEsc_ = false;
            pausedByUsForDj_  = false;
        }
        state_                  = BridgeState::Inactive;
        preEscState_            = BridgeState::PlayingSong;
        eligibleBaselineSynced_ = false;
        audioGateMuted_         = false;
        prerollActive_          = false;
        prerollAwaitingAudio_   = false;
        skipInProgress_         = false;
        backToBackPending_      = false;
        escResumeAwaitingAudio_ = false;
        escRawStartQpc_         = 0;
        djPauseStartQpc_        = 0;
        ClearSmtcVerify();
    } else {
        if (!eligibleBaselineSynced_) {
            lastHandledEndSeq_      = smtc.naturalTrackEndSeq;
            lastHandledSkipSeq_     = pendingSkipSeq;
            eligibleBaselineSynced_ = true;
            if (escPaused) {
                state_ = BridgeState::PlayingSong;
            } else if (stationTuned && djSlotOrCrossfade) {
                state_ = BridgeState::PausedForDj;
                audioGateMuted_ = true;
                if (smtc.connected && smtc.playbackStatus == kSmtcStatusPlaying) {
                    smtc_.RequestPauseAndRestartTrack();
                    pausedByUsForDj_ = true;
                    djPauseStartQpc_ = nowQpc;
                    ArmPauseVerify(nowQpc);
                }
            } else {
                state_ = BridgeState::PlayingSong;
            }
        }

        // Priority 1: ESC pause menu handling (assert debounced >= 100ms, de-assert instant)
        bool escConfirmed = false;
        if (escPaused) {
            if (state_ == BridgeState::PausedForEsc) {
                escConfirmed = true;
            } else {
                if (escRawStartQpc_ == 0) {
                    escRawStartQpc_ = nowQpc;
                }
                escConfirmed = (QpcDeltaToMs(nowQpc - escRawStartQpc_, qpcFreq) >=
                                static_cast<int64_t>(cfg_.escPauseDebounceMs));
            }
        } else {
            escRawStartQpc_ = 0;
        }

        if (escConfirmed) {
            if (state_ != BridgeState::PausedForEsc) {
                preEscState_ = state_;
                const bool shouldOwnEscPause = !pausedByUsForDj_ && smtc.connected &&
                    (smtc.playbackStatus == kSmtcStatusPlaying || playVerifyPending_);
                if (shouldOwnEscPause) {
                    if (smtc.playbackStatus == kSmtcStatusPlaying) {
                        smtc_.RequestPause();
                    }
                    pausedByUsForEsc_ = true;
                    ArmPauseVerify(nowQpc);
                    escPauseStartQpc_ = nowQpc;
                    escPauseGapStartFrame_ =
                        (lastAudibleCommitFrame > 0ULL && lastAudibleCommitFrame <= ringCommitFrame)
                        ? lastAudibleCommitFrame
                        : ringCommitFrame;
                    escResumeAwaitingAudio_ = false;
                }
                state_ = BridgeState::PausedForEsc;
            } else if (pausedByUsForEsc_ && QpcDeltaToMs(nowQpc - escPauseStartQpc_, qpcFreq) <= 800LL) {
                if (lastAudibleCommitFrame > escPauseGapStartFrame_ && lastAudibleCommitFrame <= ringCommitFrame) {
                    escPauseGapStartFrame_ = lastAudibleCommitFrame;
                }
            }
            lastHandledEndSeq_  = smtc.naturalTrackEndSeq;
            lastHandledSkipSeq_ = pendingSkipSeq;
        } else {
            if (state_ == BridgeState::PausedForEsc) {
                if (pausedByUsForEsc_) {
                    if (!pausedByUsForDj_ && smtc.connected) {
                        smtc_.RequestPlay();
                        ArmPlayVerify(nowQpc);
                        escResumeAwaitingAudio_ = true;
                        escResumeStartQpc_      = nowQpc;
                        escResumeFloorFrame_    = ringCommitFrame;
                    }
                    pausedByUsForEsc_ = false;
                }
                if (stationTuned && djSlotOrCrossfade) {
                    state_ = prerollActive_ ? BridgeState::PreRollingForDj : BridgeState::PausedForDj;
                } else {
                    state_ = (preEscState_ == BridgeState::DrainingSongEnd)
                        ? BridgeState::DrainingSongEnd
                        : BridgeState::PlayingSong;
                }
            }

            if (escResumeAwaitingAudio_ && !audioGateMuted_) {
                const bool audibleArrived = (lastAudibleCommitFrame > escResumeFloorFrame_);
                const bool timeoutHit     = QpcDeltaToMs(nowQpc - escResumeStartQpc_, qpcFreq) >= 1500LL;
                if (audibleArrived || timeoutHit) {
                    const uint64_t gapEnd =
                        (runFirstAudibleCommitFrame > escResumeFloorFrame_ && runFirstAudibleCommitFrame <= ringCommitFrame)
                        ? runFirstAudibleCommitFrame
                        : ringCommitFrame;
                    if (gapEnd > escPauseGapStartFrame_) {
                        escSpliceStartFrame_ = escPauseGapStartFrame_;
                        escSpliceEndFrame_   = gapEnd;
                        ++escSpliceSeq_;
                    }
                    escResumeAwaitingAudio_ = false;
                }
            }

            // Priority 2: Exit car / switch station MUST NEVER pause AM; release DJ hold if active
            if (!stationTuned) {
                if (pausedByUsForDj_) {
                    if (smtc.connected) {
                        smtc_.RequestPlay();
                        ArmPlayVerify(nowQpc);
                    }
                    pausedByUsForDj_ = false;
                }
                djPauseStartQpc_      = 0;
                audioGateMuted_       = false;
                prerollActive_        = false;
                prerollAwaitingAudio_ = false;
                skipInProgress_       = false;
                backToBackPending_    = false;
                state_                = BridgeState::PlayingSong;
                lastHandledEndSeq_    = smtc.naturalTrackEndSeq;
                lastHandledSkipSeq_   = pendingSkipSeq;
            } else {
                // Priority 3: B / N manual skip input (strictly user track only; never interrupts DJ/Ad/News)
                if (pendingSkipSeq != lastHandledSkipSeq_) {
                    lastHandledSkipSeq_ = pendingSkipSeq;
                    const bool canSkip = (state_ == BridgeState::PlayingSong || state_ == BridgeState::SkippingTrack) &&
                                         actActive && actIsUser && !djCrossfadingIn;
                    if (canSkip) {
                        activeSkipSeq_          = pendingSkipSeq;
                        skipStartQpc_           = nowQpc;
                        skipBaseTrackSeq_       = smtc.trackSeq;
                        skipFloorFrame_         = ringCommitFrame;
                        skipInProgress_         = true;
                        skipSuppressUntilQpc_   = nowQpc + MsToQpcTicks(cfg_.skipSuppressMs, qpcFreq);
                        lastHandledEndSeq_      = smtc.naturalTrackEndSeq;
                        audioGateMuted_         = false;
                        prerollActive_          = false;
                        prerollAwaitingAudio_   = false;
                        backToBackPending_      = false;
                        escResumeAwaitingAudio_ = false;
                        state_                  = BridgeState::SkippingTrack;

                        if (pendingSkipDir >= 0) {
                            smtc_.RequestSkipNext();
                        } else {
                            smtc_.RequestSkipPrevious();
                        }
                    }
                }

                if (skipInProgress_) {
                    lastHandledEndSeq_ = smtc.naturalTrackEndSeq;
                    const bool trackSeqAdvanced = (smtc.trackSeq != skipBaseTrackSeq_);
                    const bool ackTimeoutHit    = QpcDeltaToMs(nowQpc - skipStartQpc_, qpcFreq) >=
                                                  static_cast<int64_t>(cfg_.skipAckFallbackMs);
                    if (trackSeqAdvanced || ackTimeoutHit) {
                        skipAudioReadyFrame_ =
                            (runFirstAudibleCommitFrame > skipFloorFrame_ && runFirstAudibleCommitFrame <= ringCommitFrame)
                            ? runFirstAudibleCommitFrame
                            : ringCommitFrame;
                        requestSkipAckSeq_   = activeSkipSeq_;
                        skipInProgress_      = false;
                        state_               = BridgeState::PlayingSong;
                    }
                } else {
                    // Priority 4: Song <-> DJ / Ad / News State Machine (station_tuned == true)
                    switch (state_) {
                    case BridgeState::PlayingSong: {
                        if (backToBackPending_ && slotTransSeq != backToBackStartSlotSeq_) {
                            prerollActive_     = false;
                            backToBackPending_ = false;
                        }

                        if ((actActive && !actIsUser) || djCrossfadingIn) {
                            // Entered DJ / Commercial / Weazel News (or DJ crossfade window)
                            audioGateMuted_         = true;
                            prerollActive_          = false;
                            prerollAwaitingAudio_   = false;
                            backToBackPending_      = false;
                            escResumeAwaitingAudio_ = false;
                            const bool shouldOwnDjPause = !pausedByUsForDj_ && smtc.connected &&
                                (smtc.playbackStatus == kSmtcStatusPlaying || playVerifyPending_);
                            if (shouldOwnDjPause) {
                                smtc_.RequestPauseAndRestartTrack();
                                pausedByUsForDj_ = true;
                                djPauseStartQpc_ = nowQpc;
                                ArmPauseVerify(nowQpc);
                            }
                            lastHandledEndSeq_ = smtc.naturalTrackEndSeq;
                            state_ = BridgeState::PausedForDj;
                        } else if (smtc.naturalTrackEndSeq != lastHandledEndSeq_) {
                            lastHandledEndSeq_ = smtc.naturalTrackEndSeq;
                            const bool suppressed = (skipSuppressUntilQpc_ > 0 && nowQpc < skipSuppressUntilQpc_);
                            if (!suppressed && actActive && actIsUser) {
                                ++requestTriggerDjSeq_;
                                const uint64_t songBoundaryFrame =
                                    (runFirstAudibleCommitFrame > 0ULL &&
                                     ringCommitFrame >= runFirstAudibleCommitFrame &&
                                     (ringCommitFrame - runFirstAudibleCommitFrame) <= 96000ULL)
                                        ? runFirstAudibleCommitFrame
                                        : ringCommitFrame;
                                songEndCommitFrame_ = songBoundaryFrame;

                                if (stbyActive && stbyIsUser) {
                                    // Two consecutive songs with no DJ between them -> zero pause
                                    prerollFloorFrame_      = songBoundaryFrame;
                                    prerollStartFrame_      = songBoundaryFrame;
                                    prerollAwaitingAudio_   = false;
                                    prerollActive_          = true;
                                    audioGateMuted_         = false;
                                    backToBackPending_      = true;
                                    backToBackStartSlotSeq_ = slotTransSeq;
                                } else {
                                    // Outgoing song -> DJ / Ad / Weazel News (pause and rewind Song 2 to 0:00)
                                    audioGateMuted_         = true;
                                    escResumeAwaitingAudio_ = false;
                                    if (smtc.connected) {
                                        smtc_.RequestPauseAndRewindToStart();
                                        pausedByUsForDj_ = true;
                                        djPauseStartQpc_ = nowQpc;
                                        ArmPauseVerify(nowQpc);
                                    }
                                    drainStartQpc_         = nowQpc;
                                    drainStartSlotSeq_     = slotTransSeq;
                                    drainStartUserPlaySeq_ = userSlotPlaySeq;
                                    state_                 = BridgeState::DrainingSongEnd;
                                }
                            }
                        }
                        break;
                    }

                    case BridgeState::DrainingSongEnd: {
                        lastHandledEndSeq_ = smtc.naturalTrackEndSeq;
                        audioGateMuted_    = true;
                        if ((actActive && !actIsUser) || djCrossfadingIn) {
                            state_ = BridgeState::PausedForDj;
                        } else if (userSlotPlaySeq != drainStartUserPlaySeq_ && actActive && actIsUser && !djCrossfadingIn) {
                            // Game started a new user track directly (ignoring DJ RadioSlot_Play transitions) -> resume AM
                            audioGateMuted_ = false;
                            if (pausedByUsForDj_ && smtc.connected) {
                                smtc_.RequestPlay();
                                ArmPlayVerify(nowQpc);
                            }
                            pausedByUsForDj_ = false;
                            djPauseStartQpc_ = 0;
                            state_           = BridgeState::PlayingSong;
                        } else if (QpcDeltaToMs(nowQpc - drainStartQpc_, qpcFreq) >=
                                   static_cast<int64_t>(cfg_.drainTimeoutMs)) {
                            audioGateMuted_ = false;
                            if (pausedByUsForDj_ && smtc.connected) {
                                smtc_.RequestPlay();
                                ArmPlayVerify(nowQpc);
                            }
                            pausedByUsForDj_ = false;
                            djPauseStartQpc_ = 0;
                            state_           = BridgeState::PlayingSong;
                        }
                        break;
                    }

                    case BridgeState::PausedForDj: {
                        lastHandledEndSeq_ = smtc.naturalTrackEndSeq;
                        audioGateMuted_    = true;
                        if (actActive && actIsUser && !djCrossfadingIn) {
                            audioGateMuted_       = false;
                            prerollActive_        = false;
                            prerollAwaitingAudio_ = false;
                            if (pausedByUsForDj_ && smtc.connected) {
                                smtc_.RequestPlay();
                                ArmPlayVerify(nowQpc);
                            }
                            pausedByUsForDj_ = false;
                            djPauseStartQpc_ = 0;
                            state_           = BridgeState::PlayingSong;
                        } else {
                            const bool djPauseSettled = !pausedByUsForDj_ || (djPauseStartQpc_ == 0) ||
                                (QpcDeltaToMs(nowQpc - djPauseStartQpc_, qpcFreq) >=
                                 static_cast<int64_t>(cfg_.djPauseSettleMs));
                            if (actActive && !actIsUser && hasValidDjDur && djPauseSettled &&
                                stbyActive && stbyIsUser && stbyPreloadReady &&
                                djRemainingMs <= static_cast<int32_t>(cfg_.prerollLeadMs)) {
                                prerollFloorFrame_    = ringCommitFrame;
                                prerollStartFrame_    = ringCommitFrame;
                                prerollAwaitingAudio_ = true;
                                prerollActive_        = true;
                                audioGateMuted_       = false;
                                if (pausedByUsForDj_ && smtc.connected) {
                                    smtc_.RequestPlay();
                                    ArmPlayVerify(nowQpc);
                                }
                                pausedByUsForDj_ = false;
                                djPauseStartQpc_ = 0;
                                state_           = BridgeState::PreRollingForDj;
                            }
                        }
                        break;
                    }

                    case BridgeState::PreRollingForDj: {
                        lastHandledEndSeq_ = smtc.naturalTrackEndSeq;
                        audioGateMuted_    = false;
                        prerollActive_     = true;
                        if (prerollAwaitingAudio_ &&
                            runFirstAudibleCommitFrame > prerollFloorFrame_ &&
                            runFirstAudibleCommitFrame <= ringCommitFrame) {
                            prerollStartFrame_    = runFirstAudibleCommitFrame;
                            prerollAwaitingAudio_ = false;
                        }
                        if (actActive && actIsUser && !djCrossfadingIn) {
                            prerollActive_        = false;
                            prerollAwaitingAudio_ = false;
                            state_                = BridgeState::PlayingSong;
                        } else if (actActive && !actIsUser && stbyActive && !stbyIsUser) {
                            // Another DJ / Commercial / News slot queued ahead of next user song; re-pause AM cleanly.
                            prerollActive_        = false;
                            prerollAwaitingAudio_ = false;
                            audioGateMuted_       = true;
                            const bool shouldOwnDjPause = !pausedByUsForDj_ && smtc.connected &&
                                (smtc.playbackStatus == kSmtcStatusPlaying || playVerifyPending_);
                            if (shouldOwnDjPause) {
                                smtc_.RequestPauseAndRestartTrack();
                                pausedByUsForDj_ = true;
                                djPauseStartQpc_ = nowQpc;
                                ArmPauseVerify(nowQpc);
                            }
                            state_ = BridgeState::PausedForDj;
                        }
                        break;
                    }

                    default:
                        state_ = BridgeState::PlayingSong;
                        break;
                    }
                }
            }
        }

        TickSmtcVerify(nowQpc, qpcFreq, smtc);
    }

    PublishHostFieldsToCtrl(nowQpc, bridgeEnabledSetting, smtc, ctrl);

    status.bridgeEnabled      = bridgeEnabledSetting;
    status.gameOnline         = gameOnline;
    status.observeOnly        = observeOnly;
    status.gameRadioMode      = radioMode;
    status.stationTuned       = stationTuned;
    status.escPaused          = escPaused;
    status.smtcConnected      = smtc.connected;
    status.smtcPlaybackStatus = smtc.playbackStatus;
    status.state              = state_;
    status.audioGateMuted     = audioGateMuted_;
    status.prerollActive      = prerollActive_;
    status.djRemainingMs      = djRemainingMs;
    status.prerollFillCount   = prerollFills;
    status.trackTitleUtf8     = smtc.titleUtf8;
    status.trackArtistUtf8    = smtc.artistUtf8;
    return status;
}

void RouteBCoordinator::PublishHostFieldsToCtrl(int64_t nowQpc,
                                                bool bridgeEnabled,
                                                const SmtcSnapshot& smtc,
                                                RadioBridgeCtrlBlock* ctrl) const noexcept {
    if (!ctrl || AtomicLoad32(&ctrl->magic) != kCtrlMagic || AtomicLoad32(&ctrl->total_bytes) < kCtrlBytes) {
        return;
    }
    AtomicStore32(&ctrl->host_pid, GetCurrentProcessId());
    AtomicStore32(&ctrl->bridge_enabled, bridgeEnabled ? 1u : 0u);
    AtomicStore32(&ctrl->bridge_state, static_cast<uint32_t>(state_));
    AtomicStore32(&ctrl->smtc_connected, smtc.connected ? 1u : 0u);
    AtomicStore32(&ctrl->smtc_playback_status, smtc.playbackStatus);
    AtomicStore32(&ctrl->smtc_track_seq, smtc.trackSeq);
    AtomicStoreI64(&ctrl->smtc_position_ms, smtc.positionMs);
    AtomicStoreI64(&ctrl->smtc_duration_ms, smtc.durationMs);
    AtomicStoreU64(&ctrl->song_end_commit_frame, songEndCommitFrame_);
    AtomicStoreU64(&ctrl->preroll_start_frame, prerollStartFrame_);
    AtomicStoreU64(&ctrl->skip_audio_ready_frame, skipAudioReadyFrame_);
    AtomicStore32(&ctrl->request_trigger_dj_seq, requestTriggerDjSeq_);
    AtomicStore32(&ctrl->request_skip_ack_seq, requestSkipAckSeq_);
    AtomicStore32(&ctrl->preroll_active, prerollActive_ ? 1u : 0u);
    AtomicStore32(&ctrl->audio_gate_muted, audioGateMuted_ ? 1u : 0u);
    AtomicStore32(&ctrl->esc_splice_seq, escSpliceSeq_);
    AtomicStoreU64(&ctrl->esc_splice_start_frame, escSpliceStartFrame_);
    AtomicStoreU64(&ctrl->esc_splice_end_frame, escSpliceEndFrame_);
    AtomicStoreI64(&ctrl->host_heartbeat_qpc, nowQpc);
}

void RouteBCoordinator::ArmPlayVerify(int64_t nowQpc) noexcept {
    pauseVerifyPending_        = false;
    pauseVerifyStartQpc_       = 0;
    pauseVerifyLastAttemptQpc_ = 0;
    pauseVerifyStableSinceQpc_ = 0;
    pauseVerifyRetries_        = 0u;

    playVerifyPending_        = true;
    playVerifyStartQpc_       = nowQpc;
    playVerifyLastAttemptQpc_ = nowQpc;
    playVerifyStableSinceQpc_ = 0;
    playVerifyRetries_        = 0u;
}

void RouteBCoordinator::ArmPauseVerify(int64_t nowQpc) noexcept {
    playVerifyPending_        = false;
    playVerifyStartQpc_       = 0;
    playVerifyLastAttemptQpc_ = 0;
    playVerifyStableSinceQpc_ = 0;
    playVerifyRetries_        = 0u;

    pauseVerifyPending_        = true;
    pauseVerifyStartQpc_       = nowQpc;
    pauseVerifyLastAttemptQpc_ = nowQpc;
    pauseVerifyStableSinceQpc_ = 0;
    pauseVerifyRetries_        = 0u;
}

void RouteBCoordinator::ClearSmtcVerify() noexcept {
    playVerifyPending_         = false;
    playVerifyStartQpc_        = 0;
    playVerifyLastAttemptQpc_  = 0;
    playVerifyStableSinceQpc_  = 0;
    playVerifyRetries_         = 0u;
    pauseVerifyPending_        = false;
    pauseVerifyStartQpc_       = 0;
    pauseVerifyLastAttemptQpc_ = 0;
    pauseVerifyStableSinceQpc_ = 0;
    pauseVerifyRetries_        = 0u;
}

void RouteBCoordinator::TickSmtcVerify(int64_t nowQpc, int64_t qpcFreq, const SmtcSnapshot& smtc) {
    if (playVerifyPending_) {
        const bool validState =
            (state_ == BridgeState::PlayingSong ||
             state_ == BridgeState::PreRollingForDj ||
             state_ == BridgeState::SkippingTrack) &&
            !pausedByUsForEsc_ &&
            !pausedByUsForDj_;
        if (!validState ||
            QpcDeltaToMs(nowQpc - playVerifyStartQpc_, qpcFreq) >= static_cast<int64_t>(cfg_.smtcVerifyTimeoutMs)) {
            playVerifyPending_ = false;
        } else if (smtc.connected && smtc.playbackStatus == kSmtcStatusPlaying) {
            if (playVerifyStableSinceQpc_ == 0) {
                playVerifyStableSinceQpc_ = nowQpc;
            } else if (QpcDeltaToMs(nowQpc - playVerifyStableSinceQpc_, qpcFreq) >=
                       static_cast<int64_t>(cfg_.smtcVerifySettleMs)) {
                playVerifyPending_ = false;
            }
        } else if (smtc.connected) {
            playVerifyStableSinceQpc_ = 0;
            if (playVerifyRetries_ < cfg_.smtcVerifyMaxRetries &&
                QpcDeltaToMs(nowQpc - playVerifyLastAttemptQpc_, qpcFreq) >=
                    static_cast<int64_t>(cfg_.playVerifyRetryMs)) {
                smtc_.RequestPlay();
                playVerifyLastAttemptQpc_ = nowQpc;
                ++playVerifyRetries_;
            }
        }
    }

    if (pauseVerifyPending_) {
        const bool validState =
            (state_ == BridgeState::PausedForEsc ||
             state_ == BridgeState::PausedForDj ||
             state_ == BridgeState::DrainingSongEnd) &&
            (pausedByUsForEsc_ || pausedByUsForDj_);
        if (!validState ||
            QpcDeltaToMs(nowQpc - pauseVerifyStartQpc_, qpcFreq) >= static_cast<int64_t>(cfg_.smtcVerifyTimeoutMs)) {
            pauseVerifyPending_ = false;
        } else if (smtc.connected && smtc.playbackStatus == kSmtcStatusPaused) {
            if (pauseVerifyStableSinceQpc_ == 0) {
                pauseVerifyStableSinceQpc_ = nowQpc;
            } else if (QpcDeltaToMs(nowQpc - pauseVerifyStableSinceQpc_, qpcFreq) >=
                       static_cast<int64_t>(cfg_.smtcVerifySettleMs)) {
                pauseVerifyPending_ = false;
            }
        } else if (smtc.connected) {
            pauseVerifyStableSinceQpc_ = 0;
            if (pauseVerifyRetries_ < cfg_.smtcVerifyMaxRetries &&
                QpcDeltaToMs(nowQpc - pauseVerifyLastAttemptQpc_, qpcFreq) >=
                    static_cast<int64_t>(cfg_.pauseVerifyRetryMs)) {
                smtc_.RequestPause();
                pauseVerifyLastAttemptQpc_ = nowQpc;
                ++pauseVerifyRetries_;
            }
        }
    }
}

} // namespace routeb
