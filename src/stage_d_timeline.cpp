#include "stage_d_timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace staged {

namespace {

constexpr double kPi = 3.14159265358979323846;

void MulU64To128(uint64_t a, uint64_t b, uint64_t& outHi, uint64_t& outLo) noexcept {
    const uint64_t aLo = a & 0xFFFFFFFFULL;
    const uint64_t aHi = a >> 32;
    const uint64_t bLo = b & 0xFFFFFFFFULL;
    const uint64_t bHi = b >> 32;

    const uint64_t p0 = aLo * bLo;
    const uint64_t p1 = aLo * bHi;
    const uint64_t p2 = aHi * bLo;
    const uint64_t p3 = aHi * bHi;

    const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    outLo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
    outHi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

int64_t SignedDiffU64(uint64_t a, uint64_t b) noexcept {
    if (a >= b) {
        const uint64_t diff = a - b;
        const uint64_t maxPos = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        return (diff >= maxPos) ? std::numeric_limits<int64_t>::max()
                                : static_cast<int64_t>(diff);
    }
    const uint64_t diff = b - a;
    constexpr uint64_t kAbsMin = 1ULL << 63;
    return (diff >= kAbsMin) ? std::numeric_limits<int64_t>::min()
                             : -static_cast<int64_t>(diff);
}

std::string EscapeJsonUtf8(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 16u);
    for (unsigned char c : utf8) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20u) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

} // namespace

// ============================================================================
// Pure Portable C++17 128-bit Overflow-Safe Integer Math & Strict FloorDiv
// ============================================================================

uint64_t MulDivFloorAddRemU64(uint64_t a,
                               uint64_t num,
                               uint64_t addRem,
                               uint64_t den,
                               uint64_t* outRem) noexcept {
    if (den == 0ULL) {
        if (outRem) {
            *outRem = 0ULL;
        }
        return std::numeric_limits<uint64_t>::max();
    }

    uint64_t hi = 0ULL;
    uint64_t lo = 0ULL;
    MulU64To128(a, num, hi, lo);

    const uint64_t loWithRem = lo + addRem;
    if (loWithRem < lo) {
        ++hi;
    }
    lo = loWithRem;

    if (hi >= den) {
        if (outRem) {
            *outRem = 0ULL;
        }
        return std::numeric_limits<uint64_t>::max();
    }

    uint64_t quotient = 0ULL;
    uint64_t remHi = 0ULL;
    uint64_t remLo = hi;

    for (int bit = 63; bit >= 0; --bit) {
        const uint64_t incoming = (lo >> bit) & 1ULL;
        remHi = (remHi << 1) | (remLo >> 63);
        remLo = (remLo << 1) | incoming;

        if (remHi > 0ULL || remLo >= den) {
            const uint64_t prevLo = remLo;
            remLo -= den;
            if (remLo > prevLo) {
                --remHi;
            }
            quotient |= (1ULL << bit);
        }
    }

    if (outRem) {
        *outRem = remLo;
    }
    return quotient;
}

uint64_t MulDivFloorU64(uint64_t a,
                         uint64_t num,
                         uint64_t den,
                         uint64_t* outRem) noexcept {
    return MulDivFloorAddRemU64(a, num, 0ULL, den, outRem);
}

int64_t FloorMulDivI64(int64_t a, uint64_t num, uint64_t den) noexcept {
    if (den == 0ULL) {
        return (a < 0) ? std::numeric_limits<int64_t>::min()
                       : std::numeric_limits<int64_t>::max();
    }
    if (a == 0 || num == 0ULL) {
        return 0LL;
    }
    if (a > 0) {
        const uint64_t q = MulDivFloorU64(static_cast<uint64_t>(a), num, den, nullptr);
        const uint64_t maxPos = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (q >= maxPos) {
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(q);
    }

    const uint64_t ua = 0ULL - static_cast<uint64_t>(a);
    uint64_t rem = 0ULL;
    uint64_t q = MulDivFloorU64(ua, num, den, &rem);
    if (rem > 0ULL) {
        if (q < std::numeric_limits<uint64_t>::max()) {
            ++q;
        }
    }
    constexpr uint64_t kAbsMin = 1ULL << 63;
    if (q >= kAbsMin) {
        return std::numeric_limits<int64_t>::min();
    }
    return -static_cast<int64_t>(q);
}

int64_t QpcTicksTo100ns(int64_t ticks, uint64_t qpcFreq) noexcept {
    return FloorMulDivI64(ticks, 10000000ULL, qpcFreq);
}

// ============================================================================
// TimelineStatsSnapshot Invariant Verification
// ============================================================================

bool TimelineStatsSnapshot::VerifyInvariants(bool requireFlushed) const noexcept {
    if (holdbackFrames > kMaxHoldbackFrames) {
        return false;
    }
    if (requireFlushed && holdbackFrames != 0u) {
        return false;
    }
    if (timelineQueuedFrames != committedRingFrames + static_cast<uint64_t>(holdbackFrames)) {
        return false;
    }
    if (convertedInFrames48k < lateOverlapDroppedFrames) {
        return false;
    }
    const uint64_t expectedQueued =
        (convertedInFrames48k - lateOverlapDroppedFrames) +
        sourceGapZeroFrames +
        starvationZeroFrames;
    if (timelineQueuedFrames != expectedQueued) {
        return false;
    }
    return true;
}

// ============================================================================
// StageDTimelineBridge Implementation
// ============================================================================

StageDTimelineBridge::StageDTimelineBridge(IRingFrameSink& sink,
                                           const TimelineConfig& cfg) noexcept
    : sink_(sink)
    , cfg_(cfg) {
    if (cfg_.qpcGapThreshold100ns == 0ULL) {
        cfg_.qpcGapThreshold100ns = kDefaultQpcGapThreshold100ns;
    }
    if (cfg_.maxTimestampSkew100ns == 0ULL) {
        cfg_.maxTimestampSkew100ns = kDefaultMaxTimestampSkew100ns;
    }
}

void StageDTimelineBridge::Reset(uint64_t originQpc100ns) noexcept {
    originQpc100ns_ = originQpc100ns;
    lastWallQpc100ns_ = originQpc100ns;
    wallElapsedFrames48k_ = 0ULL;

    convertedInFrames48k_ = 0ULL;
    timelineQueuedFrames_ = 0ULL;
    committedRingFrames_ = 0ULL;
    holdbackBuf_.fill(0);
    holdbackFrames_ = 0u;

    lateOverlapDroppedFrames_ = 0ULL;
    sourceGapZeroFrames_ = 0ULL;
    starvationZeroFrames_ = 0ULL;
    timestampFallbackAnchors_ = 0ULL;

    packetCount_ = 0ULL;
    silentPacketCount_ = 0ULL;
    discontinuityCount_ = 0ULL;
    timestampErrorCount_ = 0ULL;
    streamRestartCount_ = 0ULL;
    fadeInCount_ = 0ULL;
    fadeOutCount_ = 0ULL;

    lastPacketQpc100ns_ = 0ULL;
    lastPacketArrivalQpc100ns_ = originQpc100ns;
    lastPacketFrames48k_ = 0u;
    lastPacketEndQpc100ns_ = 0ULL;

    hasStartedAnyStream_ = false;
    streamActive_ = false;
    streamHasFirstPacket_ = false;
    devPosTrackingActive_ = false;
    sourceSampleRate_ = kOutputSampleRate;
    convDelayFrames48k_ = 0u;
    streamCursor48k_ = 0ULL;
    expectedNextDevPos_ = 0ULL;
    devPosGapFracRem_ = 0ULL;
    expectedNextQpc100ns_ = 0ULL;
    qpcPacketFracRem_ = 0ULL;

    audioActive_ = false;
    fadeInRemaining_ = 0u;
}

bool StageDTimelineBridge::PrefillZeroFrames(uint64_t zeroFrames, bool commitImmediately) {
    if (zeroFrames == 0ULL) {
        return true;
    }
    if (packetCount_ > 0ULL) {
        return false;
    }

    EndActiveAudioTail();
    uint64_t emitted = 0ULL;
    const bool ok = EmitZeroFrames(zeroFrames, &emitted);
    sourceGapZeroFrames_ += emitted;
    streamCursor48k_ = timelineQueuedFrames_;
    if (!ok) {
        return false;
    }

    if (commitImmediately && holdbackFrames_ > 0u) {
        if (!sink_.WriteCommittedFrames(committedRingFrames_, holdbackBuf_.data(), holdbackFrames_)) {
            return false;
        }
        committedRingFrames_ += static_cast<uint64_t>(holdbackFrames_);
        holdbackFrames_ = 0u;
    }
    return true;
}

void StageDTimelineBridge::OnStreamStarted(uint32_t sourceSampleRate,
                                           uint32_t convDelayFrames48k) noexcept {
    if (streamActive_) {
        EndActiveAudioTail();
    }
    if (hasStartedAnyStream_) {
        ++streamRestartCount_;
    }
    hasStartedAnyStream_ = true;
    streamActive_ = true;
    streamHasFirstPacket_ = false;
    devPosTrackingActive_ = false;
    sourceSampleRate_ = (sourceSampleRate > 0u) ? sourceSampleRate : kOutputSampleRate;
    convDelayFrames48k_ = convDelayFrames48k;
    expectedNextDevPos_ = 0ULL;
    devPosGapFracRem_ = 0ULL;
    expectedNextQpc100ns_ = 0ULL;
    qpcPacketFracRem_ = 0ULL;
}

void StageDTimelineBridge::OnStreamEnded() noexcept {
    if (streamActive_) {
        streamActive_ = false;
        streamHasFirstPacket_ = false;
        EndActiveAudioTail();
    }
}

void StageDTimelineBridge::UpdateWallClock(uint64_t nowQpc100ns) noexcept {
    if (nowQpc100ns <= lastWallQpc100ns_) {
        return;
    }
    lastWallQpc100ns_ = nowQpc100ns;
    const int64_t elapsed100ns = SignedDiffU64(lastWallQpc100ns_, originQpc100ns_);
    const int64_t elapsedFramesSigned =
        FloorMulDivI64(elapsed100ns, static_cast<uint64_t>(kOutputSampleRate), 10000000ULL);
    if (elapsedFramesSigned > 0) {
        const uint64_t candidate = static_cast<uint64_t>(elapsedFramesSigned);
        if (candidate > wallElapsedFrames48k_) {
            wallElapsedFrames48k_ = candidate;
        }
    }
}

void StageDTimelineBridge::EndActiveAudioTail() noexcept {
    if (!audioActive_) {
        return;
    }
    audioActive_ = false;
    fadeInRemaining_ = 0u;

    if (holdbackFrames_ > 0u) {
        const uint32_t n = holdbackFrames_;
        for (uint32_t i = 0u; i < n; ++i) {
            const double phase = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
            const double gain = 0.5 * (1.0 + std::cos(kPi * phase));
            const size_t idx = static_cast<size_t>(i) * 2u;
            const long long fl = std::llround(static_cast<double>(holdbackBuf_[idx]) * gain);
            const long long fr = std::llround(static_cast<double>(holdbackBuf_[idx + 1u]) * gain);
            holdbackBuf_[idx] = static_cast<int16_t>(std::clamp(fl, -32768LL, 32767LL));
            holdbackBuf_[idx + 1u] = static_cast<int16_t>(std::clamp(fr, -32768LL, 32767LL));
        }
        ++fadeOutCount_;
    }
}

bool StageDTimelineBridge::PushSamplesToHoldbackAndSink(const int16_t* stereoLR, uint32_t frameCount) {
    if (frameCount == 0u) {
        return true;
    }
    if (!stereoLR) {
        return false;
    }

    if (holdbackFrames_ + frameCount <= kMaxHoldbackFrames) {
        std::memcpy(&holdbackBuf_[static_cast<size_t>(holdbackFrames_) * 2u],
                    stereoLR,
                    static_cast<size_t>(frameCount) * 2u * sizeof(int16_t));
        holdbackFrames_ += frameCount;
        timelineQueuedFrames_ += static_cast<uint64_t>(frameCount);
        return true;
    }

    if (frameCount >= kMaxHoldbackFrames) {
        if (holdbackFrames_ > 0u) {
            if (!sink_.WriteCommittedFrames(committedRingFrames_, holdbackBuf_.data(), holdbackFrames_)) {
                return false;
            }
            committedRingFrames_ += static_cast<uint64_t>(holdbackFrames_);
            holdbackFrames_ = 0u;
        }
        const uint32_t directCommit = frameCount - kMaxHoldbackFrames;
        if (directCommit > 0u) {
            if (!sink_.WriteCommittedFrames(committedRingFrames_, stereoLR, directCommit)) {
                return false;
            }
            committedRingFrames_ += static_cast<uint64_t>(directCommit);
        }
        std::memcpy(holdbackBuf_.data(),
                    stereoLR + static_cast<size_t>(directCommit) * 2u,
                    static_cast<size_t>(kMaxHoldbackFrames) * 2u * sizeof(int16_t));
        holdbackFrames_ = kMaxHoldbackFrames;
        timelineQueuedFrames_ += static_cast<uint64_t>(frameCount);
        return true;
    }

    const uint32_t overflow = holdbackFrames_ + frameCount - kMaxHoldbackFrames;
    if (!sink_.WriteCommittedFrames(committedRingFrames_, holdbackBuf_.data(), overflow)) {
        return false;
    }
    committedRingFrames_ += static_cast<uint64_t>(overflow);

    const uint32_t remain = holdbackFrames_ - overflow;
    if (remain > 0u) {
        std::memmove(holdbackBuf_.data(),
                     &holdbackBuf_[static_cast<size_t>(overflow) * 2u],
                     static_cast<size_t>(remain) * 2u * sizeof(int16_t));
    }
    std::memcpy(&holdbackBuf_[static_cast<size_t>(remain) * 2u],
                stereoLR,
                static_cast<size_t>(frameCount) * 2u * sizeof(int16_t));
    holdbackFrames_ = kMaxHoldbackFrames;
    timelineQueuedFrames_ += static_cast<uint64_t>(frameCount);
    return true;
}

bool StageDTimelineBridge::EmitZeroFrames(uint64_t zeroFrames, uint64_t* outEmittedFrames) {
    if (outEmittedFrames) {
        *outEmittedFrames = 0ULL;
    }
    if (zeroFrames == 0ULL) {
        return true;
    }
    constexpr uint32_t kZeroChunkFrames = 4096u;
    int16_t zeros[kZeroChunkFrames * 2u]{};

    uint64_t emitted = 0ULL;
    uint64_t rem = zeroFrames;
    while (rem > 0ULL) {
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(rem, kZeroChunkFrames));
        if (!PushSamplesToHoldbackAndSink(zeros, chunk)) {
            if (outEmittedFrames) {
                *outEmittedFrames = emitted;
            }
            return false;
        }
        emitted += static_cast<uint64_t>(chunk);
        rem -= static_cast<uint64_t>(chunk);
    }
    if (outEmittedFrames) {
        *outEmittedFrames = emitted;
    }
    return true;
}

bool StageDTimelineBridge::EmitActiveFrames(const int16_t* stereoLR,
                                            uint32_t frameCount,
                                            uint32_t* outEmittedFrames) {
    if (outEmittedFrames) {
        *outEmittedFrames = 0u;
    }
    if (frameCount == 0u) {
        return true;
    }
    if (!stereoLR) {
        return false;
    }

    if (!audioActive_) {
        audioActive_ = true;
        fadeInRemaining_ = kFadeFrames;
        ++fadeInCount_;
    }

    uint32_t offset = 0u;
    if (fadeInRemaining_ > 0u) {
        const uint32_t fadeCount = std::min<uint32_t>(fadeInRemaining_, frameCount);
        std::array<int16_t, kFadeFrames * 2u> fadedBuf{};
        const uint32_t doneBase = kFadeFrames - fadeInRemaining_;
        for (uint32_t i = 0u; i < fadeCount; ++i) {
            const uint32_t done = doneBase + i;
            const double phase = (static_cast<double>(done) + 0.5) / static_cast<double>(kFadeFrames);
            const double gain = 0.5 * (1.0 - std::cos(kPi * phase));
            const size_t srcIdx = static_cast<size_t>(i) * 2u;
            const long long fl = std::llround(static_cast<double>(stereoLR[srcIdx]) * gain);
            const long long fr = std::llround(static_cast<double>(stereoLR[srcIdx + 1u]) * gain);
            fadedBuf[srcIdx] = static_cast<int16_t>(std::clamp(fl, -32768LL, 32767LL));
            fadedBuf[srcIdx + 1u] = static_cast<int16_t>(std::clamp(fr, -32768LL, 32767LL));
        }
        if (!PushSamplesToHoldbackAndSink(fadedBuf.data(), fadeCount)) {
            if (outEmittedFrames) {
                *outEmittedFrames = offset;
            }
            return false;
        }
        fadeInRemaining_ -= fadeCount;
        offset += fadeCount;
    }

    if (offset < frameCount) {
        if (!PushSamplesToHoldbackAndSink(stereoLR + static_cast<size_t>(offset) * 2u,
                                          frameCount - offset)) {
            if (outEmittedFrames) {
                *outEmittedFrames = offset;
            }
            return false;
        }
        offset = frameCount;
    }
    if (outEmittedFrames) {
        *outEmittedFrames = offset;
    }
    return true;
}

uint64_t StageDTimelineBridge::AdvanceExpectedQpc(uint64_t baseQpc100ns,
                                                    uint32_t srcFrames,
                                                    uint32_t srcRate) noexcept {
    if (srcFrames == 0u || srcRate == 0u) {
        return baseQpc100ns;
    }
    uint64_t newRem = 0ULL;
    const uint64_t ticks = MulDivFloorAddRemU64(
        static_cast<uint64_t>(srcFrames),
        10000000ULL,
        qpcPacketFracRem_,
        static_cast<uint64_t>(srcRate),
        &newRem);
    qpcPacketFracRem_ = newRem;
    return baseQpc100ns + ticks;
}

bool StageDTimelineBridge::IsQpcTimestampValid(const TimelinePacketInput& pkt,
                                                 uint64_t refNowQpc100ns) const noexcept {
    if (pkt.timestampErrorFlag || pkt.qpcPos100ns == 0ULL) {
        return false;
    }
    if (pkt.qpcPos100ns < originQpc100ns_ &&
        (originQpc100ns_ - pkt.qpcPos100ns) > cfg_.maxTimestampSkew100ns) {
        return false;
    }
    if (refNowQpc100ns > 0ULL) {
        if (pkt.qpcPos100ns > refNowQpc100ns &&
            (pkt.qpcPos100ns - refNowQpc100ns) > cfg_.maxTimestampSkew100ns) {
            return false;
        }
        if (refNowQpc100ns > pkt.qpcPos100ns &&
            (refNowQpc100ns - pkt.qpcPos100ns) > cfg_.maxTimestampSkew100ns) {
            return false;
        }
    }
    return true;
}

int64_t StageDTimelineBridge::ComputeFallbackAnchor48k(uint64_t refNowQpc100ns,
                                                        uint32_t outFrames48k,
                                                        bool isColdStartCandidate) const noexcept {
    const int64_t elapsed100ns = SignedDiffU64(refNowQpc100ns, originQpc100ns_);
    const int64_t wallFrames = FloorMulDivI64(
        elapsed100ns, static_cast<uint64_t>(kOutputSampleRate), 10000000ULL);
    int64_t est = wallFrames - static_cast<int64_t>(outFrames48k) -
                  static_cast<int64_t>(convDelayFrames48k_);

    const int64_t cursorSigned = (streamCursor48k_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(streamCursor48k_);
    if (isColdStartCandidate) {
        if (est < 0) {
            est = 0;
        }
    }
    if (est < cursorSigned) {
        est = cursorSigned;
    }
    return est;
}

bool StageDTimelineBridge::IngestPacket(const TimelinePacketInput& pkt) {
    if (pkt.outFrames48k > 0u && !pkt.silentFlag && !pkt.stereo48kLR) {
        return false;
    }

    if (!streamActive_) {
        OnStreamStarted(kOutputSampleRate, 0u);
    }

    UpdateWallClock(pkt.nowQpc100ns);
    const uint64_t refNow100ns = (pkt.nowQpc100ns >= lastWallQpc100ns_) ? pkt.nowQpc100ns : lastWallQpc100ns_;
    lastPacketArrivalQpc100ns_ = refNow100ns;

    ++packetCount_;
    if (pkt.silentFlag) {
        ++silentPacketCount_;
    }
    if (pkt.discontinuityFlag) {
        ++discontinuityCount_;
    }
    if (pkt.timestampErrorFlag) {
        ++timestampErrorCount_;
    }

    if (pkt.sourceFrames == 0u && pkt.outFrames48k == 0u) {
        if (pkt.discontinuityFlag) {
            streamHasFirstPacket_ = false;
        }
        return true;
    }

    const bool qpcValid = IsQpcTimestampValid(pkt, refNow100ns);
    int64_t targetPkt48k = 0;

    auto computeTargetFromQpcOrFallback = [&](bool isColdStart) -> int64_t {
        if (qpcValid) {
            const int64_t qpcDelta100ns = SignedDiffU64(pkt.qpcPos100ns, originQpc100ns_);
            int64_t t = FloorMulDivI64(
                qpcDelta100ns, static_cast<uint64_t>(kOutputSampleRate), 10000000ULL) -
                static_cast<int64_t>(convDelayFrames48k_);
            if (isColdStart && t < 0 &&
                -t <= static_cast<int64_t>(convDelayFrames48k_)) {
                t = 0;
            }
            qpcPacketFracRem_ = 0ULL;
            expectedNextQpc100ns_ = AdvanceExpectedQpc(
                pkt.qpcPos100ns, pkt.sourceFrames, sourceSampleRate_);
            return t;
        }
        ++timestampFallbackAnchors_;
        const int64_t t = ComputeFallbackAnchor48k(refNow100ns, pkt.outFrames48k, isColdStart);
        qpcPacketFracRem_ = 0ULL;
        expectedNextQpc100ns_ = AdvanceExpectedQpc(
            refNow100ns, pkt.sourceFrames, sourceSampleRate_);
        return t;
    };

    auto computeContinuousQpcOrFallback = [&]() -> int64_t {
        if (!qpcValid) {
            ++timestampFallbackAnchors_;
            const int64_t t = ComputeFallbackAnchor48k(refNow100ns, pkt.outFrames48k, false);
            qpcPacketFracRem_ = 0ULL;
            expectedNextQpc100ns_ = AdvanceExpectedQpc(
                refNow100ns, pkt.sourceFrames, sourceSampleRate_);
            return t;
        }
        const uint64_t qpcDiff100ns = (pkt.qpcPos100ns >= expectedNextQpc100ns_)
            ? (pkt.qpcPos100ns - expectedNextQpc100ns_)
            : (expectedNextQpc100ns_ - pkt.qpcPos100ns);
        int64_t t = 0;
        if (qpcDiff100ns < cfg_.qpcGapThreshold100ns) {
            t = static_cast<int64_t>(streamCursor48k_);
        } else {
            const int64_t qpcDelta100ns = SignedDiffU64(pkt.qpcPos100ns, originQpc100ns_);
            t = FloorMulDivI64(
                qpcDelta100ns, static_cast<uint64_t>(kOutputSampleRate), 10000000ULL) -
                static_cast<int64_t>(convDelayFrames48k_);
        }
        qpcPacketFracRem_ = 0ULL;
        expectedNextQpc100ns_ = AdvanceExpectedQpc(
            pkt.qpcPos100ns, pkt.sourceFrames, sourceSampleRate_);
        return t;
    };

    if (!streamHasFirstPacket_ || pkt.discontinuityFlag) {
        const bool isColdStart =
            (convertedInFrames48k_ == 0ULL && timelineQueuedFrames_ == 0ULL);
        targetPkt48k = computeTargetFromQpcOrFallback(isColdStart);

        if (devPosTrackingActive_) {
            if (pkt.devPos != 0ULL && !pkt.timestampErrorFlag) {
                expectedNextDevPos_ = pkt.devPos + static_cast<uint64_t>(pkt.sourceFrames);
                devPosGapFracRem_ = 0ULL;
            } else {
                expectedNextDevPos_ += static_cast<uint64_t>(pkt.sourceFrames);
            }
        } else {
            if (pkt.devPos != 0ULL && !pkt.timestampErrorFlag) {
                devPosTrackingActive_ = true;
            }
            expectedNextDevPos_ = pkt.devPos + static_cast<uint64_t>(pkt.sourceFrames);
            devPosGapFracRem_ = 0ULL;
        }
    } else if (devPosTrackingActive_) {
        if (pkt.devPos == 0ULL || pkt.timestampErrorFlag || pkt.devPos < expectedNextDevPos_) {
            targetPkt48k = computeContinuousQpcOrFallback();
            expectedNextDevPos_ += static_cast<uint64_t>(pkt.sourceFrames);
        } else if (pkt.devPos == expectedNextDevPos_) {
            targetPkt48k = static_cast<int64_t>(streamCursor48k_);
            expectedNextDevPos_ += static_cast<uint64_t>(pkt.sourceFrames);
            if (qpcValid) {
                qpcPacketFracRem_ = 0ULL;
                expectedNextQpc100ns_ = AdvanceExpectedQpc(
                    pkt.qpcPos100ns, pkt.sourceFrames, sourceSampleRate_);
            }
        } else {
            const uint64_t deltaDev = pkt.devPos - expectedNextDevPos_;
            uint64_t newRem = 0ULL;
            const uint64_t gapFrames48k = MulDivFloorAddRemU64(
                deltaDev,
                static_cast<uint64_t>(kOutputSampleRate),
                devPosGapFracRem_,
                static_cast<uint64_t>(sourceSampleRate_),
                &newRem);
            devPosGapFracRem_ = newRem;
            targetPkt48k = static_cast<int64_t>(streamCursor48k_ + gapFrames48k);
            expectedNextDevPos_ = pkt.devPos + static_cast<uint64_t>(pkt.sourceFrames);
            if (qpcValid) {
                qpcPacketFracRem_ = 0ULL;
                expectedNextQpc100ns_ = AdvanceExpectedQpc(
                    pkt.qpcPos100ns, pkt.sourceFrames, sourceSampleRate_);
            }
        }
    } else if (pkt.devPos != 0ULL) {
        // Transition from devPos == 0 mode to non-zero devPos mid-stream.
        devPosTrackingActive_ = true;
        targetPkt48k = computeContinuousQpcOrFallback();
        expectedNextDevPos_ = pkt.devPos + static_cast<uint64_t>(pkt.sourceFrames);
        devPosGapFracRem_ = 0ULL;
    } else {
        // Real Process Loopback mode: u64DevicePosition == 0 on all packets, track u64QPCPosition.
        targetPkt48k = computeContinuousQpcOrFallback();
    }

    streamHasFirstPacket_ = true;
    if (qpcValid) {
        lastPacketQpc100ns_ = pkt.qpcPos100ns;
        lastPacketFrames48k_ = pkt.outFrames48k;
        const uint64_t endTicks = MulDivFloorU64(
            static_cast<uint64_t>(pkt.sourceFrames),
            10000000ULL,
            static_cast<uint64_t>(sourceSampleRate_),
            nullptr);
        lastPacketEndQpc100ns_ = pkt.qpcPos100ns + endTicks;
    }

    if (pkt.outFrames48k == 0u) {
        return true;
    }

    const uint64_t queuedBefore = timelineQueuedFrames_;
    const bool isGap = (targetPkt48k > 0) && (static_cast<uint64_t>(targetPkt48k) > queuedBefore);

    if (isGap) {
        const uint64_t gapFrames = static_cast<uint64_t>(targetPkt48k) - queuedBefore;
        EndActiveAudioTail();
        uint64_t gapEmitted = 0ULL;
        const bool gapOk = EmitZeroFrames(gapFrames, &gapEmitted);
        sourceGapZeroFrames_ += gapEmitted;
        streamCursor48k_ = timelineQueuedFrames_;
        if (!gapOk) {
            return false;
        }

        if (pkt.silentFlag) {
            EndActiveAudioTail();
            uint64_t pktEmitted = 0ULL;
            const bool pktOk = EmitZeroFrames(pkt.outFrames48k, &pktEmitted);
            convertedInFrames48k_ += pktEmitted;
            streamCursor48k_ = timelineQueuedFrames_;
            return pktOk;
        } else {
            uint32_t pktEmitted = 0u;
            const bool pktOk = EmitActiveFrames(pkt.stereo48kLR, pkt.outFrames48k, &pktEmitted);
            convertedInFrames48k_ += static_cast<uint64_t>(pktEmitted);
            streamCursor48k_ = timelineQueuedFrames_;
            return pktOk;
        }
    }

    uint64_t overlap48k = 0ULL;
    if (targetPkt48k < 0) {
        const uint64_t negOffset = 0ULL - static_cast<uint64_t>(targetPkt48k);
        overlap48k = (queuedBefore <= std::numeric_limits<uint64_t>::max() - negOffset)
            ? (queuedBefore + negOffset)
            : std::numeric_limits<uint64_t>::max();
    } else {
        overlap48k = queuedBefore - static_cast<uint64_t>(targetPkt48k);
    }

    const uint64_t trim = std::min<uint64_t>(static_cast<uint64_t>(pkt.outFrames48k), overlap48k);
    const uint32_t remain = pkt.outFrames48k - static_cast<uint32_t>(trim);

    if (remain > 0u) {
        if (pkt.silentFlag) {
            EndActiveAudioTail();
            uint64_t pktEmitted = 0ULL;
            if (!EmitZeroFrames(remain, &pktEmitted)) {
                if (pktEmitted > 0ULL) {
                    convertedInFrames48k_ += trim + pktEmitted;
                    lateOverlapDroppedFrames_ += trim;
                    streamCursor48k_ = timelineQueuedFrames_;
                }
                return false;
            }
        } else {
            const int16_t* remainPtr = pkt.stereo48kLR + static_cast<size_t>(trim) * 2u;
            uint32_t pktEmitted = 0u;
            if (!EmitActiveFrames(remainPtr, remain, &pktEmitted)) {
                if (pktEmitted > 0u) {
                    convertedInFrames48k_ += trim + static_cast<uint64_t>(pktEmitted);
                    lateOverlapDroppedFrames_ += trim;
                    streamCursor48k_ = timelineQueuedFrames_;
                }
                return false;
            }
        }
    }

    convertedInFrames48k_ += static_cast<uint64_t>(pkt.outFrames48k);
    lateOverlapDroppedFrames_ += trim;
    const int64_t pktEnd48k = targetPkt48k + static_cast<int64_t>(pkt.outFrames48k);
    streamCursor48k_ = (pktEnd48k > 0) ? static_cast<uint64_t>(pktEnd48k) : 0ULL;
    return true;
}

bool StageDTimelineBridge::Tick(uint64_t nowQpc100ns) {
    UpdateWallClock(nowQpc100ns);
    const uint64_t horizonLimit =
        timelineQueuedFrames_ + static_cast<uint64_t>(kStarvationHorizonFrames);
    if (wallElapsedFrames48k_ > horizonLimit) {
        const uint64_t starv = wallElapsedFrames48k_ - horizonLimit;
        EndActiveAudioTail();
        uint64_t emitted = 0ULL;
        const bool ok = EmitZeroFrames(starv, &emitted);
        starvationZeroFrames_ += emitted;
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool StageDTimelineBridge::FlushHoldbackOnStop() {
    EndActiveAudioTail();
    if (holdbackFrames_ > 0u) {
        if (!sink_.WriteCommittedFrames(committedRingFrames_, holdbackBuf_.data(), holdbackFrames_)) {
            return false;
        }
        committedRingFrames_ += static_cast<uint64_t>(holdbackFrames_);
        holdbackFrames_ = 0u;
    }
    return true;
}

TimelineStatsSnapshot StageDTimelineBridge::Snapshot() const noexcept {
    TimelineStatsSnapshot snap;
    snap.originQpc100ns = originQpc100ns_;
    snap.lastWallQpc100ns = lastWallQpc100ns_;
    snap.wallElapsedFrames48k = wallElapsedFrames48k_;

    snap.convertedInFrames48k = convertedInFrames48k_;
    snap.timelineQueuedFrames = timelineQueuedFrames_;
    snap.committedRingFrames = committedRingFrames_;
    snap.holdbackFrames = holdbackFrames_;

    snap.lateOverlapDroppedFrames = lateOverlapDroppedFrames_;
    snap.sourceGapZeroFrames = sourceGapZeroFrames_;
    snap.starvationZeroFrames = starvationZeroFrames_;
    snap.timestampFallbackAnchors = timestampFallbackAnchors_;

    snap.packetCount = packetCount_;
    snap.silentPacketCount = silentPacketCount_;
    snap.discontinuityCount = discontinuityCount_;
    snap.timestampErrorCount = timestampErrorCount_;
    snap.streamRestartCount = streamRestartCount_;
    snap.fadeInCount = fadeInCount_;
    snap.fadeOutCount = fadeOutCount_;

    snap.lastPacketQpc100ns = lastPacketQpc100ns_;
    snap.lastPacketArrivalQpc100ns = lastPacketArrivalQpc100ns_;
    snap.lastPacketFrames48k = lastPacketFrames48k_;
    snap.lastPacketEndQpc100ns = lastPacketEndQpc100ns_;

    snap.streamCursor48k = streamCursor48k_;
    snap.sourceSampleRate = sourceSampleRate_;
    snap.convDelayFrames48k = convDelayFrames48k_;
    snap.streamActive = streamActive_;
    snap.audioActive = audioActive_;
    snap.devPosTrackingActive = devPosTrackingActive_;
    return snap;
}

// ============================================================================
// Telemetry JSONL Formatting
// ============================================================================

std::string FormatTelemetryJsonLine(const TimelineStatsSnapshot& snap,
                                    const TelemetryLineContext& ctx) {
    const uint64_t now100ns = (ctx.nowQpc100ns > 0ULL) ? ctx.nowQpc100ns : snap.lastWallQpc100ns;
    const double elapsedSec = (now100ns >= snap.originQpc100ns)
        ? static_cast<double>(now100ns - snap.originQpc100ns) / 10000000.0
        : -static_cast<double>(snap.originQpc100ns - now100ns) / 10000000.0;

    char buf[2048]{};
    const int n = std::snprintf(
        buf,
        sizeof(buf),
        "{\"kind\":\"%s\",\"now_qpc_100ns\":%llu,\"origin_qpc_100ns\":%llu,"
        "\"last_wall_qpc_100ns\":%llu,\"elapsed_s\":%.6f,"
        "\"target_pid\":%lu,\"target_exe\":\"%s\",\"format\":\"%s\","
        "\"wall_elapsed_frames_48k\":%llu,\"converted_in_frames_48k\":%llu,"
        "\"timeline_queued_frames\":%llu,\"committed_ring_frames\":%llu,"
        "\"holdback_frames\":%u,\"late_overlap_dropped_frames\":%llu,"
        "\"source_gap_zero_frames\":%llu,\"starvation_zero_frames\":%llu,"
        "\"timestamp_fallback_anchors\":%llu,\"packet_count\":%llu,"
        "\"silent_packet_count\":%llu,\"discontinuity_count\":%llu,"
        "\"timestamp_error_count\":%llu,\"stream_restart_count\":%llu,"
        "\"fade_in_count\":%llu,\"fade_out_count\":%llu,"
        "\"last_packet_qpc_100ns\":%llu,\"last_packet_end_qpc_100ns\":%llu,"
        "\"last_packet_frames_48k\":%u,\"last_packet_arrival_qpc_100ns\":%llu,"
        "\"stream_cursor_48k\":%llu,\"source_sample_rate\":%u,"
        "\"conv_delay_frames_48k\":%u,\"stream_active\":%s,"
        "\"audio_active\":%s,\"dev_pos_tracking_active\":%s,"
        "\"ipc_session_generation\":%llu,\"ipc_producer_state\":%u,"
        "\"ipc_write_reserve_frame\":%llu,\"ipc_write_commit_frame\":%llu,"
        "\"ipc_consumer_read_frame\":%llu,\"ipc_watermark_frames\":%llu,"
        "\"ipc_underrun_events\":%llu,\"ipc_underrun_frames\":%llu,"
        "\"ipc_overrun_events\":%llu,\"ipc_overrun_skipped_frames\":%llu,"
        "\"ipc_overwrite_conflict_events\":%llu,\"ipc_last_frame_age_us\":%llu,"
        "\"ipc_consumer_active_readers\":%u}",
        EscapeJsonUtf8(ctx.kind ? ctx.kind : "periodic").c_str(),
        static_cast<unsigned long long>(now100ns),
        static_cast<unsigned long long>(snap.originQpc100ns),
        static_cast<unsigned long long>(snap.lastWallQpc100ns),
        elapsedSec,
        static_cast<unsigned long>(ctx.targetPid),
        EscapeJsonUtf8(ctx.targetExeUtf8).c_str(),
        EscapeJsonUtf8(ctx.formatLabelUtf8).c_str(),
        static_cast<unsigned long long>(snap.wallElapsedFrames48k),
        static_cast<unsigned long long>(snap.convertedInFrames48k),
        static_cast<unsigned long long>(snap.timelineQueuedFrames),
        static_cast<unsigned long long>(snap.committedRingFrames),
        static_cast<unsigned>(snap.holdbackFrames),
        static_cast<unsigned long long>(snap.lateOverlapDroppedFrames),
        static_cast<unsigned long long>(snap.sourceGapZeroFrames),
        static_cast<unsigned long long>(snap.starvationZeroFrames),
        static_cast<unsigned long long>(snap.timestampFallbackAnchors),
        static_cast<unsigned long long>(snap.packetCount),
        static_cast<unsigned long long>(snap.silentPacketCount),
        static_cast<unsigned long long>(snap.discontinuityCount),
        static_cast<unsigned long long>(snap.timestampErrorCount),
        static_cast<unsigned long long>(snap.streamRestartCount),
        static_cast<unsigned long long>(snap.fadeInCount),
        static_cast<unsigned long long>(snap.fadeOutCount),
        static_cast<unsigned long long>(snap.lastPacketQpc100ns),
        static_cast<unsigned long long>(snap.lastPacketEndQpc100ns),
        static_cast<unsigned>(snap.lastPacketFrames48k),
        static_cast<unsigned long long>(snap.lastPacketArrivalQpc100ns),
        static_cast<unsigned long long>(snap.streamCursor48k),
        static_cast<unsigned>(snap.sourceSampleRate),
        static_cast<unsigned>(snap.convDelayFrames48k),
        snap.streamActive ? "true" : "false",
        snap.audioActive ? "true" : "false",
        snap.devPosTrackingActive ? "true" : "false",
        static_cast<unsigned long long>(ctx.ipc.sessionGeneration),
        static_cast<unsigned>(ctx.ipc.producerState),
        static_cast<unsigned long long>(ctx.ipc.writeReserveFrame),
        static_cast<unsigned long long>(ctx.ipc.writeCommitFrame),
        static_cast<unsigned long long>(ctx.ipc.consumerReadFrame),
        static_cast<unsigned long long>(ctx.ipc.watermarkFrames),
        static_cast<unsigned long long>(ctx.ipc.underrunEvents),
        static_cast<unsigned long long>(ctx.ipc.underrunFrames),
        static_cast<unsigned long long>(ctx.ipc.overrunEvents),
        static_cast<unsigned long long>(ctx.ipc.overrunSkippedFrames),
        static_cast<unsigned long long>(ctx.ipc.overwriteConflictEvents),
        static_cast<unsigned long long>(ctx.ipc.lastFrameAgeUs),
        static_cast<unsigned>(ctx.ipc.consumerActiveReaders));

    if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
        return std::string(buf, static_cast<size_t>(n));
    }

    // Fallback dynamic allocation if strings are unusually long
    std::string fallback;
    fallback.reserve(4096u);
    int cap = 4096;
    for (int attempt = 0; attempt < 3; ++attempt) {
        fallback.resize(static_cast<size_t>(cap));
        const int written = std::snprintf(
            fallback.data(),
            fallback.size(),
            "{\"kind\":\"%s\",\"now_qpc_100ns\":%llu,\"origin_qpc_100ns\":%llu,"
            "\"last_wall_qpc_100ns\":%llu,\"elapsed_s\":%.6f,"
            "\"target_pid\":%lu,\"target_exe\":\"%s\",\"format\":\"%s\","
            "\"wall_elapsed_frames_48k\":%llu,\"converted_in_frames_48k\":%llu,"
            "\"timeline_queued_frames\":%llu,\"committed_ring_frames\":%llu,"
            "\"holdback_frames\":%u,\"late_overlap_dropped_frames\":%llu,"
            "\"source_gap_zero_frames\":%llu,\"starvation_zero_frames\":%llu,"
            "\"timestamp_fallback_anchors\":%llu,\"packet_count\":%llu,"
            "\"silent_packet_count\":%llu,\"discontinuity_count\":%llu,"
            "\"timestamp_error_count\":%llu,\"stream_restart_count\":%llu,"
            "\"fade_in_count\":%llu,\"fade_out_count\":%llu,"
            "\"last_packet_qpc_100ns\":%llu,\"last_packet_end_qpc_100ns\":%llu,"
            "\"last_packet_frames_48k\":%u,\"last_packet_arrival_qpc_100ns\":%llu,"
            "\"stream_cursor_48k\":%llu,\"source_sample_rate\":%u,"
            "\"conv_delay_frames_48k\":%u,\"stream_active\":%s,"
            "\"audio_active\":%s,\"dev_pos_tracking_active\":%s,"
            "\"ipc_session_generation\":%llu,\"ipc_producer_state\":%u,"
            "\"ipc_write_reserve_frame\":%llu,\"ipc_write_commit_frame\":%llu,"
            "\"ipc_consumer_read_frame\":%llu,\"ipc_watermark_frames\":%llu,"
            "\"ipc_underrun_events\":%llu,\"ipc_underrun_frames\":%llu,"
            "\"ipc_overrun_events\":%llu,\"ipc_overrun_skipped_frames\":%llu,"
            "\"ipc_overwrite_conflict_events\":%llu,\"ipc_last_frame_age_us\":%llu,"
            "\"ipc_consumer_active_readers\":%u}",
            EscapeJsonUtf8(ctx.kind ? ctx.kind : "periodic").c_str(),
            static_cast<unsigned long long>(now100ns),
            static_cast<unsigned long long>(snap.originQpc100ns),
            static_cast<unsigned long long>(snap.lastWallQpc100ns),
            elapsedSec,
            static_cast<unsigned long>(ctx.targetPid),
            EscapeJsonUtf8(ctx.targetExeUtf8).c_str(),
            EscapeJsonUtf8(ctx.formatLabelUtf8).c_str(),
            static_cast<unsigned long long>(snap.wallElapsedFrames48k),
            static_cast<unsigned long long>(snap.convertedInFrames48k),
            static_cast<unsigned long long>(snap.timelineQueuedFrames),
            static_cast<unsigned long long>(snap.committedRingFrames),
            static_cast<unsigned>(snap.holdbackFrames),
            static_cast<unsigned long long>(snap.lateOverlapDroppedFrames),
            static_cast<unsigned long long>(snap.sourceGapZeroFrames),
            static_cast<unsigned long long>(snap.starvationZeroFrames),
            static_cast<unsigned long long>(snap.timestampFallbackAnchors),
            static_cast<unsigned long long>(snap.packetCount),
            static_cast<unsigned long long>(snap.silentPacketCount),
            static_cast<unsigned long long>(snap.discontinuityCount),
            static_cast<unsigned long long>(snap.timestampErrorCount),
            static_cast<unsigned long long>(snap.streamRestartCount),
            static_cast<unsigned long long>(snap.fadeInCount),
            static_cast<unsigned long long>(snap.fadeOutCount),
            static_cast<unsigned long long>(snap.lastPacketQpc100ns),
            static_cast<unsigned long long>(snap.lastPacketEndQpc100ns),
            static_cast<unsigned>(snap.lastPacketFrames48k),
            static_cast<unsigned long long>(snap.lastPacketArrivalQpc100ns),
            static_cast<unsigned long long>(snap.streamCursor48k),
            static_cast<unsigned>(snap.sourceSampleRate),
            static_cast<unsigned>(snap.convDelayFrames48k),
            snap.streamActive ? "true" : "false",
            snap.audioActive ? "true" : "false",
            snap.devPosTrackingActive ? "true" : "false",
            static_cast<unsigned long long>(ctx.ipc.sessionGeneration),
            static_cast<unsigned>(ctx.ipc.producerState),
            static_cast<unsigned long long>(ctx.ipc.writeReserveFrame),
            static_cast<unsigned long long>(ctx.ipc.writeCommitFrame),
            static_cast<unsigned long long>(ctx.ipc.consumerReadFrame),
            static_cast<unsigned long long>(ctx.ipc.watermarkFrames),
            static_cast<unsigned long long>(ctx.ipc.underrunEvents),
            static_cast<unsigned long long>(ctx.ipc.underrunFrames),
            static_cast<unsigned long long>(ctx.ipc.overrunEvents),
            static_cast<unsigned long long>(ctx.ipc.overrunSkippedFrames),
            static_cast<unsigned long long>(ctx.ipc.overwriteConflictEvents),
            static_cast<unsigned long long>(ctx.ipc.lastFrameAgeUs),
            static_cast<unsigned>(ctx.ipc.consumerActiveReaders));
        if (written > 0 && static_cast<size_t>(written) < fallback.size()) {
            fallback.resize(static_cast<size_t>(written));
            return fallback;
        }
        cap *= 2;
    }
    return fallback;
}

} // namespace staged
