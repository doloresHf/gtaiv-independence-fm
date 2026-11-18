#include "stage_c_ipc.h"
#include "async_logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace probe {

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

StageCIpcManager& StageCIpcManager::Instance() {
    static StageCIpcManager inst;
    return inst;
}

StageCIpcManager::StageCIpcManager() {
    InitializeCriticalSection(&cs_);
    QueryPerformanceFrequency(&localQpcFreq_);
}

StageCIpcManager::~StageCIpcManager() {
    Shutdown();
    DeleteCriticalSection(&cs_);
}

void StageCIpcManager::Configure(const StageCIpcConfig& cfg) {
    EnterCriticalSection(&cs_);
    if (shared_ && cfg_.mappingName != cfg.mappingName) {
        CloseMappingLocked();
        attachedGeneration_ = 0;
        sessionEverAttached_ = false;
        disconnectedLatched_ = false;
        primaryReaderId_ = 0;
        primaryLastPullUs_ = 0;
        hasPriorPrimaryEnd_ = false;
        lastPrimaryEndFrame_ = 0;
        cursorAnchored_ = false;
        liveReadFrame_ = 0;
        startupSilenceDebtFrames_ = 0;
        fadeInRemaining_ = 0;
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

void StageCIpcManager::Shutdown() {
    EnterCriticalSection(&cs_);
    if (shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicStore32(&shared_->header.consumer_active_readers, 0u);
    }
    CloseMappingLocked();
    activeReaders_.clear();
    lastOpenAttemptUs_ = 0;
    attachedGeneration_ = 0;
    sessionEverAttached_ = false;
    disconnectedLatched_ = false;
    primaryReaderId_ = 0;
    primaryLastPullUs_ = 0;
    hasPriorPrimaryEnd_ = false;
    lastPrimaryEndFrame_ = 0;
    cursorAnchored_ = false;
    liveReadFrame_ = 0;
    startupSilenceDebtFrames_ = 0;
    fadeTotalFrames_ = (cfg_.fadeMs * stagec::kSampleRate) / 1000u;
    fadeInRemaining_ = 0;
    lastOutputL_ = 0;
    lastOutputR_ = 0;
    hadNonZeroAudio_ = false;
    LeaveCriticalSection(&cs_);
}

const StageCIpcConfig& StageCIpcManager::GetConfig() const {
    return cfg_;
}

void StageCIpcManager::NotifyReaderOpened(int32_t readerId) {
    EnterCriticalSection(&cs_);
    if (std::find(activeReaders_.begin(), activeReaders_.end(), readerId) == activeReaders_.end()) {
        activeReaders_.push_back(readerId);
    }
    if (shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicStore32(&shared_->header.consumer_active_readers,
                              static_cast<uint32_t>(activeReaders_.size()));
    }
    LeaveCriticalSection(&cs_);
}

void StageCIpcManager::NotifyReaderEofOrClosed(int32_t readerId) {
    EnterCriticalSection(&cs_);
    auto it = std::find(activeReaders_.begin(), activeReaders_.end(), readerId);
    if (it != activeReaders_.end()) {
        activeReaders_.erase(it);
    }

    if (primaryReaderId_ == readerId) {
        const uint64_t endFrame = liveReadFrame_;
        if (cursorAnchored_) {
            hasPriorPrimaryEnd_ = true;
            lastPrimaryEndFrame_ = liveReadFrame_;
        }
        primaryReaderId_ = 0;
        cursorAnchored_ = false;
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"primary_lease_released\",\"reader_id\":%d,\"end_frame\":%llu",
            readerId, static_cast<unsigned long long>(endFrame));
    }

    if (shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicStore32(&shared_->header.consumer_active_readers,
                              static_cast<uint32_t>(activeReaders_.size()));
    }
    LeaveCriticalSection(&cs_);
}

StageCPullResult StageCIpcManager::PullFramesForReader(int32_t readerId,
                                                       uint64_t sampleSeq,
                                                       uint32_t frameCount,
                                                       int16_t* outInterleavedLR) {
    StageCPullResult res{};
    if (!outInterleavedLR || frameCount == 0) {
        return res;
    }

    EnterCriticalSection(&cs_);

    LARGE_INTEGER nowQpcLi{};
    QueryPerformanceCounter(&nowQpcLi);
    const int64_t nowQpc = nowQpcLi.QuadPart;
    const int64_t nowUs = QpcToMicros(nowQpc, localQpcFreq_.QuadPart);

    const bool mapped = TryEnsureMappingLocked(nowUs);
    uint64_t liveGen = 0;
    const bool rawProducerLive = mapped && IsProducerLiveLocked(&shared_->header, nowQpc, liveGen);

    if (attachedGeneration_ != 0 && !rawProducerLive) {
        const uint64_t prevGen = attachedGeneration_;
        attachedGeneration_ = 0;
        cursorAnchored_ = false;
        if (!cfg_.allowHotReattach) {
            disconnectedLatched_ = true;
        }
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"producer_disconnected\",\"prev_gen\":%llu",
            static_cast<unsigned long long>(prevGen));
    }

    const bool producerLive = rawProducerLive && !disconnectedLatched_;

    if (producerLive && attachedGeneration_ != liveGen) {
        attachedGeneration_ = liveGen;
        sessionEverAttached_ = true;
        cursorAnchored_ = false;
        startupSilenceDebtFrames_ = 0;
        hasPriorPrimaryEnd_ = false;
        fadeTotalFrames_ = (cfg_.fadeMs * stagec::kSampleRate) / 1000u;
        fadeInRemaining_ = fadeTotalFrames_;

        stagec::AtomicAddU64(&shared_->header.session_attach_count, 1ULL);
        stagec::AtomicStore32(&shared_->header.consumer_pid, GetCurrentProcessId());
        const uint64_t sid = stagec::AtomicLoadU64(&shared_->header.session_id);
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"session_attached\",\"gen\":%llu,\"session_id\":%llu",
            static_cast<unsigned long long>(liveGen),
            static_cast<unsigned long long>(sid));
    }

    if (sampleSeq <= static_cast<uint64_t>(stagec::kPreloadBurstSamples)) {
        std::memset(outInterleavedLR, 0, static_cast<size_t>(frameCount) * 2u * sizeof(int16_t));
        if (mapped &&
            stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
            ValidateHeaderStaticFieldsLocked(&shared_->header)) {
            stagec::AtomicAddU64(&shared_->header.undecided_silence_frames, static_cast<uint64_t>(frameCount));
        }
        res.filled = true;
        res.undecidedPreloadSilence = true;
        res.producerOnline = producerLive;
        res.sessionGeneration = attachedGeneration_;
        res.silenceFramesDelivered = frameCount;
        LeaveCriticalSection(&cs_);
        return res;
    }

    // Active playback path (sampleSeq >= 222): primary lease arbitration
    if (primaryReaderId_ != 0 && primaryReaderId_ != readerId) {
        if ((nowUs - primaryLastPullUs_) > 6000000LL) {
            const int32_t oldPrimary = primaryReaderId_;
            if (cursorAnchored_) {
                hasPriorPrimaryEnd_ = true;
                lastPrimaryEndFrame_ = liveReadFrame_;
            }
            primaryReaderId_ = readerId;
            cursorAnchored_ = false;
            AsyncLogger::Instance().LogJsonEvent(
                "StageC_StreamEvent", readerId, "stage_c",
                "\"kind\":\"primary_lease_stale_takeover\",\"old_reader_id\":%d,\"new_reader_id\":%d,\"seq\":%llu",
                oldPrimary, readerId, static_cast<unsigned long long>(sampleSeq));
        } else {
            std::memset(outInterleavedLR, 0, static_cast<size_t>(frameCount) * 2u * sizeof(int16_t));
            res.filled = true;
            res.producerOnline = producerLive;
            res.sessionGeneration = attachedGeneration_;
            res.silenceFramesDelivered = frameCount;
            LeaveCriticalSection(&cs_);
            return res;
        }
    }

    if (primaryReaderId_ == 0) {
        primaryReaderId_ = readerId;
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"primary_lease_claimed\",\"reader_id\":%d,\"seq\":%llu",
            readerId, static_cast<unsigned long long>(sampleSeq));
    }
    primaryLastPullUs_ = nowUs;

    if (!producerLive) {
        EmitDisconnectedOrStartupSilenceLocked(outInterleavedLR, frameCount, true);
        res.filled = true;
        res.producerOnline = false;
        res.sessionGeneration = attachedGeneration_;
        res.silenceFramesDelivered = frameCount;
        LeaveCriticalSection(&cs_);
        return res;
    }

    stagec::StageCIpcRingHeader* hdr = &shared_->header;
    const uint64_t gen1 = attachedGeneration_;
    const uint64_t steadyLeadFrames = (static_cast<uint64_t>(cfg_.steadyLeadMs) * 48000ULL) / 1000ULL;

    uint64_t commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
    const uint64_t reserveBefore = stagec::AtomicLoadU64(&hdr->write_reserve_frame);

    if (!cursorAnchored_) {
        const uint64_t idealPos = (commitBefore >= steadyLeadFrames)
            ? (commitBefore - steadyLeadFrames)
            : 0ULL;

        if (hasPriorPrimaryEnd_) {
            if (idealPos >= lastPrimaryEndFrame_) {
                liveReadFrame_ = idealPos;
                startupSilenceDebtFrames_ = 0;
            } else {
                liveReadFrame_ = std::min<uint64_t>(lastPrimaryEndFrame_, commitBefore);
                const uint64_t availAfterClamp = commitBefore - liveReadFrame_;
                constexpr uint64_t kMinBurst2Frames = 132000ULL; // Covers 111-sample (127,872-frame) Burst 2
                if (availAfterClamp >= kMinBurst2Frames || availAfterClamp >= steadyLeadFrames) {
                    startupSilenceDebtFrames_ = 0;
                } else {
                    startupSilenceDebtFrames_ = steadyLeadFrames - availAfterClamp;
                }
            }
        } else {
            if (commitBefore >= steadyLeadFrames) {
                liveReadFrame_ = idealPos;
                startupSilenceDebtFrames_ = 0;
            } else {
                liveReadFrame_ = 0;
                startupSilenceDebtFrames_ = steadyLeadFrames - commitBefore;
            }
        }
        cursorAnchored_ = true;
        fadeInRemaining_ = fadeTotalFrames_;

        if (hasPriorPrimaryEnd_) {
            const uint64_t skipped = (liveReadFrame_ > lastPrimaryEndFrame_)
                ? (liveReadFrame_ - lastPrimaryEndFrame_)
                : 0ULL;
            stagec::AtomicAddU64(&hdr->preload_resync_events, 1ULL);
            if (skipped > 0ULL) {
                stagec::AtomicAddU64(&hdr->preload_skipped_frames, skipped);
            }
            hasPriorPrimaryEnd_ = false;
            AsyncLogger::Instance().LogJsonEvent(
                "StageC_StreamEvent", readerId, "stage_c",
                "\"kind\":\"preload_resync\",\"reader_id\":%d,\"new_frame\":%llu,\"skipped_frames\":%llu",
                readerId,
                static_cast<unsigned long long>(liveReadFrame_),
                static_cast<unsigned long long>(skipped));
        }
    }

    int16_t* outPtr = outInterleavedLR;
    uint32_t remFrames = frameCount;

    if (startupSilenceDebtFrames_ > 0) {
        const uint32_t debtChunk = static_cast<uint32_t>(
            std::min<uint64_t>(remFrames, startupSilenceDebtFrames_));
        std::memset(outPtr, 0, static_cast<size_t>(debtChunk) * 2u * sizeof(int16_t));
        startupSilenceDebtFrames_ -= debtChunk;
        outPtr += static_cast<size_t>(debtChunk) * 2u;
        remFrames -= debtChunk;
        res.silenceFramesDelivered += debtChunk;

        if (remFrames == 0) {
            const uint64_t wm = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
            const uint64_t frameAgeUs = (wm * 1000000ULL) / 48000ULL;
            stagec::AtomicStoreU64(&hdr->consumer_read_frame, liveReadFrame_);
            stagec::AtomicStoreI64(&hdr->consumer_heartbeat_qpc, nowQpc);
            stagec::AtomicStoreU64(&hdr->watermark_frames, wm);
            stagec::AtomicStoreU64(&hdr->last_frame_age_us, frameAgeUs);
            stagec::AtomicStore32(&hdr->consumer_active_readers,
                                  static_cast<uint32_t>(activeReaders_.size()));

            res.filled = true;
            res.producerOnline = true;
            res.sessionGeneration = gen1;
            res.watermarkFrames = wm;
            res.frameAgeUs = frameAgeUs;
            LeaveCriticalSection(&cs_);
            return res;
        }
    }

    if (reserveBefore > liveReadFrame_ &&
        (reserveBefore - liveReadFrame_) > (stagec::kRingCapacityFrames - stagec::kGuardFrames)) {
        const uint64_t newPos = (commitBefore > steadyLeadFrames) ? (commitBefore - steadyLeadFrames) : 0ULL;
        const uint64_t skipped = (newPos > liveReadFrame_) ? (newPos - liveReadFrame_) : 0ULL;
        liveReadFrame_ = newPos;
        fadeInRemaining_ = fadeTotalFrames_;
        stagec::AtomicAddU64(&hdr->overrun_events, 1ULL);
        if (skipped > 0ULL) {
            stagec::AtomicAddU64(&hdr->overrun_skipped_frames, skipped);
        }
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"overrun_resync\",\"new_frame\":%llu,\"skipped\":%llu",
            static_cast<unsigned long long>(liveReadFrame_),
            static_cast<unsigned long long>(skipped));
    }

    uint32_t toCopy = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        commitBefore = stagec::AtomicLoadU64(&hdr->write_commit_frame);
        const uint64_t avail = (commitBefore > liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
        toCopy = static_cast<uint32_t>(std::min<uint64_t>(remFrames, avail));

        if (toCopy > 0) {
            const uint32_t idx = static_cast<uint32_t>(liveReadFrame_ & stagec::kRingFrameMask);
            const uint32_t firstChunk = std::min<uint32_t>(toCopy, stagec::kRingCapacityFrames - idx);
            const uint32_t secondChunk = toCopy - firstChunk;

            std::memcpy(
                outPtr,
                shared_->pcm_samples + static_cast<size_t>(idx) * 2u,
                static_cast<size_t>(firstChunk) * 2u * sizeof(int16_t));
            if (secondChunk > 0) {
                std::memcpy(
                    outPtr + static_cast<size_t>(firstChunk) * 2u,
                    shared_->pcm_samples,
                    static_cast<size_t>(secondChunk) * 2u * sizeof(int16_t));
            }
        }

        const uint64_t reserveAfter = stagec::AtomicLoadU64(&hdr->write_reserve_frame);
        const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);

        const bool conflict = (toCopy > 0) &&
            ((gen2 != gen1) ||
             (reserveAfter > liveReadFrame_ && (reserveAfter - liveReadFrame_) > stagec::kRingCapacityFrames));

        if (!conflict) {
            break;
        }

        stagec::AtomicAddU64(&hdr->overwrite_conflict_events, 1ULL);
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"overwrite_conflict\",\"attempt\":%d,\"read_start\":%llu,\"reserve_after\":%llu,\"gen1\":%llu,\"gen2\":%llu",
            attempt,
            static_cast<unsigned long long>(liveReadFrame_),
            static_cast<unsigned long long>(reserveAfter),
            static_cast<unsigned long long>(gen1),
            static_cast<unsigned long long>(gen2));

        if (attempt == 0 && gen2 == gen1) {
            const uint64_t latestCommit = stagec::AtomicLoadU64(&hdr->write_commit_frame);
            liveReadFrame_ = (latestCommit > steadyLeadFrames) ? (latestCommit - steadyLeadFrames) : latestCommit;
            fadeInRemaining_ = fadeTotalFrames_;
        } else {
            const bool sameGen = (gen2 == gen1);
            if (!sameGen) {
                attachedGeneration_ = 0;
                cursorAnchored_ = false;
                if (!cfg_.allowHotReattach) {
                    disconnectedLatched_ = true;
                }
            }
            EmitDisconnectedOrStartupSilenceLocked(outPtr, remFrames, true);
            res.filled = true;
            res.producerOnline = sameGen;
            res.sessionGeneration = sameGen ? gen1 : 0ULL;
            res.silenceFramesDelivered += remFrames;
            LeaveCriticalSection(&cs_);
            return res;
        }
    }

    res.liveReadFrameStart = liveReadFrame_;
    res.liveFramesDelivered = toCopy;
    liveReadFrame_ += toCopy;

    if (fadeInRemaining_ > 0 && fadeTotalFrames_ > 0 && toCopy > 0) {
        constexpr double kPi = 3.14159265358979323846;
        const uint32_t fadeCount = std::min<uint32_t>(toCopy, fadeInRemaining_);
        const uint32_t alreadyDone = fadeTotalFrames_ - fadeInRemaining_;
        for (uint32_t i = 0; i < fadeCount; ++i) {
            const double done = static_cast<double>(alreadyDone + i);
            const double gain = 0.5 * (1.0 - std::cos(kPi * (done + 0.5) / static_cast<double>(fadeTotalFrames_)));
            const size_t pos = static_cast<size_t>(i) * 2u;
            const long long lScaled = std::llround(static_cast<double>(outPtr[pos]) * gain);
            const long long rScaled = std::llround(static_cast<double>(outPtr[pos + 1]) * gain);
            outPtr[pos]     = static_cast<int16_t>(std::clamp(lScaled, -32768LL, 32767LL));
            outPtr[pos + 1] = static_cast<int16_t>(std::clamp(rScaled, -32768LL, 32767LL));
        }
        fadeInRemaining_ -= fadeCount;
    }

    if (toCopy < remFrames) {
        const uint32_t missing = remFrames - toCopy;
        if (fadeTotalFrames_ > 0 && toCopy > 0) {
            ApplyFadeOutTail(outPtr, toCopy);
        }
        std::memset(outPtr + static_cast<size_t>(toCopy) * 2u, 0,
                    static_cast<size_t>(missing) * 2u * sizeof(int16_t));
        fadeInRemaining_ = fadeTotalFrames_;
        lastOutputL_ = 0;
        lastOutputR_ = 0;
        hadNonZeroAudio_ = false;
        stagec::AtomicAddU64(&hdr->underrun_events, 1ULL);
        stagec::AtomicAddU64(&hdr->underrun_frames, static_cast<uint64_t>(missing));
        res.silenceFramesDelivered += missing;
        AsyncLogger::Instance().LogJsonEvent(
            "StageC_StreamEvent", readerId, "stage_c",
            "\"kind\":\"underrun\",\"copied\":%u,\"missing\":%u",
            static_cast<unsigned>(toCopy), static_cast<unsigned>(missing));
    } else if (toCopy > 0) {
        const size_t lastPos = static_cast<size_t>(toCopy - 1u) * 2u;
        lastOutputL_ = outPtr[lastPos];
        lastOutputR_ = outPtr[lastPos + 1u];
        hadNonZeroAudio_ = true;
    }

    const uint64_t wm = (commitBefore >= liveReadFrame_) ? (commitBefore - liveReadFrame_) : 0ULL;
    const uint64_t frameAgeUs = (wm * 1000000ULL) / 48000ULL;
    stagec::AtomicStoreU64(&hdr->consumer_read_frame, liveReadFrame_);
    stagec::AtomicStoreI64(&hdr->consumer_heartbeat_qpc, nowQpc);
    stagec::AtomicStoreU64(&hdr->watermark_frames, wm);
    stagec::AtomicStoreU64(&hdr->last_frame_age_us, frameAgeUs);
    stagec::AtomicStore32(&hdr->consumer_active_readers,
                          static_cast<uint32_t>(activeReaders_.size()));

    res.filled = true;
    res.producerOnline = true;
    res.sessionGeneration = gen1;
    res.watermarkFrames = wm;
    res.frameAgeUs = frameAgeUs;

    LeaveCriticalSection(&cs_);
    return res;
}

bool StageCIpcManager::ReadSharedHeaderSnapshot(stagec::StageCIpcRingHeader& outHeader) const {
    std::memset(&outHeader, 0, sizeof(outHeader));
    EnterCriticalSection(&cs_);
    if (!shared_ || mappedBytes_ < stagec::kTotalSharedBytes) {
        LeaveCriticalSection(&cs_);
        return false;
    }
    const stagec::StageCIpcRingHeader* h = &shared_->header;
    outHeader.magic = stagec::AtomicLoad32(&h->magic);
    outHeader.version = stagec::AtomicLoad32(&h->version);
    outHeader.header_bytes = stagec::AtomicLoad32(&h->header_bytes);
    outHeader.total_bytes = stagec::AtomicLoad32(&h->total_bytes);
    outHeader.sample_rate = stagec::AtomicLoad32(&h->sample_rate);
    outHeader.channels = stagec::AtomicLoad32(&h->channels);
    outHeader.bits_per_sample = stagec::AtomicLoad32(&h->bits_per_sample);
    outHeader.capacity_frames = stagec::AtomicLoad32(&h->capacity_frames);
    outHeader.session_generation = stagec::AtomicLoadU64(&h->session_generation);
    outHeader.session_id = stagec::AtomicLoadU64(&h->session_id);
    outHeader.qpc_freq = stagec::AtomicLoadU64(&h->qpc_freq);
    outHeader.producer_pid = stagec::AtomicLoad32(&h->producer_pid);
    outHeader.producer_arch = stagec::AtomicLoad32(&h->producer_arch);

    outHeader.write_reserve_frame = stagec::AtomicLoadU64(&h->write_reserve_frame);
    outHeader.write_commit_frame = stagec::AtomicLoadU64(&h->write_commit_frame);
    outHeader.last_heartbeat_qpc = stagec::AtomicLoadI64(&h->last_heartbeat_qpc);
    outHeader.start_qpc = stagec::AtomicLoadI64(&h->start_qpc);
    outHeader.producer_tick_count = stagec::AtomicLoadU64(&h->producer_tick_count);
    outHeader.producer_state = stagec::AtomicLoad32(&h->producer_state);
    outHeader.pattern_mode = stagec::AtomicLoad32(&h->pattern_mode);

    outHeader.consumer_read_frame = stagec::AtomicLoadU64(&h->consumer_read_frame);
    outHeader.consumer_heartbeat_qpc = stagec::AtomicLoadI64(&h->consumer_heartbeat_qpc);
    outHeader.watermark_frames = stagec::AtomicLoadU64(&h->watermark_frames);
    outHeader.underrun_events = stagec::AtomicLoadU64(&h->underrun_events);
    outHeader.underrun_frames = stagec::AtomicLoadU64(&h->underrun_frames);
    outHeader.overrun_events = stagec::AtomicLoadU64(&h->overrun_events);
    outHeader.overrun_skipped_frames = stagec::AtomicLoadU64(&h->overrun_skipped_frames);
    outHeader.consumer_pid = stagec::AtomicLoad32(&h->consumer_pid);
    outHeader.consumer_active_readers = stagec::AtomicLoad32(&h->consumer_active_readers);

    outHeader.overwrite_conflict_events = stagec::AtomicLoadU64(&h->overwrite_conflict_events);
    outHeader.undecided_silence_frames = stagec::AtomicLoadU64(&h->undecided_silence_frames);
    outHeader.preload_resync_events = stagec::AtomicLoadU64(&h->preload_resync_events);
    outHeader.preload_skipped_frames = stagec::AtomicLoadU64(&h->preload_skipped_frames);
    outHeader.silence_disconnect_frames = stagec::AtomicLoadU64(&h->silence_disconnect_frames);
    outHeader.session_attach_count = stagec::AtomicLoadU64(&h->session_attach_count);
    outHeader.last_frame_age_us = stagec::AtomicLoadU64(&h->last_frame_age_us);
    LeaveCriticalSection(&cs_);
    return true;
}

bool StageCIpcManager::TryEnsureMappingLocked(int64_t nowUs) {
    if (shared_ != nullptr) {
        return true;
    }
    if (cfg_.mappingName.empty()) {
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

    const uint64_t gen = stagec::AtomicLoadU64(&shared_->header.session_generation);
    if (stagec::IsReadyEvenGeneration(gen) && ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicStore32(&shared_->header.consumer_pid, GetCurrentProcessId());
    }
    return true;
}

void StageCIpcManager::CloseMappingLocked() {
    if (shared_) {
        UnmapViewOfFile(shared_);
        shared_ = nullptr;
    }
    if (hMapping_) {
        CloseHandle(hMapping_);
        hMapping_ = nullptr;
    }
    mappedBytes_ = 0;
}

bool StageCIpcManager::ValidateHeaderStaticFieldsLocked(const stagec::StageCIpcRingHeader* hdr) const {
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

bool StageCIpcManager::IsProducerLiveLocked(const stagec::StageCIpcRingHeader* hdr,
                                            int64_t nowQpc,
                                            uint64_t& outGen) const {
    outGen = 0;
    if (!hdr || localQpcFreq_.QuadPart <= 0) {
        return false;
    }

    const uint64_t gen1 = stagec::AtomicLoadU64(&hdr->session_generation);
    if (!stagec::IsReadyEvenGeneration(gen1)) {
        return false;
    }
    if (!ValidateHeaderStaticFieldsLocked(hdr)) {
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
    if (ageTicks > 0) {
        const int64_t ageMs = QpcTicksToMillis(ageTicks, localQpcFreq_.QuadPart);
        if (ageMs > static_cast<int64_t>(cfg_.heartbeatTimeoutMs)) {
            return false;
        }
    }

    const uint64_t gen2 = stagec::AtomicLoadU64(&hdr->session_generation);
    if (gen2 != gen1) {
        return false;
    }

    outGen = gen1;
    return true;
}

void StageCIpcManager::ApplyFadeOutTail(int16_t* buf, uint32_t totalFrames) {
    if (!buf || totalFrames == 0 || fadeTotalFrames_ == 0) {
        return;
    }
    constexpr double kPi = 3.14159265358979323846;
    const uint32_t fadeLen = std::min<uint32_t>(totalFrames, std::min<uint32_t>(240u, fadeTotalFrames_));
    const uint32_t startIdx = totalFrames - fadeLen;
    for (uint32_t i = 0; i < fadeLen; ++i) {
        const double gain = 0.5 * (1.0 + std::cos(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(fadeLen)));
        const size_t pos = static_cast<size_t>(startIdx + i) * 2u;
        const long long lScaled = std::llround(static_cast<double>(buf[pos]) * gain);
        const long long rScaled = std::llround(static_cast<double>(buf[pos + 1u]) * gain);
        buf[pos]      = static_cast<int16_t>(std::clamp(lScaled, -32768LL, 32767LL));
        buf[pos + 1u] = static_cast<int16_t>(std::clamp(rScaled, -32768LL, 32767LL));
    }
}

void StageCIpcManager::EmitDisconnectedOrStartupSilenceLocked(int16_t* outLR,
                                                              uint32_t frameCount,
                                                              bool countAsDisconnect) {
    if (!outLR || frameCount == 0) {
        return;
    }

    if (hadNonZeroAudio_ && fadeTotalFrames_ > 0) {
        constexpr double kPi = 3.14159265358979323846;
        const uint32_t fadeN = std::min<uint32_t>(frameCount, fadeTotalFrames_);
        for (uint32_t i = 0; i < fadeN; ++i) {
            const double gain = 0.5 * (1.0 + std::cos(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(fadeN)));
            const long long lVal = std::llround(static_cast<double>(lastOutputL_) * gain);
            const long long rVal = std::llround(static_cast<double>(lastOutputR_) * gain);
            const size_t pos = static_cast<size_t>(i) * 2u;
            outLR[pos]      = static_cast<int16_t>(std::clamp(lVal, -32768LL, 32767LL));
            outLR[pos + 1u] = static_cast<int16_t>(std::clamp(rVal, -32768LL, 32767LL));
        }
        if (fadeN < frameCount) {
            std::memset(outLR + static_cast<size_t>(fadeN) * 2u, 0,
                        static_cast<size_t>(frameCount - fadeN) * 2u * sizeof(int16_t));
        }
    } else {
        std::memset(outLR, 0, static_cast<size_t>(frameCount) * 2u * sizeof(int16_t));
    }

    hadNonZeroAudio_ = false;
    lastOutputL_ = 0;
    lastOutputR_ = 0;

    if (countAsDisconnect &&
        shared_ &&
        mappedBytes_ >= stagec::kTotalSharedBytes &&
        stagec::IsReadyEvenGeneration(stagec::AtomicLoadU64(&shared_->header.session_generation)) &&
        ValidateHeaderStaticFieldsLocked(&shared_->header)) {
        stagec::AtomicAddU64(&shared_->header.silence_disconnect_frames, static_cast<uint64_t>(frameCount));
    }
}

} // namespace probe
