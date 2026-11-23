#include "route_b_ipc_consumer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace routeb {

namespace {

int64_t QpcToMicros(int64_t qpcTicks, int64_t qpcFreq) noexcept {
    if (qpcFreq <= 0 || qpcTicks <= 0) {
        return 0;
    }
    const int64_t sec = qpcTicks / qpcFreq;
    const int64_t rem = qpcTicks % qpcFreq;
    return sec * 1000000LL + (rem * 1000000LL) / qpcFreq;
}

int64_t QpcTicksToMillis(int64_t ticks, int64_t qpcFreq) noexcept {
    if (qpcFreq <= 0 || ticks <= 0) {
        return 0;
    }
    const int64_t sec = ticks / qpcFreq;
    const int64_t rem = ticks % qpcFreq;
    return sec * 1000LL + (rem * 1000LL) / qpcFreq;
}

} // namespace

void ZeroPlanarBlock(void* blockPtr, uint32_t blockBytes) noexcept {
    if (blockPtr && blockBytes > 0u) {
        std::memset(blockPtr, 0, blockBytes);
    }
}

void DeinterleaveStereoToPlanar(const int16_t* interleavedLR,
                                uint32_t frames,
                                int16_t* outL,
                                int16_t* outR) noexcept {
    if (!interleavedLR || !outL || !outR || frames == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < frames; ++i) {
        outL[i] = interleavedLR[static_cast<size_t>(i) * 2u];
        outR[i] = interleavedLR[static_cast<size_t>(i) * 2u + 1u];
    }
}

bool WriteInterleavedToPlanarBlock(void* blockPtr,
                                   uint32_t blockBytes,
                                   const int16_t* interleavedLR,
                                   uint32_t frames) noexcept {
    if (!blockPtr || !interleavedLR) {
        return false;
    }
    const uint32_t maxFrames = ComputeBlockFrameCount(blockBytes);
    if (frames > maxFrames) {
        return false;
    }
    const uint32_t halfBytes = blockBytes >> 1;
    auto* baseBytes = static_cast<uint8_t*>(blockPtr);
    auto* outL = reinterpret_cast<int16_t*>(baseBytes);
    auto* outR = reinterpret_cast<int16_t*>(baseBytes + halfBytes);

    DeinterleaveStereoToPlanar(interleavedLR, frames, outL, outR);

    const uint32_t writtenPlaneBytes = frames * sizeof(int16_t);
    if (halfBytes > writtenPlaneBytes) {
        std::memset(baseBytes + writtenPlaneBytes, 0, halfBytes - writtenPlaneBytes);
    }
    const uint32_t rightEndOffset = halfBytes + writtenPlaneBytes;
    if (blockBytes > rightEndOffset) {
        std::memset(baseBytes + rightEndOffset, 0, blockBytes - rightEndOffset);
    }
    return true;
}

RouteBIpcConsumer::RouteBIpcConsumer() {
    InitializeCriticalSection(&cs_);
    QueryPerformanceFrequency(&localQpcFreq_);
    // Pre-reserve standard 524,288 B wave block capacity (131,072 stereo frames) to avoid hot-path allocations
    scratchInterleaved_.reserve(131072u * 2u);
}

RouteBIpcConsumer::~RouteBIpcConsumer() {
    Shutdown();
    DeleteCriticalSection(&cs_);
}

void RouteBIpcConsumer::Configure(const RouteBConsumerConfig& cfg) {
    EnterCriticalSection(&cs_);
    if (!externalBlock_ && shared_ && cfg_.mappingName != cfg.mappingName) {
        CloseMappingLocked();
        attachedGeneration_ = 0ULL;
        disconnectedLatched_ = false;
        cursorAnchored_ = false;
        liveReadFrame_ = 0ULL;
        hasPendingSkip_ = false;
        pendingSkipTargetFrame_ = 0ULL;
        pendingGaps_.clear();
        ClearSavedTailLocked();
        ClearSongEndBarrierLocked();
        fadeInRemaining_ = 0u;
        fadeInSpanFrames_ = 0u;
        lastOutputL_ = 0;
        lastOutputR_ = 0;
        hadNonZeroAudio_ = false;
    }
    cfg_ = cfg;
    if (cfg_.mappingName.empty()) {
        cfg_.mappingName = stagec::kDefaultMappingName;
    }
    fadeTotalFrames_ = (cfg_.fadeMs * stagec::kSampleRate) / 1000u;
    lastOpenAttemptUs_ = 0;
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::Shutdown() {
    EnterCriticalSection(&cs_);
    activeReaderCount_ = 0u;
    if (shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicStore32(&shared_->header.consumer_active_readers, 0u);
    }
    CloseMappingLocked();
    lastOpenAttemptUs_ = 0;
    attachedGeneration_ = 0ULL;
    disconnectedLatched_ = false;
    cursorAnchored_ = false;
    liveReadFrame_ = 0ULL;
    hasPendingSkip_ = false;
    pendingSkipTargetFrame_ = 0ULL;
    pendingGaps_.clear();
    ClearSavedTailLocked();
    ClearSongEndBarrierLocked();
    fadeTotalFrames_ = (cfg_.fadeMs * stagec::kSampleRate) / 1000u;
    fadeInSpanFrames_ = fadeTotalFrames_;
    fadeInRemaining_ = 0u;
    lastOutputL_ = 0;
    lastOutputR_ = 0;
    hadNonZeroAudio_ = false;
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::AttachExternalBlockForTest(stagec::StageCIpcSharedBlock* externalBlock,
                                                     size_t mappedBytes) {
    EnterCriticalSection(&cs_);
    CloseMappingLocked();
    shared_ = externalBlock;
    mappedBytes_ = externalBlock ? mappedBytes : 0u;
    externalBlock_ = (externalBlock != nullptr);
    attachedGeneration_ = 0ULL;
    disconnectedLatched_ = false;
    cursorAnchored_ = false;
    liveReadFrame_ = 0ULL;
    hasPendingSkip_ = false;
    pendingSkipTargetFrame_ = 0ULL;
    pendingGaps_.clear();
    ClearSavedTailLocked();
    ClearSongEndBarrierLocked();
    SetFadeInLocked(fadeTotalFrames_);
    lastOutputL_ = 0;
    lastOutputR_ = 0;
    hadNonZeroAudio_ = false;
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::ResetCursor() {
    EnterCriticalSection(&cs_);
    cursorAnchored_ = false;
    liveReadFrame_ = 0ULL;
    hasPendingSkip_ = false;
    pendingSkipTargetFrame_ = 0ULL;
    pendingGaps_.clear();
    ClearSavedTailLocked();
    ClearSongEndBarrierLocked();
    SetFadeInLocked(fadeTotalFrames_);
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::JumpReadCursorToFrame(uint64_t targetFrame) {
    EnterCriticalSection(&cs_);
    pendingSkipTargetFrame_ = targetFrame;
    hasPendingSkip_ = true;
    ClearSavedTailLocked();
    ClearSongEndBarrierLocked();
    while (!pendingGaps_.empty() && pendingGaps_.front().endFrame <= targetFrame) {
        pendingGaps_.pop_front();
    }
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::QueueSkipGap(uint64_t gapStartFrame, uint64_t gapEndFrame) {
    if (gapEndFrame <= gapStartFrame) {
        return;
    }
    EnterCriticalSection(&cs_);
    if (!pendingGaps_.empty() && gapStartFrame <= pendingGaps_.back().endFrame) {
        if (gapStartFrame < pendingGaps_.back().startFrame) {
            pendingGaps_.back().startFrame = gapStartFrame;
        }
        if (gapEndFrame > pendingGaps_.back().endFrame) {
            pendingGaps_.back().endFrame = gapEndFrame;
        }
    } else {
        if (pendingGaps_.size() >= 8u) {
            pendingGaps_.pop_front();
        }
        pendingGaps_.push_back(SkipGapRange{ gapStartFrame, gapEndFrame });
    }
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::SetActiveReaderCount(uint32_t count) {
    EnterCriticalSection(&cs_);
    activeReaderCount_ = count;
    if (shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicStore32(&shared_->header.consumer_active_readers, count);
    }
    LeaveCriticalSection(&cs_);
}

bool RouteBIpcConsumer::PreserveUnreadTailIfApproachingLap() {
    EnterCriticalSection(&cs_);
    if (!shared_ ||
        mappedBytes_ < stagec::kTotalSharedBytes ||
        !cursorAnchored_ ||
        hasPendingSkip_ ||
        attachedGeneration_ == 0ULL) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    if (IsFrameInSavedTailLocked(liveReadFrame_, attachedGeneration_)) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    const stagec::StageCIpcRingHeader* hdr = &shared_->header;
    const uint64_t gen1 = stagec::AtomicLoadU64(&hdr->session_generation);
    if (gen1 != attachedGeneration_ || !stagec::IsReadyEvenGeneration(gen1)) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    const uint64_t reserveBefore = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
    const uint64_t commitBefore  = stagec::AtomicLoadU64(&hdr->write_commit_frame);
    if (reserveBefore <= liveReadFrame_ || commitBefore <= liveReadFrame_) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    const uint64_t lag = reserveBefore - liveReadFrame_;
    if (lag < kSavedTailTriggerLagFrames || lag > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    const uint64_t avail = commitBefore - liveReadFrame_;
    const uint32_t copyFrames = static_cast<uint32_t>(std::min<uint64_t>(avail, kMaxSavedTailFrames));
    if (copyFrames == 0u) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    std::vector<int16_t> temp(static_cast<size_t>(copyFrames) * 2u);
    CopyRingRangeInterleavedLocked(liveReadFrame_, copyFrames, temp.data());

    const uint64_t reserveAfter = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
    const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
    if (gen2 != gen1 ||
        (reserveAfter > liveReadFrame_ && (reserveAfter - liveReadFrame_) > stagec::kRingCapacityFrames)) {
        LeaveCriticalSection(&cs_);
        return false;
    }

    savedTailInterleaved_ = std::move(temp);
    savedTailStartFrame_  = liveReadFrame_;
    savedTailEndFrame_    = liveReadFrame_ + copyFrames;
    savedTailGeneration_  = gen1;
    LeaveCriticalSection(&cs_);
    return true;
}

void RouteBIpcConsumer::SetSongEndBarrier(uint64_t barrierFrame) {
    EnterCriticalSection(&cs_);
    if (barrierFrame == 0ULL) {
        ClearSongEndBarrierLocked();
    } else {
        hasSongEndBarrier_   = true;
        songEndBarrierFrame_ = barrierFrame;
    }
    LeaveCriticalSection(&cs_);
}

void RouteBIpcConsumer::ClearSongEndBarrier() {
    EnterCriticalSection(&cs_);
    ClearSongEndBarrierLocked();
    LeaveCriticalSection(&cs_);
}

uint64_t RouteBIpcConsumer::GetLiveReadFrame() const {
    EnterCriticalSection(&cs_);
    const uint64_t frame = liveReadFrame_;
    LeaveCriticalSection(&cs_);
    return frame;
}

bool RouteBIpcConsumer::ReadSharedHeaderSnapshot(stagec::StageCIpcRingHeader& outHeader) const {
    std::memset(&outHeader, 0, sizeof(outHeader));
    EnterCriticalSection(&cs_);
    if (!shared_ || mappedBytes_ < stagec::kTotalSharedBytes) {
        LeaveCriticalSection(&cs_);
        return false;
    }
    const stagec::StageCIpcRingHeader* h = &shared_->header;
    outHeader.magic                = stagec::AtomicLoad32(&h->magic);
    outHeader.version              = stagec::AtomicLoad32(&h->version);
    outHeader.header_bytes         = stagec::AtomicLoad32(&h->header_bytes);
    outHeader.total_bytes          = stagec::AtomicLoad32(&h->total_bytes);
    outHeader.sample_rate          = stagec::AtomicLoad32(&h->sample_rate);
    outHeader.channels             = stagec::AtomicLoad32(&h->channels);
    outHeader.bits_per_sample      = stagec::AtomicLoad32(&h->bits_per_sample);
    outHeader.capacity_frames      = stagec::AtomicLoad32(&h->capacity_frames);
    outHeader.session_generation   = stagec::AtomicLoadU64(&h->session_generation);
    outHeader.session_id           = stagec::AtomicLoadU64(&h->session_id);
    outHeader.qpc_freq             = stagec::AtomicLoadU64(&h->qpc_freq);
    outHeader.producer_pid         = stagec::AtomicLoad32(&h->producer_pid);
    outHeader.producer_arch        = stagec::AtomicLoad32(&h->producer_arch);
    outHeader.write_reserve_frame  = stagec::AtomicLoadU64(&h->write_reserve_frame);
    outHeader.write_commit_frame   = stagec::AtomicLoadU64(&h->write_commit_frame);
    outHeader.last_heartbeat_qpc   = stagec::AtomicLoadI64(&h->last_heartbeat_qpc);
    outHeader.start_qpc            = stagec::AtomicLoadI64(&h->start_qpc);
    outHeader.producer_tick_count  = stagec::AtomicLoadU64(&h->producer_tick_count);
    outHeader.producer_state       = stagec::AtomicLoad32(&h->producer_state);
    outHeader.pattern_mode         = stagec::AtomicLoad32(&h->pattern_mode);
    outHeader.consumer_read_frame  = stagec::AtomicLoadU64(&h->consumer_read_frame);
    outHeader.consumer_heartbeat_qpc = stagec::AtomicLoadI64(&h->consumer_heartbeat_qpc);
    outHeader.watermark_frames     = stagec::AtomicLoadU64(&h->watermark_frames);
    outHeader.underrun_events      = stagec::AtomicLoadU64(&h->underrun_events);
    outHeader.underrun_frames      = stagec::AtomicLoadU64(&h->underrun_frames);
    outHeader.overrun_events       = stagec::AtomicLoadU64(&h->overrun_events);
    outHeader.overrun_skipped_frames = stagec::AtomicLoadU64(&h->overrun_skipped_frames);
    outHeader.consumer_pid         = stagec::AtomicLoad32(&h->consumer_pid);
    outHeader.consumer_active_readers = stagec::AtomicLoad32(&h->consumer_active_readers);
    outHeader.overwrite_conflict_events = stagec::AtomicLoadU64(&h->overwrite_conflict_events);
    outHeader.undecided_silence_frames = stagec::AtomicLoadU64(&h->undecided_silence_frames);
    outHeader.preload_resync_events = stagec::AtomicLoadU64(&h->preload_resync_events);
    outHeader.preload_skipped_frames = stagec::AtomicLoadU64(&h->preload_skipped_frames);
    outHeader.silence_disconnect_frames = stagec::AtomicLoadU64(&h->silence_disconnect_frames);
    outHeader.session_attach_count = stagec::AtomicLoadU64(&h->session_attach_count);
    outHeader.last_frame_age_us    = stagec::AtomicLoadU64(&h->last_frame_age_us);
    LeaveCriticalSection(&cs_);
    return true;
}

RouteBPullStatus RouteBIpcConsumer::FillSteadyPlanarBlock(void* blockPtr,
                                                          uint32_t blockBytes,
                                                          uint64_t* outEndFrame) {
    RouteBPullStatus res{};
    const uint32_t framesPerBlock = ComputeBlockFrameCount(blockBytes);
    res.framesPerBlock = framesPerBlock;
    if (!blockPtr || framesPerBlock == 0u) {
        return res;
    }

    EnterCriticalSection(&cs_);

    LARGE_INTEGER nowQpcLi{};
    QueryPerformanceCounter(&nowQpcLi);
    const int64_t nowQpc = nowQpcLi.QuadPart;
    const int64_t nowUs  = QpcToMicros(nowQpc, localQpcFreq_.QuadPart);

    uint64_t liveGen = 0ULL;
    const bool producerLive = RefreshProducerAttachmentLocked(nowQpc, nowUs, liveGen);
    if (!producerLive) {
        EmitSilencePlanarLocked(blockPtr, blockBytes, true);
        res.ok = true;
        res.producerOnline = false;
        res.sessionGeneration = attachedGeneration_;
        res.silenceFramesDelivered = framesPerBlock;
        res.readStartFrame = liveReadFrame_;
        res.readEndFrame = liveReadFrame_;
        if (outEndFrame) {
            *outEndFrame = liveReadFrame_;
        }
        LeaveCriticalSection(&cs_);
        return res;
    }

    stagec::StageCIpcRingHeader* hdr = &shared_->header;
    const uint64_t gen1 = attachedGeneration_;
    const uint64_t steadyLeadFrames = (static_cast<uint64_t>(cfg_.steadyLeadMs) * stagec::kSampleRate) / 1000ULL;

    scratchInterleaved_.resize(static_cast<size_t>(framesPerBlock) * 2u);

    // Primed B/N track skip (or lapped ESC gap fast-forward): hold clean silence until > 1.75s buffer is committed
    if (hasPendingSkip_) {
        if (FillPrimedSkipTargetLocked(blockPtr, blockBytes, nowQpc, gen1, steadyLeadFrames, res, outEndFrame)) {
            LeaveCriticalSection(&cs_);
            return res;
        }
    }

    uint64_t commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
    const uint64_t reserveCurrent = stagec::AtomicLoadU64(&hdr->write_reserve_frame);

    if (!cursorAnchored_) {
        liveReadFrame_ = (commitBefore >= steadyLeadFrames) ? (commitBefore - steadyLeadFrames) : 0ULL;
        cursorAnchored_ = true;
        SetFadeInLocked(fadeTotalFrames_);
    }

    if (hasSongEndBarrier_ && liveReadFrame_ >= songEndBarrierFrame_) {
        EmitSilencePlanarLocked(blockPtr, blockBytes, false);
        UpdateConsumerTelemetryLocked(nowQpc, commitBefore);
        res.ok = true;
        res.producerOnline = true;
        res.sessionGeneration = gen1;
        res.silenceFramesDelivered = framesPerBlock;
        res.readStartFrame = liveReadFrame_;
        res.readEndFrame = liveReadFrame_;
        res.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
        if (outEndFrame) {
            *outEndFrame = liveReadFrame_;
        }
        LeaveCriticalSection(&cs_);
        return res;
    }

    // Fast-forward across queued ESC silence holes whose start is already behind / lapped by writer
    while (!pendingGaps_.empty() &&
           liveReadFrame_ >= pendingGaps_.front().endFrame &&
           (!hasSongEndBarrier_ || pendingGaps_.front().endFrame <= songEndBarrierFrame_)) {
        pendingGaps_.pop_front();
    }
    if (!pendingGaps_.empty() &&
        liveReadFrame_ >= pendingGaps_.front().startFrame &&
        liveReadFrame_ < pendingGaps_.front().endFrame &&
        (!hasSongEndBarrier_ || pendingGaps_.front().startFrame < songEndBarrierFrame_)) {
        if (hasSongEndBarrier_ && pendingGaps_.front().endFrame >= songEndBarrierFrame_) {
            liveReadFrame_ = songEndBarrierFrame_;
            ClearSavedTailLocked();
            EmitSilencePlanarLocked(blockPtr, blockBytes, false);
            UpdateConsumerTelemetryLocked(nowQpc, commitBefore);
            res.ok = true;
            res.producerOnline = true;
            res.sessionGeneration = gen1;
            res.silenceFramesDelivered = framesPerBlock;
            res.readStartFrame = liveReadFrame_;
            res.readEndFrame = liveReadFrame_;
            res.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
            if (outEndFrame) {
                *outEndFrame = liveReadFrame_;
            }
            LeaveCriticalSection(&cs_);
            return res;
        }
        liveReadFrame_ = pendingGaps_.front().endFrame;
        pendingGaps_.pop_front();
        if (savedTailGeneration_ != 0ULL &&
            (liveReadFrame_ >= savedTailEndFrame_ || liveReadFrame_ < savedTailStartFrame_)) {
            ClearSavedTailLocked();
        }
        if (fadeInRemaining_ == 0u) {
            SetFadeInLocked(std::min<uint32_t>(240u, fadeTotalFrames_));
        }
    }
    {
        bool fastForwarded = false;
        uint64_t candidateFrame = liveReadFrame_;
        while (!pendingGaps_.empty()) {
            const SkipGapRange& g = pendingGaps_.front();
            if (hasSongEndBarrier_ && g.startFrame >= songEndBarrierFrame_) {
                break;
            }
            if (IsRangeCoveredBySavedTailLocked(candidateFrame, g.startFrame, gen1)) {
                break;
            }
            const bool gapStartLapped =
                (reserveCurrent > g.startFrame) &&
                ((reserveCurrent - g.startFrame) > (stagec::kRingCapacityFrames - stagec::kGuardFrames));
            const bool readLappedBeforeGap =
                (reserveCurrent > candidateFrame) &&
                ((reserveCurrent - candidateFrame) > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) &&
                (candidateFrame <= g.endFrame);
            if (!gapStartLapped && !readLappedBeforeGap) {
                break;
            }
            if (hasSongEndBarrier_ && g.endFrame >= songEndBarrierFrame_) {
                liveReadFrame_ = songEndBarrierFrame_;
                ClearSavedTailLocked();
                EmitSilencePlanarLocked(blockPtr, blockBytes, false);
                UpdateConsumerTelemetryLocked(nowQpc, commitBefore);
                res.ok = true;
                res.producerOnline = true;
                res.sessionGeneration = gen1;
                res.silenceFramesDelivered = framesPerBlock;
                res.readStartFrame = liveReadFrame_;
                res.readEndFrame = liveReadFrame_;
                res.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
                if (outEndFrame) {
                    *outEndFrame = liveReadFrame_;
                }
                LeaveCriticalSection(&cs_);
                return res;
            }
            candidateFrame = g.endFrame;
            pendingGaps_.pop_front();
            fastForwarded = true;
        }
        if (fastForwarded) {
            ClearSavedTailLocked();
            const bool candLapped =
                (reserveCurrent > candidateFrame) &&
                ((reserveCurrent - candidateFrame) > (stagec::kRingCapacityFrames - stagec::kGuardFrames));
            const uint64_t availAfterCand =
                (commitBefore > candidateFrame) ? (commitBefore - candidateFrame) : 0ULL;
            if (!candLapped && availAfterCand >= (static_cast<uint64_t>(framesPerBlock) + kSkipPostPullReserveFrames)) {
                liveReadFrame_ = candidateFrame;
                SetFadeInLocked(fadeTotalFrames_);
            } else if (!candLapped) {
                pendingSkipTargetFrame_ = candidateFrame;
                hasPendingSkip_ = true;
                if (FillPrimedSkipTargetLocked(blockPtr, blockBytes, nowQpc, gen1, steadyLeadFrames, res, outEndFrame)) {
                    LeaveCriticalSection(&cs_);
                    return res;
                }
            } else {
                liveReadFrame_ = (commitBefore > steadyLeadFrames) ? (commitBefore - steadyLeadFrames) : 0ULL;
                SetFadeInLocked(fadeTotalFrames_);
            }
        }
    }

    int16_t* outLR = scratchInterleaved_.data();
    const uint64_t reserveCheck = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
    if (!IsFrameInSavedTailLocked(liveReadFrame_, gen1) &&
        reserveCheck > liveReadFrame_ &&
        (reserveCheck - liveReadFrame_) > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) {
        const uint64_t newPos  = (commitBefore > steadyLeadFrames) ? (commitBefore - steadyLeadFrames) : 0ULL;
        const uint64_t skipped = (newPos > liveReadFrame_) ? (newPos - liveReadFrame_) : 0ULL;
        liveReadFrame_ = newPos;
        ClearSavedTailLocked();
        ClearSongEndBarrierLocked();
        while (!pendingGaps_.empty() && pendingGaps_.front().endFrame <= liveReadFrame_) {
            pendingGaps_.pop_front();
        }
        SetFadeInLocked(fadeTotalFrames_);
        stagec::AtomicAddU64(&hdr->overrun_events, 1ULL);
        if (skipped > 0ULL) {
            stagec::AtomicAddU64(&hdr->overrun_skipped_frames, skipped);
        }
    }

    uint32_t toCopy = 0u;
    uint64_t curRead = liveReadFrame_;
    uint64_t blockReadStart = liveReadFrame_;
    size_t gapsConsumed = 0u;
    bool trailingSpliceFadeIn = false;
    bool hitSongEndBarrier = false;
    bool readAnyFromRing = false;
    uint64_t ringReadMinFrame = 0ULL;

    for (int attempt = 0; attempt < 2; ++attempt) {
        commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        const uint64_t reserveLoop = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        curRead = liveReadFrame_;
        gapsConsumed = 0u;
        hitSongEndBarrier = false;
        readAnyFromRing = false;
        ringReadMinFrame = 0ULL;
        while (gapsConsumed < pendingGaps_.size() &&
               curRead >= pendingGaps_[gapsConsumed].endFrame &&
               (!hasSongEndBarrier_ || pendingGaps_[gapsConsumed].endFrame <= songEndBarrierFrame_)) {
            ++gapsConsumed;
        }
        if (gapsConsumed < pendingGaps_.size() &&
            curRead >= pendingGaps_[gapsConsumed].startFrame &&
            curRead < pendingGaps_[gapsConsumed].endFrame &&
            (!hasSongEndBarrier_ || pendingGaps_[gapsConsumed].startFrame < songEndBarrierFrame_)) {
            if (hasSongEndBarrier_ && pendingGaps_[gapsConsumed].endFrame >= songEndBarrierFrame_) {
                curRead = songEndBarrierFrame_;
                hitSongEndBarrier = true;
            } else {
                curRead = pendingGaps_[gapsConsumed].endFrame;
                ++gapsConsumed;
                if (fadeInRemaining_ == 0u) {
                    SetFadeInLocked(std::min<uint32_t>(240u, fadeTotalFrames_));
                }
            }
        }
        blockReadStart = curRead;

        toCopy = 0u;
        trailingSpliceFadeIn = false;
        while (!hitSongEndBarrier &&
               toCopy < framesPerBlock &&
               (commitBefore > curRead || IsFrameInSavedTailLocked(curRead, gen1))) {
            if (hasSongEndBarrier_ && curRead >= songEndBarrierFrame_) {
                hitSongEndBarrier = true;
                break;
            }
            while (gapsConsumed < pendingGaps_.size() && curRead >= pendingGaps_[gapsConsumed].startFrame) {
                if (hasSongEndBarrier_ && pendingGaps_[gapsConsumed].startFrame >= songEndBarrierFrame_) {
                    hitSongEndBarrier = true;
                    break;
                }
                if (curRead < pendingGaps_[gapsConsumed].endFrame) {
                    if (hasSongEndBarrier_ && pendingGaps_[gapsConsumed].endFrame >= songEndBarrierFrame_) {
                        curRead = songEndBarrierFrame_;
                        hitSongEndBarrier = true;
                        break;
                    }
                    curRead = pendingGaps_[gapsConsumed].endFrame;
                    trailingSpliceFadeIn = true;
                }
                ++gapsConsumed;
            }
            if (hitSongEndBarrier ||
                (commitBefore <= curRead && !IsFrameInSavedTailLocked(curRead, gen1))) {
                break;
            }

            const bool inSavedTail = IsFrameInSavedTailLocked(curRead, gen1);
            if (!inSavedTail &&
                reserveLoop > curRead &&
                (reserveLoop - curRead) > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) {
                if (gapsConsumed < pendingGaps_.size() &&
                    (!hasSongEndBarrier_ || pendingGaps_[gapsConsumed].startFrame < songEndBarrierFrame_)) {
                    if (hasSongEndBarrier_ && pendingGaps_[gapsConsumed].endFrame >= songEndBarrierFrame_) {
                        curRead = songEndBarrierFrame_;
                        hitSongEndBarrier = true;
                        break;
                    }
                    curRead = pendingGaps_[gapsConsumed].endFrame;
                    ++gapsConsumed;
                    trailingSpliceFadeIn = true;
                    continue;
                }
                break;
            }

            const uint64_t avail = inSavedTail ? (savedTailEndFrame_ - curRead) : (commitBefore - curRead);
            uint64_t segLimit = static_cast<uint64_t>(framesPerBlock - toCopy);
            if (inSavedTail) {
                segLimit = std::min<uint64_t>(segLimit, savedTailEndFrame_ - curRead);
            }
            bool hitsBarrier = false;
            if (hasSongEndBarrier_ && songEndBarrierFrame_ > curRead) {
                const uint64_t distToBarrier = songEndBarrierFrame_ - curRead;
                if (distToBarrier <= segLimit && distToBarrier <= avail) {
                    segLimit = distToBarrier;
                    hitsBarrier = true;
                }
            }
            bool hitsGapStart = false;
            if (gapsConsumed < pendingGaps_.size() &&
                pendingGaps_[gapsConsumed].startFrame > curRead &&
                (!hasSongEndBarrier_ || pendingGaps_[gapsConsumed].startFrame < songEndBarrierFrame_)) {
                const uint64_t distToGap = pendingGaps_[gapsConsumed].startFrame - curRead;
                if (distToGap <= segLimit && distToGap <= avail) {
                    segLimit = distToGap;
                    hitsGapStart = true;
                    hitsBarrier = false;
                }
            }

            const uint32_t segFrames = static_cast<uint32_t>(std::min<uint64_t>(segLimit, avail));
            if (segFrames == 0u) {
                break;
            }

            int16_t* segPtr = outLR + static_cast<size_t>(toCopy) * 2u;
            if (inSavedTail) {
                std::memcpy(segPtr,
                            savedTailInterleaved_.data() + static_cast<size_t>(curRead - savedTailStartFrame_) * 2u,
                            static_cast<size_t>(segFrames) * 2u * sizeof(int16_t));
            } else {
                CopyRingRangeInterleavedLocked(curRead, segFrames, segPtr);
                if (!readAnyFromRing || curRead < ringReadMinFrame) {
                    ringReadMinFrame = curRead;
                    readAnyFromRing = true;
                }
            }

            if (trailingSpliceFadeIn) {
                const uint32_t spliceSpan = std::min<uint32_t>(240u, fadeTotalFrames_);
                if (spliceSpan > 0u) {
                    ApplyCosineFadeWindowInLocked(segPtr, segFrames, 0u, spliceSpan);
                }
                trailingSpliceFadeIn = false;
            }

            if (hitsGapStart) {
                const uint32_t fadeOutSpan = std::min<uint32_t>(segFrames, std::min<uint32_t>(240u, fadeTotalFrames_));
                if (fadeOutSpan > 0u) {
                    ApplyCosineFadeOutTailExactLocked(segPtr, segFrames, fadeOutSpan);
                }
                if (hasSongEndBarrier_ && pendingGaps_[gapsConsumed].endFrame >= songEndBarrierFrame_) {
                    curRead = songEndBarrierFrame_;
                    hitSongEndBarrier = true;
                } else {
                    curRead = pendingGaps_[gapsConsumed].endFrame;
                    ++gapsConsumed;
                    trailingSpliceFadeIn = true;
                }
            } else if (hitsBarrier) {
                curRead += segFrames;
                hitSongEndBarrier = true;
            } else {
                curRead += segFrames;
            }

            toCopy += segFrames;
        }

        const uint64_t reserveAfter = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
        const bool conflict = (toCopy > 0u) &&
            ((gen2 != gen1) ||
             (readAnyFromRing && reserveAfter > ringReadMinFrame &&
              (reserveAfter - ringReadMinFrame) > stagec::kRingCapacityFrames));
        if (!conflict) {
            break;
        }

        stagec::AtomicAddU64(&hdr->overwrite_conflict_events, 1ULL);
        if (attempt == 0 && gen2 == gen1) {
            const uint64_t latestCommit = stagec::AtomicLoadU64(&hdr->write_commit_frame);
            liveReadFrame_ = (latestCommit > steadyLeadFrames) ? (latestCommit - steadyLeadFrames) : latestCommit;
            pendingGaps_.clear();
            ClearSavedTailLocked();
            SetFadeInLocked(fadeTotalFrames_);
        } else {
            const bool sameGen = (gen2 == gen1);
            if (!sameGen) {
                attachedGeneration_ = 0ULL;
                cursorAnchored_ = false;
                pendingGaps_.clear();
                ClearSavedTailLocked();
                ClearSongEndBarrierLocked();
                if (!cfg_.allowHotReattach) {
                    disconnectedLatched_ = true;
                }
            }
            EmitSilencePlanarLocked(blockPtr, blockBytes, true);
            res.ok = true;
            res.producerOnline = sameGen;
            res.sessionGeneration = sameGen ? gen1 : 0ULL;
            res.silenceFramesDelivered = framesPerBlock;
            res.readStartFrame = liveReadFrame_;
            res.readEndFrame = liveReadFrame_;
            if (outEndFrame) {
                *outEndFrame = liveReadFrame_;
            }
            LeaveCriticalSection(&cs_);
            return res;
        }
    }

    for (size_t gi = 0u; gi < gapsConsumed && !pendingGaps_.empty(); ++gi) {
        pendingGaps_.pop_front();
    }

    liveReadFrame_ = curRead;
    if (savedTailGeneration_ != 0ULL &&
        (savedTailGeneration_ != gen1 || liveReadFrame_ >= savedTailEndFrame_)) {
        ClearSavedTailLocked();
    }

    if (toCopy == 0u && hitSongEndBarrier) {
        EmitSilencePlanarLocked(blockPtr, blockBytes, false);
        UpdateConsumerTelemetryLocked(nowQpc, commitBefore);
        res.ok = true;
        res.producerOnline = true;
        res.sessionGeneration = gen1;
        res.silenceFramesDelivered = framesPerBlock;
        res.readStartFrame = liveReadFrame_;
        res.readEndFrame = liveReadFrame_;
        res.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
        if (outEndFrame) {
            *outEndFrame = liveReadFrame_;
        }
        LeaveCriticalSection(&cs_);
        return res;
    }

    res.readStartFrame = blockReadStart;
    res.liveFramesDelivered = toCopy;
    res.readEndFrame = liveReadFrame_;

    if (toCopy > 0u) {
        ApplyCosineFadeInLocked(outLR, toCopy);
    }
    if (trailingSpliceFadeIn && fadeInRemaining_ == 0u) {
        SetFadeInLocked(std::min<uint32_t>(240u, fadeTotalFrames_));
    }

    if (toCopy < framesPerBlock) {
        const uint32_t missing = framesPerBlock - toCopy;
        if (toCopy > 0u) {
            ApplyCosineFadeOutTailLocked(outLR, toCopy);
        }
        std::memset(outLR + static_cast<size_t>(toCopy) * 2u,
                    0,
                    static_cast<size_t>(missing) * 2u * sizeof(int16_t));
        fadeInRemaining_ = fadeTotalFrames_;
        fadeInSpanFrames_ = fadeTotalFrames_;
        lastOutputL_ = 0;
        lastOutputR_ = 0;
        hadNonZeroAudio_ = false;
        if (!hitSongEndBarrier) {
            stagec::AtomicAddU64(&hdr->underrun_events, 1ULL);
            stagec::AtomicAddU64(&hdr->underrun_frames, static_cast<uint64_t>(missing));
        }
        res.silenceFramesDelivered = missing;
    } else if (hitSongEndBarrier && toCopy > 0u) {
        ApplyCosineFadeOutTailLocked(outLR, toCopy);
        lastOutputL_ = 0;
        lastOutputR_ = 0;
        hadNonZeroAudio_ = false;
    } else if (toCopy > 0u) {
        const size_t lastPos = static_cast<size_t>(toCopy - 1u) * 2u;
        lastOutputL_ = outLR[lastPos];
        lastOutputR_ = outLR[lastPos + 1u];
        hadNonZeroAudio_ = true;
    }

    WriteInterleavedToPlanarBlock(blockPtr, blockBytes, outLR, framesPerBlock);
    UpdateConsumerTelemetryLocked(nowQpc, commitBefore);

    res.ok = true;
    res.producerOnline = true;
    res.sessionGeneration = gen1;
    res.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
    if (outEndFrame) {
        *outEndFrame = liveReadFrame_;
    }

    LeaveCriticalSection(&cs_);
    return res;
}

RouteBPullStatus RouteBIpcConsumer::FillPreloadedBlocksAtPlay(void* block0,
                                                              void* block1,
                                                              uint32_t blockBytes,
                                                              bool hasPrerollAnchor,
                                                              uint64_t prerollStartFrame,
                                                              uint32_t minPostPlayReserveFrames,
                                                              uint64_t* outEndFrame,
                                                              uint32_t minHeadPadFrames) {
    RouteBPullStatus res{};
    const uint32_t framesPerBlock = ComputeBlockFrameCount(blockBytes);
    res.framesPerBlock = framesPerBlock;
    if (!block0 || !block1 || framesPerBlock == 0u) {
        return res;
    }

    const uint32_t totalFrames = framesPerBlock * 2u;
    EnterCriticalSection(&cs_);

    hasPendingSkip_ = false;
    pendingSkipTargetFrame_ = 0ULL;
    ClearSongEndBarrierLocked();
    ClearSavedTailLocked();
    if (!hasPrerollAnchor) {
        pendingGaps_.clear();
    }

    LARGE_INTEGER nowQpcLi{};
    QueryPerformanceCounter(&nowQpcLi);
    const int64_t nowQpc = nowQpcLi.QuadPart;
    const int64_t nowUs  = QpcToMicros(nowQpc, localQpcFreq_.QuadPart);

    uint64_t liveGen = 0ULL;
    const bool producerLive = RefreshProducerAttachmentLocked(nowQpc, nowUs, liveGen);
    if (!producerLive) {
        pendingGaps_.clear();
        EmitSilencePlanarLocked(block0, blockBytes, true);
        ZeroPlanarBlock(block1, blockBytes);
        cursorAnchored_ = false;
        res.ok = true;
        res.producerOnline = false;
        res.sessionGeneration = attachedGeneration_;
        res.silenceFramesDelivered = totalFrames;
        if (outEndFrame) {
            *outEndFrame = liveReadFrame_;
        }
        LeaveCriticalSection(&cs_);
        return res;
    }

    stagec::StageCIpcRingHeader* hdr = &shared_->header;
    const uint64_t gen1 = attachedGeneration_;
    const uint64_t steadyLeadFrames = (static_cast<uint64_t>(cfg_.steadyLeadMs) * stagec::kSampleRate) / 1000ULL;

    scratchInterleaved_.resize(static_cast<size_t>(totalFrames) * 2u);
    int16_t* fullLR = scratchInterleaved_.data();

    uint64_t commitBefore = 0ULL;
    uint64_t readStart    = 0ULL;
    uint64_t curRead      = 0ULL;
    uint32_t liveFrames   = 0u;
    uint32_t copied       = 0u;
    uint32_t padFrames    = totalFrames;
    bool usedPreroll      = false;
    size_t gapsConsumed   = 0u;
    bool trailingSpliceFadeIn = false;
    bool readAnyFromRing  = false;
    uint64_t ringReadMinFrame = 0ULL;

    for (int attempt = 0; attempt < 2; ++attempt) {
        commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        const uint64_t reserveNow = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        gapsConsumed = 0u;
        readAnyFromRing = false;
        ringReadMinFrame = 0ULL;
        trailingSpliceFadeIn = false;

        if (hasPrerollAnchor) {
            uint64_t effectiveStart = prerollStartFrame;
            while (gapsConsumed < pendingGaps_.size() && pendingGaps_[gapsConsumed].endFrame <= effectiveStart) {
                ++gapsConsumed;
            }
            if (gapsConsumed < pendingGaps_.size() &&
                effectiveStart >= pendingGaps_[gapsConsumed].startFrame &&
                effectiveStart < pendingGaps_[gapsConsumed].endFrame) {
                effectiveStart = pendingGaps_[gapsConsumed].endFrame;
                ++gapsConsumed;
            }
            while (gapsConsumed < pendingGaps_.size()) {
                const SkipGapRange& g = pendingGaps_[gapsConsumed];
                const bool gapStartLapped =
                    (reserveNow > g.startFrame) &&
                    ((reserveNow - g.startFrame) > (stagec::kRingCapacityFrames - stagec::kGuardFrames));
                const bool readLappedBeforeGap =
                    (reserveNow > effectiveStart) &&
                    ((reserveNow - effectiveStart) > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) &&
                    (effectiveStart <= g.endFrame);
                if (!gapStartLapped && !readLappedBeforeGap) {
                    break;
                }
                effectiveStart = g.endFrame;
                ++gapsConsumed;
            }

            const bool notLapped =
                (reserveNow <= effectiveStart) ||
                ((reserveNow - effectiveStart) <= (stagec::kRingCapacityFrames - stagec::kGuardFrames));
            if (notLapped && commitBefore > effectiveStart) {
                const uint64_t effectiveAvail = CountEffectiveFramesExcludingGapsLocked(
                    effectiveStart, commitBefore, gapsConsumed);
                const uint64_t usable64 = (effectiveAvail > minPostPlayReserveFrames)
                    ? std::min<uint64_t>(totalFrames, effectiveAvail - minPostPlayReserveFrames)
                    : 0ULL;
                const uint32_t maxLive = (totalFrames > minHeadPadFrames)
                    ? (totalFrames - minHeadPadFrames)
                    : 0u;
                liveFrames  = static_cast<uint32_t>(std::min<uint64_t>(usable64, static_cast<uint64_t>(maxLive)));
                padFrames   = totalFrames - liveFrames;
                readStart   = effectiveStart;
                usedPreroll = (liveFrames > 0u);
            } else {
                liveFrames  = 0u;
                padFrames   = totalFrames;
                readStart   = commitBefore;
                usedPreroll = false;
            }
        } else {
            const uint64_t anchor = (commitBefore >= steadyLeadFrames) ? (commitBefore - steadyLeadFrames) : 0ULL;
            const uint64_t avail  = (commitBefore > anchor) ? (commitBefore - anchor) : 0ULL;
            const uint64_t usable64 = (avail > minPostPlayReserveFrames)
                ? std::min<uint64_t>(totalFrames, avail - minPostPlayReserveFrames)
                : 0ULL;
            liveFrames  = static_cast<uint32_t>(usable64);
            padFrames   = totalFrames - liveFrames;
            readStart   = anchor;
            usedPreroll = false;
        }

        curRead = readStart;
        copied = 0u;
        if (padFrames > 0u) {
            std::memset(fullLR, 0, static_cast<size_t>(padFrames) * 2u * sizeof(int16_t));
        }
        if (liveFrames > 0u) {
            int16_t* dstBase = fullLR + static_cast<size_t>(padFrames) * 2u;
            if (!hasPrerollAnchor) {
                CopyRingRangeInterleavedLocked(readStart, liveFrames, dstBase);
                curRead = readStart + liveFrames;
                copied = liveFrames;
                readAnyFromRing = true;
                ringReadMinFrame = readStart;
            } else {
                while (copied < liveFrames && commitBefore > curRead) {
                    while (gapsConsumed < pendingGaps_.size() && curRead >= pendingGaps_[gapsConsumed].startFrame) {
                        if (curRead < pendingGaps_[gapsConsumed].endFrame) {
                            curRead = pendingGaps_[gapsConsumed].endFrame;
                            trailingSpliceFadeIn = (copied > 0u);
                        }
                        ++gapsConsumed;
                    }
                    if (commitBefore <= curRead) {
                        break;
                    }

                    const uint64_t avail = commitBefore - curRead;
                    uint64_t segLimit = static_cast<uint64_t>(liveFrames - copied);
                    bool hitsGapStart = false;
                    if (gapsConsumed < pendingGaps_.size() && pendingGaps_[gapsConsumed].startFrame > curRead) {
                        const uint64_t distToGap = pendingGaps_[gapsConsumed].startFrame - curRead;
                        if (distToGap <= segLimit && distToGap <= avail) {
                            segLimit = distToGap;
                            hitsGapStart = true;
                        }
                    }

                    const uint32_t segFrames = static_cast<uint32_t>(std::min<uint64_t>(segLimit, avail));
                    if (segFrames == 0u) {
                        break;
                    }

                    int16_t* segPtr = dstBase + static_cast<size_t>(copied) * 2u;
                    CopyRingRangeInterleavedLocked(curRead, segFrames, segPtr);
                    if (!readAnyFromRing || curRead < ringReadMinFrame) {
                        ringReadMinFrame = curRead;
                        readAnyFromRing = true;
                    }

                    if (trailingSpliceFadeIn) {
                        const uint32_t spliceSpan = std::min<uint32_t>(240u, fadeTotalFrames_);
                        if (spliceSpan > 0u) {
                            ApplyCosineFadeWindowInLocked(segPtr, segFrames, 0u, spliceSpan);
                        }
                        trailingSpliceFadeIn = false;
                    }

                    if (hitsGapStart) {
                        const uint32_t fadeOutSpan = std::min<uint32_t>(segFrames, std::min<uint32_t>(240u, fadeTotalFrames_));
                        if (fadeOutSpan > 0u) {
                            ApplyCosineFadeOutTailExactLocked(segPtr, segFrames, fadeOutSpan);
                        }
                        curRead = pendingGaps_[gapsConsumed].endFrame;
                        ++gapsConsumed;
                        trailingSpliceFadeIn = true;
                    } else {
                        curRead += segFrames;
                    }

                    copied += segFrames;
                }
            }
        }

        const uint64_t reserveAfter = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
        const bool conflict = (copied > 0u) &&
            ((gen2 != gen1) ||
             (readAnyFromRing && reserveAfter > ringReadMinFrame &&
              (reserveAfter - ringReadMinFrame) > stagec::kRingCapacityFrames));
        if (!conflict) {
            break;
        }

        stagec::AtomicAddU64(&hdr->overwrite_conflict_events, 1ULL);
        if (attempt == 1 || gen2 != gen1) {
            if (gen2 != gen1) {
                attachedGeneration_ = 0ULL;
                pendingGaps_.clear();
                ClearSavedTailLocked();
                ClearSongEndBarrierLocked();
                if (!cfg_.allowHotReattach) {
                    disconnectedLatched_ = true;
                }
            }
            ZeroPlanarBlock(block0, blockBytes);
            ZeroPlanarBlock(block1, blockBytes);
            cursorAnchored_ = false;
            hadNonZeroAudio_ = false;
            lastOutputL_ = 0;
            lastOutputR_ = 0;
            res.ok = true;
            res.producerOnline = (gen2 == gen1);
            res.sessionGeneration = (gen2 == gen1) ? gen1 : 0ULL;
            res.silenceFramesDelivered = totalFrames;
            LeaveCriticalSection(&cs_);
            return res;
        }
    }

    for (size_t gi = 0u; gi < gapsConsumed && !pendingGaps_.empty(); ++gi) {
        pendingGaps_.pop_front();
    }

    padFrames = totalFrames - copied;
    if (copied > 0u) {
        fadeInRemaining_ = fadeTotalFrames_;
        fadeInSpanFrames_ = fadeTotalFrames_;
        ApplyCosineFadeInLocked(fullLR + static_cast<size_t>(padFrames) * 2u, copied);
        if (trailingSpliceFadeIn && fadeInRemaining_ == 0u) {
            SetFadeInLocked(std::min<uint32_t>(240u, fadeTotalFrames_));
        }
        const size_t lastPos = static_cast<size_t>(totalFrames - 1u) * 2u;
        lastOutputL_ = fullLR[lastPos];
        lastOutputR_ = fullLR[lastPos + 1u];
        hadNonZeroAudio_ = true;
        liveReadFrame_ = curRead;
        cursorAnchored_ = true;
    } else {
        fadeInRemaining_ = fadeTotalFrames_;
        fadeInSpanFrames_ = fadeTotalFrames_;
        lastOutputL_ = 0;
        lastOutputR_ = 0;
        hadNonZeroAudio_ = false;
        cursorAnchored_ = false;
    }

    WriteInterleavedToPlanarBlock(block0, blockBytes, fullLR, framesPerBlock);
    WriteInterleavedToPlanarBlock(block1, blockBytes, fullLR + static_cast<size_t>(framesPerBlock) * 2u, framesPerBlock);
    UpdateConsumerTelemetryLocked(nowQpc, commitBefore);

    res.ok = true;
    res.producerOnline = true;
    res.usedPrerollAnchor = usedPreroll;
    res.sessionGeneration = gen1;
    res.readStartFrame = readStart;
    res.readEndFrame = liveReadFrame_;
    res.liveFramesDelivered = copied;
    res.silenceFramesDelivered = padFrames;
    res.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
    if (outEndFrame) {
        *outEndFrame = liveReadFrame_;
    }

    LeaveCriticalSection(&cs_);
    return res;
}

bool RouteBIpcConsumer::TryEnsureMappingLocked(int64_t nowUs) {
    if (shared_ != nullptr) {
        return true;
    }
    if (externalBlock_ || cfg_.mappingName.empty()) {
        return false;
    }
    if (lastOpenAttemptUs_ != 0 && nowUs >= lastOpenAttemptUs_ && (nowUs - lastOpenAttemptUs_) < 50000LL) {
        return false;
    }
    lastOpenAttemptUs_ = (nowUs > 0) ? nowUs : 1LL;

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, cfg_.mappingName.c_str());
    if (!hMap) {
        return false;
    }

    void* view = MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < stagec::kTotalSharedBytes) {
        UnmapViewOfFile(view);
        CloseHandle(hMap);
        return false;
    }

    hMapping_ = hMap;
    shared_ = static_cast<stagec::StageCIpcSharedBlock*>(view);
    mappedBytes_ = mbi.RegionSize;
    return true;
}

void RouteBIpcConsumer::CloseMappingLocked() {
    if (!externalBlock_) {
        if (shared_) {
            UnmapViewOfFile(shared_);
        }
        if (hMapping_) {
            CloseHandle(hMapping_);
        }
    }
    shared_ = nullptr;
    hMapping_ = nullptr;
    mappedBytes_ = 0u;
    externalBlock_ = false;
}

bool RouteBIpcConsumer::ValidateHeaderStaticFieldsLocked(const stagec::StageCIpcRingHeader* hdr) const {
    if (!hdr) {
        return false;
    }
    return stagec::AtomicLoad32(&hdr->magic) == stagec::kIpcMagic &&
           stagec::AtomicLoad32(&hdr->version) == stagec::kIpcVersion &&
           stagec::AtomicLoad32(&hdr->header_bytes) == stagec::kHeaderBytes &&
           stagec::AtomicLoad32(&hdr->total_bytes) == stagec::kTotalSharedBytes &&
           stagec::AtomicLoad32(&hdr->sample_rate) == stagec::kSampleRate &&
           stagec::AtomicLoad32(&hdr->channels) == stagec::kChannels &&
           stagec::AtomicLoad32(&hdr->bits_per_sample) == stagec::kBitsPerSample &&
           stagec::AtomicLoad32(&hdr->capacity_frames) == stagec::kRingCapacityFrames &&
           stagec::AtomicLoadU64(&hdr->qpc_freq) > 0ULL;
}

bool RouteBIpcConsumer::IsProducerLiveLocked(const stagec::StageCIpcRingHeader* hdr,
                                             int64_t nowQpc,
                                             uint64_t& outGen) const {
    outGen = 0ULL;
    if (!hdr || localQpcFreq_.QuadPart <= 0) {
        return false;
    }

    const uint64_t gen1 = stagec::AtomicLoadU64(&hdr->session_generation);
    if (!stagec::IsReadyEvenGeneration(gen1) || !ValidateHeaderStaticFieldsLocked(hdr)) {
        return false;
    }
    if (stagec::AtomicLoad32(&hdr->producer_state) != static_cast<uint32_t>(stagec::ProducerState::Streaming)) {
        return false;
    }

    const int64_t hbQpc = stagec::AtomicLoadI64(&hdr->last_heartbeat_qpc);
    if (hbQpc <= 0) {
        return false;
    }
    const int64_t ageTicks = nowQpc - hbQpc;
    if (ageTicks > 0 && QpcTicksToMillis(ageTicks, localQpcFreq_.QuadPart) > static_cast<int64_t>(cfg_.heartbeatTimeoutMs)) {
        return false;
    }

    const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
    if (gen2 != gen1) {
        return false;
    }

    outGen = gen1;
    return true;
}

bool RouteBIpcConsumer::RefreshProducerAttachmentLocked(int64_t nowQpc,
                                                        int64_t nowUs,
                                                        uint64_t& outGen) {
    outGen = 0ULL;
    const bool mapped = TryEnsureMappingLocked(nowUs);
    uint64_t liveGen = 0ULL;
    const bool rawLive = mapped && IsProducerLiveLocked(&shared_->header, nowQpc, liveGen);

    if (attachedGeneration_ != 0ULL && !rawLive) {
        attachedGeneration_ = 0ULL;
        cursorAnchored_ = false;
        hasPendingSkip_ = false;
        pendingGaps_.clear();
        ClearSavedTailLocked();
        ClearSongEndBarrierLocked();
        if (!cfg_.allowHotReattach) {
            disconnectedLatched_ = true;
        }
    }

    const bool producerLive = rawLive && !disconnectedLatched_;
    if (producerLive && attachedGeneration_ != liveGen) {
        const bool switchingLiveGen = (attachedGeneration_ != 0ULL);
        attachedGeneration_ = liveGen;
        cursorAnchored_ = false;
        hasPendingSkip_ = false;
        if (switchingLiveGen) {
            pendingGaps_.clear();
        }
        ClearSavedTailLocked();
        ClearSongEndBarrierLocked();
        fadeTotalFrames_ = (cfg_.fadeMs * stagec::kSampleRate) / 1000u;
        SetFadeInLocked(fadeTotalFrames_);
        stagec::AtomicAddU64(&shared_->header.session_attach_count, 1ULL);
        stagec::AtomicStore32(&shared_->header.consumer_pid, GetCurrentProcessId());
    }

    outGen = producerLive ? attachedGeneration_ : 0ULL;
    return producerLive;
}

void RouteBIpcConsumer::CopyRingRangeInterleavedLocked(uint64_t startFrame,
                                                       uint32_t frameCount,
                                                       int16_t* outLR) const {
    const uint32_t idx = static_cast<uint32_t>(startFrame & stagec::kRingFrameMask);
    const uint32_t firstChunk = std::min<uint32_t>(frameCount, stagec::kRingCapacityFrames - idx);
    const uint32_t secondChunk = frameCount - firstChunk;

    std::memcpy(outLR,
                shared_->pcm_samples + static_cast<size_t>(idx) * 2u,
                static_cast<size_t>(firstChunk) * 2u * sizeof(int16_t));
    if (secondChunk > 0u) {
        std::memcpy(outLR + static_cast<size_t>(firstChunk) * 2u,
                    shared_->pcm_samples,
                    static_cast<size_t>(secondChunk) * 2u * sizeof(int16_t));
    }
}

void RouteBIpcConsumer::SetFadeInLocked(uint32_t spanFrames) noexcept {
    fadeInSpanFrames_ = spanFrames;
    fadeInRemaining_  = spanFrames;
}

void RouteBIpcConsumer::ApplyCosineFadeWindowInLocked(int16_t* interleavedLR,
                                                      uint32_t frameCount,
                                                      uint32_t alreadyDone,
                                                      uint32_t spanFrames) {
    if (!interleavedLR || frameCount == 0u || spanFrames == 0u || alreadyDone >= spanFrames) {
        return;
    }
    constexpr double kPi = 3.14159265358979323846;
    const uint32_t fadeCount = std::min<uint32_t>(frameCount, spanFrames - alreadyDone);
    for (uint32_t i = 0u; i < fadeCount; ++i) {
        const double done = static_cast<double>(alreadyDone + i);
        const double gain = 0.5 * (1.0 - std::cos(kPi * (done + 0.5) / static_cast<double>(spanFrames)));
        const size_t pos = static_cast<size_t>(i) * 2u;
        const long long lVal = std::llround(static_cast<double>(interleavedLR[pos]) * gain);
        const long long rVal = std::llround(static_cast<double>(interleavedLR[pos + 1u]) * gain);
        interleavedLR[pos]      = static_cast<int16_t>(std::clamp(lVal, -32768LL, 32767LL));
        interleavedLR[pos + 1u] = static_cast<int16_t>(std::clamp(rVal, -32768LL, 32767LL));
    }
}

void RouteBIpcConsumer::ApplyCosineFadeInLocked(int16_t* interleavedLR, uint32_t frameCount) {
    if (!interleavedLR || fadeInRemaining_ == 0u || fadeInSpanFrames_ == 0u || frameCount == 0u) {
        return;
    }
    const uint32_t fadeCount = std::min<uint32_t>(frameCount, fadeInRemaining_);
    const uint32_t alreadyDone = (fadeInSpanFrames_ > fadeInRemaining_)
        ? (fadeInSpanFrames_ - fadeInRemaining_)
        : 0u;
    ApplyCosineFadeWindowInLocked(interleavedLR, fadeCount, alreadyDone, fadeInSpanFrames_);
    fadeInRemaining_ -= fadeCount;
}

void RouteBIpcConsumer::ApplyCosineFadeOutTailExactLocked(int16_t* interleavedLR,
                                                           uint32_t totalFrames,
                                                           uint32_t fadeLen) {
    if (!interleavedLR || totalFrames == 0u || fadeLen == 0u) {
        return;
    }
    constexpr double kPi = 3.14159265358979323846;
    const uint32_t clampedLen = std::min<uint32_t>(totalFrames, fadeLen);
    const uint32_t startIdx = totalFrames - clampedLen;
    for (uint32_t i = 0u; i < clampedLen; ++i) {
        const double gain = 0.5 * (1.0 + std::cos(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(clampedLen)));
        const size_t pos = static_cast<size_t>(startIdx + i) * 2u;
        const long long lVal = std::llround(static_cast<double>(interleavedLR[pos]) * gain);
        const long long rVal = std::llround(static_cast<double>(interleavedLR[pos + 1u]) * gain);
        interleavedLR[pos]      = static_cast<int16_t>(std::clamp(lVal, -32768LL, 32767LL));
        interleavedLR[pos + 1u] = static_cast<int16_t>(std::clamp(rVal, -32768LL, 32767LL));
    }
}

void RouteBIpcConsumer::ApplyCosineFadeOutTailLocked(int16_t* interleavedLR, uint32_t totalFrames) {
    if (!interleavedLR || totalFrames == 0u || fadeTotalFrames_ == 0u) {
        return;
    }
    const uint32_t fadeLen = std::min<uint32_t>(totalFrames, std::min<uint32_t>(240u, fadeTotalFrames_));
    ApplyCosineFadeOutTailExactLocked(interleavedLR, totalFrames, fadeLen);
}

void RouteBIpcConsumer::EmitSilencePlanarLocked(void* blockPtr, uint32_t blockBytes, bool countAsDisconnect) {
    const uint32_t framesPerBlock = ComputeBlockFrameCount(blockBytes);
    if (!blockPtr || framesPerBlock == 0u) {
        return;
    }

    if (hadNonZeroAudio_ && fadeTotalFrames_ > 0u) {
        scratchInterleaved_.resize(static_cast<size_t>(framesPerBlock) * 2u);
        int16_t* outLR = scratchInterleaved_.data();
        constexpr double kPi = 3.14159265358979323846;
        const uint32_t fadeN = std::min<uint32_t>(framesPerBlock, fadeTotalFrames_);
        for (uint32_t i = 0u; i < fadeN; ++i) {
            const double gain = 0.5 * (1.0 + std::cos(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(fadeN)));
            const long long lVal = std::llround(static_cast<double>(lastOutputL_) * gain);
            const long long rVal = std::llround(static_cast<double>(lastOutputR_) * gain);
            outLR[static_cast<size_t>(i) * 2u]      = static_cast<int16_t>(std::clamp(lVal, -32768LL, 32767LL));
            outLR[static_cast<size_t>(i) * 2u + 1u] = static_cast<int16_t>(std::clamp(rVal, -32768LL, 32767LL));
        }
        if (fadeN < framesPerBlock) {
            std::memset(outLR + static_cast<size_t>(fadeN) * 2u,
                        0,
                        static_cast<size_t>(framesPerBlock - fadeN) * 2u * sizeof(int16_t));
        }
        WriteInterleavedToPlanarBlock(blockPtr, blockBytes, outLR, framesPerBlock);
    } else {
        ZeroPlanarBlock(blockPtr, blockBytes);
    }

    hadNonZeroAudio_ = false;
    lastOutputL_ = 0;
    lastOutputR_ = 0;

    if (countAsDisconnect &&
        shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicAddU64(&shared_->header.silence_disconnect_frames, static_cast<uint64_t>(framesPerBlock));
    }
}

void RouteBIpcConsumer::UpdateConsumerTelemetryLocked(int64_t nowQpc, uint64_t commitFrame) noexcept {
    if (!shared_ || mappedBytes_ < stagec::kTotalSharedBytes) {
        return;
    }
    stagec::StageCIpcRingHeader* hdr = &shared_->header;
    const uint64_t wm = (commitFrame >= liveReadFrame_) ? (commitFrame - liveReadFrame_) : 0ULL;
    const uint64_t frameAgeUs = (wm * 1000000ULL) / stagec::kSampleRate;
    stagec::AtomicStoreU64(&hdr->consumer_read_frame, liveReadFrame_);
    stagec::AtomicStoreI64(&hdr->consumer_heartbeat_qpc, nowQpc);
    stagec::AtomicStoreU64(&hdr->watermark_frames, wm);
    stagec::AtomicStoreU64(&hdr->last_frame_age_us, frameAgeUs);
    stagec::AtomicStore32(&hdr->consumer_active_readers, activeReaderCount_);
}

void RouteBIpcConsumer::ClearSavedTailLocked() noexcept {
    savedTailInterleaved_.clear();
    savedTailStartFrame_ = 0ULL;
    savedTailEndFrame_   = 0ULL;
    savedTailGeneration_ = 0ULL;
}

void RouteBIpcConsumer::ClearSongEndBarrierLocked() noexcept {
    hasSongEndBarrier_   = false;
    songEndBarrierFrame_ = 0ULL;
}

bool RouteBIpcConsumer::IsFrameInSavedTailLocked(uint64_t frame, uint64_t gen) const noexcept {
    if (savedTailGeneration_ == 0ULL || savedTailGeneration_ != gen) {
        return false;
    }
    if (frame < savedTailStartFrame_ || frame >= savedTailEndFrame_) {
        return false;
    }
    const uint64_t span = savedTailEndFrame_ - savedTailStartFrame_;
    return (span * 2ULL) <= savedTailInterleaved_.size();
}

bool RouteBIpcConsumer::IsRangeCoveredBySavedTailLocked(uint64_t startFrame,
                                                        uint64_t endFrame,
                                                        uint64_t gen) const noexcept {
    if (savedTailGeneration_ == 0ULL || savedTailGeneration_ != gen || startFrame > endFrame) {
        return false;
    }
    if (startFrame < savedTailStartFrame_ || endFrame > savedTailEndFrame_) {
        return false;
    }
    const uint64_t span = savedTailEndFrame_ - savedTailStartFrame_;
    return (span * 2ULL) <= savedTailInterleaved_.size();
}

uint64_t RouteBIpcConsumer::CountEffectiveFramesExcludingGapsLocked(uint64_t startFrame,
                                                                     uint64_t commitFrame,
                                                                     size_t firstGapIdx) const noexcept {
    if (commitFrame <= startFrame) {
        return 0ULL;
    }
    uint64_t cursor = startFrame;
    uint64_t total  = 0ULL;
    size_t idx = firstGapIdx;
    while (idx < pendingGaps_.size() && cursor < commitFrame) {
        const SkipGapRange& g = pendingGaps_[idx];
        if (g.endFrame <= cursor) {
            ++idx;
            continue;
        }
        if (g.startFrame > cursor) {
            const uint64_t segEnd = std::min<uint64_t>(g.startFrame, commitFrame);
            total += (segEnd - cursor);
            cursor = segEnd;
            if (cursor >= commitFrame) {
                break;
            }
        }
        if (g.endFrame >= commitFrame) {
            cursor = commitFrame;
            break;
        }
        cursor = g.endFrame;
        ++idx;
    }
    if (commitFrame > cursor) {
        total += (commitFrame - cursor);
    }
    return total;
}

bool RouteBIpcConsumer::FillPrimedSkipTargetLocked(void* blockPtr,
                                                   uint32_t blockBytes,
                                                   int64_t nowQpc,
                                                   uint64_t gen1,
                                                   uint64_t steadyLeadFrames,
                                                   RouteBPullStatus& outRes,
                                                   uint64_t* outEndFrame) {
    const uint32_t framesPerBlock = ComputeBlockFrameCount(blockBytes);
    outRes = RouteBPullStatus{};
    outRes.framesPerBlock = framesPerBlock;
    if (!blockPtr || framesPerBlock == 0u || !shared_) {
        return false;
    }

    stagec::StageCIpcRingHeader* hdr = &shared_->header;
    uint64_t commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
    const uint64_t reserveBefore = stagec::AtomicLoadU64(&hdr->write_reserve_frame);

    const bool targetLapped =
        (reserveBefore > pendingSkipTargetFrame_) &&
        ((reserveBefore - pendingSkipTargetFrame_) > (stagec::kRingCapacityFrames - stagec::kGuardFrames));
    if (targetLapped) {
        liveReadFrame_ = (commitBefore >= steadyLeadFrames) ? (commitBefore - steadyLeadFrames) : 0ULL;
        cursorAnchored_ = true;
        hasPendingSkip_ = false;
        pendingSkipTargetFrame_ = 0ULL;
        pendingGaps_.clear();
        ClearSavedTailLocked();
        ClearSongEndBarrierLocked();
        SetFadeInLocked(fadeTotalFrames_);
        return false;
    }

    uint64_t skipAvail = (commitBefore > pendingSkipTargetFrame_)
        ? (commitBefore - pendingSkipTargetFrame_)
        : 0ULL;
    if (skipAvail <= kSkipPostPullReserveFrames) {
        EmitSilencePlanarLocked(blockPtr, blockBytes, false);
        outRes.ok = true;
        outRes.producerOnline = true;
        outRes.sessionGeneration = gen1;
        outRes.silenceFramesDelivered = framesPerBlock;
        outRes.readStartFrame = pendingSkipTargetFrame_;
        outRes.readEndFrame = pendingSkipTargetFrame_;
        outRes.watermarkFrames = skipAvail;
        if (outEndFrame) {
            *outEndFrame = pendingSkipTargetFrame_;
        }
        return true;
    }

    scratchInterleaved_.resize(static_cast<size_t>(framesPerBlock) * 2u);
    int16_t* outLR = scratchInterleaved_.data();

    uint32_t usable = 0u;
    uint32_t pad = 0u;
    for (int attempt = 0; attempt < 2; ++attempt) {
        commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        skipAvail = (commitBefore > pendingSkipTargetFrame_)
            ? (commitBefore - pendingSkipTargetFrame_)
            : 0ULL;
        usable = (skipAvail > kSkipPostPullReserveFrames)
            ? static_cast<uint32_t>(std::min<uint64_t>(framesPerBlock, skipAvail - kSkipPostPullReserveFrames))
            : 0u;
        pad = framesPerBlock - usable;

        if (pad > 0u) {
            std::memset(outLR, 0, static_cast<size_t>(pad) * 2u * sizeof(int16_t));
            if (hadNonZeroAudio_ && fadeTotalFrames_ > 0u) {
                constexpr double kPi = 3.14159265358979323846;
                const uint32_t fadeN = std::min<uint32_t>(pad, fadeTotalFrames_);
                for (uint32_t fi = 0u; fi < fadeN; ++fi) {
                    const double gain = 0.5 * (1.0 + std::cos(kPi * (static_cast<double>(fi) + 0.5) / static_cast<double>(fadeN)));
                    const long long lVal = std::llround(static_cast<double>(lastOutputL_) * gain);
                    const long long rVal = std::llround(static_cast<double>(lastOutputR_) * gain);
                    outLR[static_cast<size_t>(fi) * 2u]      = static_cast<int16_t>(std::clamp(lVal, -32768LL, 32767LL));
                    outLR[static_cast<size_t>(fi) * 2u + 1u] = static_cast<int16_t>(std::clamp(rVal, -32768LL, 32767LL));
                }
            }
        }
        if (usable > 0u) {
            CopyRingRangeInterleavedLocked(
                pendingSkipTargetFrame_, usable, outLR + static_cast<size_t>(pad) * 2u);
        }

        const uint64_t reserveAfter = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
        const bool conflict = (usable > 0u) &&
            ((gen2 != gen1) ||
             (reserveAfter > pendingSkipTargetFrame_ &&
              (reserveAfter - pendingSkipTargetFrame_) > stagec::kRingCapacityFrames));
        if (!conflict) {
            break;
        }

        stagec::AtomicAddU64(&hdr->overwrite_conflict_events, 1ULL);
        if (attempt == 1 || gen2 != gen1) {
            if (gen2 != gen1) {
                attachedGeneration_ = 0ULL;
                cursorAnchored_ = false;
                hasPendingSkip_ = false;
                pendingGaps_.clear();
                ClearSavedTailLocked();
                ClearSongEndBarrierLocked();
                if (!cfg_.allowHotReattach) {
                    disconnectedLatched_ = true;
                }
            }
            EmitSilencePlanarLocked(blockPtr, blockBytes, true);
            outRes.ok = true;
            outRes.producerOnline = (gen2 == gen1);
            outRes.sessionGeneration = (gen2 == gen1) ? gen1 : 0ULL;
            outRes.silenceFramesDelivered = framesPerBlock;
            outRes.readStartFrame = pendingSkipTargetFrame_;
            outRes.readEndFrame = pendingSkipTargetFrame_;
            if (outEndFrame) {
                *outEndFrame = pendingSkipTargetFrame_;
            }
            return true;
        }
    }

    if (usable > 0u) {
        SetFadeInLocked(fadeTotalFrames_);
        ApplyCosineFadeInLocked(outLR + static_cast<size_t>(pad) * 2u, usable);
        const size_t lastPos = static_cast<size_t>(framesPerBlock - 1u) * 2u;
        lastOutputL_ = outLR[lastPos];
        lastOutputR_ = outLR[lastPos + 1u];
        hadNonZeroAudio_ = true;
    } else {
        lastOutputL_ = 0;
        lastOutputR_ = 0;
        hadNonZeroAudio_ = false;
    }

    liveReadFrame_ = pendingSkipTargetFrame_ + usable;
    cursorAnchored_ = true;
    hasPendingSkip_ = false;
    ClearSavedTailLocked();
    ClearSongEndBarrierLocked();

    WriteInterleavedToPlanarBlock(blockPtr, blockBytes, outLR, framesPerBlock);
    UpdateConsumerTelemetryLocked(nowQpc, commitBefore);

    outRes.ok = true;
    outRes.producerOnline = true;
    outRes.sessionGeneration = gen1;
    outRes.readStartFrame = pendingSkipTargetFrame_;
    outRes.readEndFrame = liveReadFrame_;
    outRes.liveFramesDelivered = usable;
    outRes.silenceFramesDelivered = pad;
    outRes.watermarkFrames = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
    if (outEndFrame) {
        *outEndFrame = liveReadFrame_;
    }
    pendingSkipTargetFrame_ = 0ULL;
    return true;
}

} // namespace routeb
