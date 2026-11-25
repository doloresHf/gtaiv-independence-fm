#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "ipc_ring_protocol.h"
#include "route_b_coordinator.h"
#include "route_b_ctrl_protocol.h"
#include "route_b_ipc_consumer.h"
#include "route_b_offsets.h"
#include "route_b_smtc.h"

namespace {

int g_failures = 0;

void CheckCond(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] Line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) CheckCond(static_cast<bool>(expr), #expr, __LINE__)

void InitTestSharedBlock(stagec::StageCIpcSharedBlock& blk,
                         uint64_t sessionGen,
                         int64_t qpcNow,
                         int64_t qpcFreq) {
    std::memset(&blk, 0, sizeof(blk));
    blk.header.magic              = stagec::kIpcMagic;
    blk.header.version            = stagec::kIpcVersion;
    blk.header.header_bytes       = stagec::kHeaderBytes;
    blk.header.total_bytes        = stagec::kTotalSharedBytes;
    blk.header.sample_rate        = stagec::kSampleRate;
    blk.header.channels           = stagec::kChannels;
    blk.header.bits_per_sample    = stagec::kBitsPerSample;
    blk.header.capacity_frames    = stagec::kRingCapacityFrames;
    blk.header.qpc_freq           = static_cast<uint64_t>(qpcFreq);
    blk.header.producer_pid       = GetCurrentProcessId();
    blk.header.producer_arch      = (sizeof(void*) == 8) ? 64u : 32u;
    blk.header.session_id         = 100ULL + sessionGen;
    blk.header.producer_state     = static_cast<uint32_t>(stagec::ProducerState::Streaming);
    blk.header.pattern_mode       = static_cast<uint32_t>(stagec::ProducerPattern::FrameCode);
    blk.header.last_heartbeat_qpc = qpcNow;
    blk.header.start_qpc          = qpcNow;
    blk.header.session_generation = sessionGen;
}

void WriteFrameCodeSpan(stagec::StageCIpcSharedBlock& blk,
                        uint64_t startFrame,
                        uint64_t frameCount,
                        uint64_t sessionGen) {
    stagec::AtomicStoreU64(&blk.header.write_reserve_frame, startFrame + frameCount);
    for (uint64_t i = 0ULL; i < frameCount; ++i) {
        const uint64_t frameIdx = startFrame + i;
        const uint32_t slot = static_cast<uint32_t>(frameIdx & stagec::kRingFrameMask);
        int16_t l = 0;
        int16_t r = 0;
        stagec::EncodeFrameCode(frameIdx, sessionGen, l, r);
        blk.pcm_samples[slot * 2u]      = l;
        blk.pcm_samples[slot * 2u + 1u] = r;
    }
    stagec::AtomicStoreU64(&blk.header.write_commit_frame, startFrame + frameCount);
}

void ReadPlanarSample(const void* blockPtr, uint32_t blockBytes, uint32_t frameIdx, int16_t& outL, int16_t& outR) {
    const auto* base = static_cast<const uint8_t*>(blockPtr);
    const auto* inL  = reinterpret_cast<const int16_t*>(base);
    const auto* inR  = reinterpret_cast<const int16_t*>(base + (blockBytes >> 1));
    outL = inL[frameIdx];
    outR = inR[frameIdx];
}

void RunSuite1CtrlProtocolLayout() {
    std::printf("  [Suite 1] Cross-bitness Control SHM layout & atomics...\n");
    CHECK(sizeof(routeb::RadioBridgeCtrlBlock) == 512u);
    CHECK(alignof(routeb::RadioBridgeCtrlBlock) == 64u);

    auto blk = std::make_unique<routeb::RadioBridgeCtrlBlock>();
    std::memset(blk.get(), 0, sizeof(*blk));
    routeb::AtomicStore32(&blk->magic, routeb::kCtrlMagic);
    routeb::AtomicStoreI32(&blk->pending_skip_dir, -1);
    routeb::AtomicStore32(&blk->user_slot_play_seq, 7u);
    routeb::AtomicStore32(&blk->station_crossfading, 1u);
    routeb::AtomicStore32(&blk->esc_splice_seq, 3u);
    routeb::AtomicStoreU64(&blk->preroll_start_frame, 0x0123456789ABCDEFULL);
    routeb::AtomicStoreU64(&blk->esc_splice_start_frame, 100000ULL);
    routeb::AtomicStoreU64(&blk->esc_splice_end_frame, 250000ULL);
    routeb::AtomicStoreI64(&blk->smtc_position_ms, -42LL);

    CHECK(routeb::AtomicLoad32(&blk->magic) == routeb::kCtrlMagic);
    CHECK(routeb::AtomicLoadI32(&blk->pending_skip_dir) == -1);
    CHECK(routeb::AtomicLoad32(&blk->user_slot_play_seq) == 7u);
    CHECK(routeb::AtomicLoad32(&blk->station_crossfading) == 1u);
    CHECK(routeb::AtomicLoad32(&blk->esc_splice_seq) == 3u);
    CHECK(routeb::AtomicLoadU64(&blk->preroll_start_frame) == 0x0123456789ABCDEFULL);
    CHECK(routeb::AtomicLoadU64(&blk->esc_splice_start_frame) == 100000ULL);
    CHECK(routeb::AtomicLoadU64(&blk->esc_splice_end_frame) == 250000ULL);
    CHECK(routeb::AtomicLoadI64(&blk->smtc_position_ms) == -42LL);
    CHECK(offsetof(routeb::RadioBridgeCtrlBlock, user_slot_play_seq) == 0x0B0u);
    CHECK(offsetof(routeb::RadioBridgeCtrlBlock, station_crossfading) == 0x0B4u);
    CHECK(offsetof(routeb::RadioBridgeCtrlBlock, esc_splice_seq) == 0x158u);
    CHECK(offsetof(routeb::RadioBridgeCtrlBlock, esc_splice_start_frame) == 0x160u);
    CHECK(offsetof(routeb::RadioBridgeCtrlBlock, esc_splice_end_frame) == 0x168u);
}

void RunSuite2OffsetsAndSignatures() {
    std::printf("  [Suite 2] ASLR-safe RVA + masked prologue signature resolution...\n");

    size_t count = 0u;
    const routeb::SignatureSpec* specs = routeb::GetGtaIvSignatureSpecs(&count);
    CHECK(count == 8u);
    CHECK(specs != nullptr);

    // Allocate synthetic PE image buffer covering the highest global RVA
    constexpr size_t kSyntheticImageSize = 0x00F00000u;
    auto image = std::make_unique<uint8_t[]>(kSyntheticImageSize);
    std::memset(image.get(), 0xCC, kSyntheticImageSize);

    for (size_t i = 0u; i < count; ++i) {
        std::memcpy(image.get() + specs[i].rva, specs[i].pattern, specs[i].patternLen);
    }

    const uintptr_t synthBase = reinterpret_cast<uintptr_t>(image.get());
    routeb::ResolvedTargets res{};
    CHECK(routeb::ResolveGtaIvTargets(synthBase, kSyntheticImageSize, res));
    CHECK(res.allMatched);
    for (size_t i = 0u; i < count; ++i) {
        CHECK(res.entries[i].matched);
        CHECK(res.entries[i].address == synthBase + specs[i].rva);
    }
    CHECK(res.globalRadioMode == synthBase + routeb::kRvaGlobalRadioMode);
    CHECK(res.globalAudioTimeMs == synthBase + routeb::kRvaGlobalAudioTimeMs);

    // Mutate ASLR-relocated wildcard bytes [1..4] of GetUserRadioMode -> must still pass
    const uint32_t getModeRva = specs[static_cast<size_t>(routeb::TargetId::GetUserRadioMode)].rva;
    image[getModeRva + 1u] = 0x78u;
    image[getModeRva + 2u] = 0x56u;
    image[getModeRva + 3u] = 0x34u;
    image[getModeRva + 4u] = 0x12u;
    CHECK(routeb::ResolveGtaIvTargets(synthBase, kSyntheticImageSize, res));
    CHECK(res.allMatched);

    // Tamper 1 non-wildcard byte in AudRadioSlot_Play -> safety fuse MUST trip
    const uint32_t playRva = specs[static_cast<size_t>(routeb::TargetId::AudRadioSlotPlay)].rva;
    const uint8_t savedByte = image[playRva + 5u];
    image[playRva + 5u] ^= 0xFFu;
    CHECK(!routeb::ResolveGtaIvTargets(synthBase, kSyntheticImageSize, res));
    CHECK(!res.allMatched);
    CHECK(!res.entries[static_cast<size_t>(routeb::TargetId::AudRadioSlotPlay)].matched);
    CHECK(res.entries[static_cast<size_t>(routeb::TargetId::AudRadioSlotPlay)].address == 0u);
    CHECK(res.entries[static_cast<size_t>(routeb::TargetId::AudRadioStationUpdate)].matched);
    image[playRva + 5u] = savedByte;

    // Out-of-bounds image size MUST fail cleanly
    CHECK(!routeb::ResolveGtaIvTargets(synthBase, 0x00100000u, res));
    CHECK(!res.allMatched);
}

void RunSuite3PlanarFraming() {
    std::printf("  [Suite 3] Planar L/R block layout & tail-zeroing verification...\n");
    CHECK(routeb::ComputeBlockFrameCount(508128u) == 127032u);
    CHECK(routeb::ComputeBlockFrameCount(522240u) == 130560u);
    CHECK(routeb::ComputeBlockFrameCount(102u) == 24u);

    constexpr uint32_t kOddBlockBytes = 102u; // 24 usable frames (48 B per plane in 51 B half-planes)
    const uint32_t frames = routeb::ComputeBlockFrameCount(kOddBlockBytes);
    CHECK(frames == 24u);

    std::vector<int16_t> interleaved(static_cast<size_t>(frames) * 2u);
    for (uint32_t i = 0u; i < frames; ++i) {
        interleaved[i * 2u]      = static_cast<int16_t>(100 + static_cast<int32_t>(i));
        interleaved[i * 2u + 1u] = static_cast<int16_t>(-200 - static_cast<int32_t>(i));
    }

    std::vector<uint8_t> block(kOddBlockBytes, 0xAAu);
    CHECK(routeb::WriteInterleavedToPlanarBlock(block.data(), kOddBlockBytes, interleaved.data(), frames));

    for (uint32_t i = 0u; i < frames; ++i) {
        int16_t l = 0;
        int16_t r = 0;
        ReadPlanarSample(block.data(), kOddBlockBytes, i, l, r);
        CHECK(l == static_cast<int16_t>(100 + static_cast<int32_t>(i)));
        CHECK(r == static_cast<int16_t>(-200 - static_cast<int32_t>(i)));
    }
    // Verify tail bytes in L gap [48..51) and R gap [99..102) were zeroed
    CHECK(block[48] == 0u && block[49] == 0u && block[50] == 0u);
    CHECK(block[99] == 0u && block[100] == 0u && block[101] == 0u);
}

void RunSuite4ConsumerPlanarHandoff() {
    std::printf("  [Suite 4] RouteBIpcConsumer planar steady + Play() zero-gap + short-ident + timeout...\n");

    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    auto shm = std::make_unique<stagec::StageCIpcSharedBlock>();
    InitTestSharedBlock(*shm, 2ULL, now.QuadPart, freq.QuadPart);

    // Real GTA IV carrier block size: 508,128 bytes = 127,032 stereo frames (~2.6465s per block)
    constexpr uint32_t kBlockBytes     = 508128u;
    constexpr uint32_t kFramesPerBlock = routeb::ComputeBlockFrameCount(kBlockBytes);
    CHECK(kFramesPerBlock == 127032u);

    auto blk0 = std::make_unique<uint8_t[]>(kBlockBytes);
    auto blk1 = std::make_unique<uint8_t[]>(kBlockBytes);
    auto blk2 = std::make_unique<uint8_t[]>(kBlockBytes);

    routeb::RouteBIpcConsumer consumer;
    routeb::RouteBConsumerConfig cfg{};
    cfg.steadyLeadMs       = 3500u;
    cfg.heartbeatTimeoutMs = 500u;
    cfg.fadeMs             = 0u; // Bit-exact FrameCode verification
    cfg.allowHotReattach   = true;
    consumer.Configure(cfg);
    consumer.AttachExternalBlockForTest(shm.get(), sizeof(*shm));

    // (4a) Pre-roll 9.0s = 432,000 frames starting at frame 48,000 -> enough for Block0 + Block1 + Burst2 reserve!
    constexpr uint64_t kPrerollStart = 48000ULL;
    WriteFrameCodeSpan(*shm, 0ULL, kPrerollStart + 432000ULL, 2ULL);

    uint64_t endFrame = 0ULL;
    const routeb::RouteBPullStatus stPlay = consumer.FillPreloadedBlocksAtPlay(
        blk0.get(), blk1.get(), kBlockBytes, true, kPrerollStart, kFramesPerBlock, &endFrame);
    CHECK(stPlay.ok);
    CHECK(stPlay.producerOnline);
    CHECK(stPlay.usedPrerollAnchor);
    CHECK(stPlay.liveFramesDelivered == kFramesPerBlock * 2u);
    CHECK(stPlay.silenceFramesDelivered == 0u);
    CHECK(endFrame == kPrerollStart + kFramesPerBlock * 2u);

    for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
        int16_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
        uint64_t dec0 = 0ULL, dec1 = 0ULL;
        ReadPlanarSample(blk0.get(), kBlockBytes, i, l0, r0);
        ReadPlanarSample(blk1.get(), kBlockBytes, i, l1, r1);
        CHECK(stagec::DecodeFrameCode(l0, r0, 2ULL, dec0));
        CHECK(stagec::DecodeFrameCode(l1, r1, 2ULL, dec1));
        CHECK(dec0 == ((kPrerollStart + i) % stagec::kFrameCodeMod));
        CHECK(dec1 == ((kPrerollStart + kFramesPerBlock + i) % stagec::kFrameCodeMod));
    }

    // Immediate Burst 2 steady refill -> must seamlessly continue at kPrerollStart + 2*kFramesPerBlock
    const routeb::RouteBPullStatus stSteady = consumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
    CHECK(stSteady.ok);
    CHECK(stSteady.producerOnline);
    CHECK(stSteady.liveFramesDelivered == kFramesPerBlock);
    CHECK(stSteady.silenceFramesDelivered == 0u);
    CHECK(stSteady.readStartFrame == kPrerollStart + kFramesPerBlock * 2u);
    CHECK(endFrame == kPrerollStart + kFramesPerBlock * 3u);

    for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
        int16_t l2 = 0, r2 = 0;
        uint64_t dec2 = 0ULL;
        ReadPlanarSample(blk2.get(), kBlockBytes, i, l2, r2);
        CHECK(stagec::DecodeFrameCode(l2, r2, 2ULL, dec2));
        CHECK(dec2 == ((kPrerollStart + kFramesPerBlock * 2u + i) % stagec::kFrameCodeMod));
    }

    // (4b) Short <5.4s ident right-aligned padding test:
    // Available = 190,000 frames, reserveForBurst2 = 127,032 -> usable intro = 62,968 frames (no clipped note 0:00.0!)
    constexpr uint64_t kShortPrerollStart = 600000ULL;
    InitTestSharedBlock(*shm, 4ULL, now.QuadPart, freq.QuadPart);
    WriteFrameCodeSpan(*shm, 0ULL, kShortPrerollStart + 190000ULL, 4ULL);
    consumer.ResetCursor();

    const routeb::RouteBPullStatus stShort = consumer.FillPreloadedBlocksAtPlay(
        blk0.get(), blk1.get(), kBlockBytes, true, kShortPrerollStart, kFramesPerBlock, &endFrame);
    CHECK(stShort.ok);
    CHECK(stShort.producerOnline);
    CHECK(stShort.liveFramesDelivered == 62968u);
    CHECK(stShort.silenceFramesDelivered == (kFramesPerBlock * 2u - 62968u));
    CHECK(endFrame == kShortPrerollStart + 62968u);

    // Check head is pure zero silence, and tail [pad .. 2*kFramesPerBlock) is exact intro [600000 .. +62968)
    const uint32_t pad = kFramesPerBlock * 2u - 62968u;
    for (uint32_t i = 0u; i < kFramesPerBlock * 2u; ++i) {
        int16_t l = 0, r = 0;
        if (i < kFramesPerBlock) {
            ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
        } else {
            ReadPlanarSample(blk1.get(), kBlockBytes, i - kFramesPerBlock, l, r);
        }
        if (i < pad) {
            CHECK(l == 0 && r == 0);
        } else {
            uint64_t dec = 0ULL;
            CHECK(stagec::DecodeFrameCode(l, r, 4ULL, dec));
            CHECK(dec == ((kShortPrerollStart + (i - pad)) % stagec::kFrameCodeMod));
        }
    }

    // Subsequent Burst 2 pulls remaining 127,032 frames cleanly with ZERO underrun!
    const routeb::RouteBPullStatus stShortNext = consumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
    CHECK(stShortNext.ok);
    CHECK(stShortNext.liveFramesDelivered == kFramesPerBlock);
    CHECK(stShortNext.silenceFramesDelivered == 0u);
    CHECK(stShortNext.readStartFrame == kShortPrerollStart + 62968u);

    // (4c) Producer timeout / offline fade-out + hot reattach
    cfg.fadeMs = 10u;
    consumer.Configure(cfg);
    // Simulate stale heartbeat (2 seconds old)
    stagec::AtomicStoreI64(&shm->header.last_heartbeat_qpc, now.QuadPart - freq.QuadPart * 2LL);
    const routeb::RouteBPullStatus stOffline = consumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
    CHECK(stOffline.ok);
    CHECK(!stOffline.producerOnline);
    CHECK(stOffline.silenceFramesDelivered == kFramesPerBlock);
    // Tail after 480-frame cosine fade MUST be exact silence
    for (uint32_t i = 1000u; i < kFramesPerBlock; ++i) {
        int16_t l = 0, r = 0;
        ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
        CHECK(l == 0 && r == 0);
    }

    // Producer restarts at generation 6 -> consumer must hot-reattach cleanly
    cfg.fadeMs = 0u;
    consumer.Configure(cfg);
    InitTestSharedBlock(*shm, 6ULL, now.QuadPart, freq.QuadPart);
    WriteFrameCodeSpan(*shm, 0ULL, 300000ULL, 6ULL);
    const routeb::RouteBPullStatus stReattach = consumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
    CHECK(stReattach.ok);
    CHECK(stReattach.producerOnline);
    CHECK(stReattach.sessionGeneration == 6ULL);
    CHECK(stReattach.liveFramesDelivered == kFramesPerBlock);
    int16_t lrFirstL = 0, lrFirstR = 0;
    uint64_t decRe = 0ULL;
    ReadPlanarSample(blk0.get(), kBlockBytes, 0u, lrFirstL, lrFirstR);
    CHECK(stagec::DecodeFrameCode(lrFirstL, lrFirstR, 6ULL, decRe));

    // (4d) ESC silence-hole splice (QueueSkipGap): in-window seamless cut & lapped fast-forward
    {
        QueryPerformanceCounter(&now);
        auto gapShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*gapShm, 8ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*gapShm, 0ULL, 750000ULL, 8ULL);

        routeb::RouteBIpcConsumer gapConsumer;
        routeb::RouteBConsumerConfig gapCfg{};
        gapCfg.steadyLeadMs       = 4500u; // 216,000 frames -> anchor at 534,000 if 750k, let's prime at 600k
        gapCfg.heartbeatTimeoutMs = 500u;
        gapCfg.fadeMs             = 0u;
        gapConsumer.Configure(gapCfg);
        gapConsumer.AttachExternalBlockForTest(gapShm.get(), sizeof(*gapShm));

        // Anchor first while commit == 600,000 -> anchor = 384,000, first steady ends at 511,032
        stagec::AtomicStoreU64(&gapShm->header.write_commit_frame, 600000ULL);
        stagec::AtomicStoreU64(&gapShm->header.write_reserve_frame, 600000ULL);
        const routeb::RouteBPullStatus g1 = gapConsumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(g1.ok);
        CHECK(g1.readStartFrame == 384000ULL);
        CHECK(endFrame == 384000ULL + kFramesPerBlock); // 511,032

        // Producer resumes after ESC pause and commits to 750,000; ESC hole [520,000 .. 570,000) skipped!
        stagec::AtomicStoreU64(&gapShm->header.write_reserve_frame, 750000ULL);
        stagec::AtomicStoreU64(&gapShm->header.write_commit_frame, 750000ULL);
        gapConsumer.QueueSkipGap(520000ULL, 570000ULL);

        const routeb::RouteBPullStatus g2 = gapConsumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(g2.ok);
        CHECK(g2.liveFramesDelivered == kFramesPerBlock);
        CHECK(g2.silenceFramesDelivered == 0u);
        CHECK(gapShm->header.underrun_events == 0ULL);
        CHECK(gapShm->header.overrun_events == 0ULL);
        const uint32_t preGapFrames = static_cast<uint32_t>(520000ULL - g1.readEndFrame); // 8,968
        for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
            int16_t l = 0, r = 0;
            uint64_t dec = 0ULL;
            ReadPlanarSample(blk1.get(), kBlockBytes, i, l, r);
            CHECK(stagec::DecodeFrameCode(l, r, 8ULL, dec));
            const uint64_t expectedFrame = (i < preGapFrames)
                ? (g1.readEndFrame + i)
                : (570000ULL + (i - preGapFrames));
            CHECK(dec == (expectedFrame % stagec::kFrameCodeMod));
        }

        // Long ESC pause > ring capacity (writer advances far past old cursor, queued gap end is fresh)
        constexpr uint64_t kLongPauseCommit = 2420000ULL;
        WriteFrameCodeSpan(*gapShm, 2150000ULL, 270000ULL, 8ULL);
        stagec::AtomicStoreU64(&gapShm->header.write_reserve_frame, kLongPauseCommit);
        stagec::AtomicStoreU64(&gapShm->header.write_commit_frame, kLongPauseCommit);
        gapConsumer.QueueSkipGap(690000ULL, 2200000ULL);
        const routeb::RouteBPullStatus gLong = gapConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(gLong.ok);
        CHECK(gLong.liveFramesDelivered == kFramesPerBlock);
        CHECK(gLong.readStartFrame == 2200000ULL);
        CHECK(gapShm->header.overrun_events == 0ULL);
        int16_t ll = 0, lr = 0;
        uint64_t decLong = 0ULL;
        ReadPlanarSample(blk2.get(), kBlockBytes, 0u, ll, lr);
        CHECK(stagec::DecodeFrameCode(ll, lr, 8ULL, decLong));
        CHECK(decLong == (2200000ULL % stagec::kFrameCodeMod));
    }

    // (4d-2) Lapped long ESC pause with low post-resume commit (<= kSkipPostPullReserveFrames):
    // must prime cleanly with digital silence instead of rewinding into pause silence or underrunning.
    {
        QueryPerformanceCounter(&now);
        auto gap2Shm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*gap2Shm, 14ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*gap2Shm, 0ULL, 600000ULL, 14ULL);

        routeb::RouteBIpcConsumer gap2Consumer;
        routeb::RouteBConsumerConfig gap2Cfg{};
        gap2Cfg.steadyLeadMs       = 5500u;
        gap2Cfg.heartbeatTimeoutMs = 500u;
        gap2Cfg.fadeMs             = 0u;
        gap2Consumer.Configure(gap2Cfg);
        gap2Consumer.AttachExternalBlockForTest(gap2Shm.get(), sizeof(*gap2Shm));

        const routeb::RouteBPullStatus gInit = gap2Consumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(gInit.ok && gInit.readStartFrame == 336000ULL);

        // Writer advances past ring capacity during long pause, leaving only 40,000 fresh frames after gapEnd
        WriteFrameCodeSpan(*gap2Shm, 2200000ULL, 40000ULL, 14ULL);
        gap2Consumer.QueueSkipGap(690000ULL, 2200000ULL);
        const routeb::RouteBPullStatus gWait = gap2Consumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(gWait.ok);
        CHECK(gWait.producerOnline);
        CHECK(gWait.silenceFramesDelivered == kFramesPerBlock);
        CHECK(gWait.readEndFrame == 2200000ULL);
        CHECK(gap2Shm->header.underrun_events == 0ULL);
        CHECK(gap2Shm->header.overrun_events == 0ULL);

        // Producer commits +94,000 more frames -> avail=134,000 > 84,000 -> 50,000 live frames right-aligned from gapEnd
        WriteFrameCodeSpan(*gap2Shm, 2240000ULL, 94000ULL, 14ULL);
        const routeb::RouteBPullStatus gFire = gap2Consumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(gFire.ok);
        CHECK(gFire.liveFramesDelivered == 50000u);
        CHECK(gFire.silenceFramesDelivered == (kFramesPerBlock - 50000u));
        CHECK(gFire.watermarkFrames == routeb::kSkipPostPullReserveFrames);
        CHECK(gap2Shm->header.underrun_events == 0ULL);
        CHECK(gap2Shm->header.overrun_events == 0ULL);

        const uint32_t gPad = kFramesPerBlock - 50000u;
        for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
            int16_t l = 0, r = 0;
            ReadPlanarSample(blk1.get(), kBlockBytes, i, l, r);
            if (i < gPad) {
                CHECK(l == 0 && r == 0);
            } else {
                uint64_t dec = 0ULL;
                CHECK(stagec::DecodeFrameCode(l, r, 14ULL, dec));
                CHECK(dec == ((2200000ULL + (i - gPad)) % stagec::kFrameCodeMod));
            }
        }
    }

    // (4e) Zero-underrun primed JumpReadCursorToFrame (waits cleanly until > 84,000 frames committed)
    {
        QueryPerformanceCounter(&now);
        auto skipShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*skipShm, 10ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*skipShm, 0ULL, 360000ULL, 10ULL);

        routeb::RouteBIpcConsumer skipConsumer;
        routeb::RouteBConsumerConfig skipCfg{};
        skipCfg.steadyLeadMs       = 5500u;
        skipCfg.heartbeatTimeoutMs = 500u;
        skipCfg.fadeMs             = 0u;
        skipConsumer.Configure(skipCfg);
        skipConsumer.AttachExternalBlockForTest(skipShm.get(), sizeof(*skipShm));

        const routeb::RouteBPullStatus sInit = skipConsumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(sInit.ok && sInit.liveFramesDelivered == kFramesPerBlock);
        CHECK(skipShm->header.underrun_events == 0ULL);

        // Skip target at 300,000 when commit is 360,000 -> avail=60,000 <= 84,000 -> silent prime, ZERO underruns!
        skipConsumer.JumpReadCursorToFrame(300000ULL);
        const routeb::RouteBPullStatus sWait = skipConsumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(sWait.ok);
        CHECK(sWait.producerOnline);
        CHECK(sWait.silenceFramesDelivered == kFramesPerBlock);
        CHECK(skipShm->header.underrun_events == 0ULL);

        // Producer commits to 434,000 -> avail=134,000 > 84,000 -> usable=50,000 right-aligned after pad!
        WriteFrameCodeSpan(*skipShm, 360000ULL, 74000ULL, 10ULL);
        const routeb::RouteBPullStatus sFire = skipConsumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(sFire.ok);
        CHECK(sFire.liveFramesDelivered == 50000u);
        CHECK(sFire.silenceFramesDelivered == (kFramesPerBlock - 50000u));
        CHECK(sFire.watermarkFrames == routeb::kSkipPostPullReserveFrames);
        CHECK(skipShm->header.underrun_events == 0ULL);

        const uint32_t sPad = kFramesPerBlock - 50000u;
        for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
            int16_t l = 0, r = 0;
            ReadPlanarSample(blk1.get(), kBlockBytes, i, l, r);
            if (i < sPad) {
                CHECK(l == 0 && r == 0);
            } else {
                uint64_t dec = 0ULL;
                CHECK(stagec::DecodeFrameCode(l, r, 10ULL, dec));
                CHECK(dec == ((300000ULL + (i - sPad)) % stagec::kFrameCodeMod));
            }
        }

        // Next steady block pulls cleanly at 350,000 with zero underrun!
        WriteFrameCodeSpan(*skipShm, 434000ULL, 190000ULL, 10ULL);
        const routeb::RouteBPullStatus sNext = skipConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(sNext.ok);
        CHECK(sNext.liveFramesDelivered == kFramesPerBlock);
        CHECK(sNext.silenceFramesDelivered == 0u);
        CHECK(sNext.readStartFrame == 350000ULL);
        CHECK(skipShm->header.underrun_events == 0ULL);
    }

    // (4f) FillPreloadedBlocksAtPlay with minHeadPadFrames = 43200u (900ms DJ-crossfade pad)
    {
        QueryPerformanceCounter(&now);
        auto padShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*padShm, 12ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*padShm, 0ULL, 600000ULL, 12ULL);

        routeb::RouteBIpcConsumer padConsumer;
        routeb::RouteBConsumerConfig padCfg{};
        padCfg.steadyLeadMs       = 5500u;
        padCfg.heartbeatTimeoutMs = 500u;
        padCfg.fadeMs             = 0u;
        padConsumer.Configure(padCfg);
        padConsumer.AttachExternalBlockForTest(padShm.get(), sizeof(*padShm));

        constexpr uint32_t kMinHeadPad = 43200u;
        const routeb::RouteBPullStatus sPadPlay = padConsumer.FillPreloadedBlocksAtPlay(
            blk0.get(), blk1.get(), kBlockBytes, true, 100000ULL, kFramesPerBlock + 48000u, &endFrame, kMinHeadPad);
        CHECK(sPadPlay.ok);
        CHECK(sPadPlay.usedPrerollAnchor);
        CHECK(sPadPlay.silenceFramesDelivered == kMinHeadPad);
        CHECK(sPadPlay.liveFramesDelivered == (kFramesPerBlock * 2u - kMinHeadPad));
        CHECK(endFrame == 100000ULL + sPadPlay.liveFramesDelivered);

        for (uint32_t i = 0u; i < kFramesPerBlock * 2u; ++i) {
            int16_t l = 0, r = 0;
            if (i < kFramesPerBlock) {
                ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
            } else {
                ReadPlanarSample(blk1.get(), kBlockBytes, i - kFramesPerBlock, l, r);
            }
            if (i < kMinHeadPad) {
                CHECK(l == 0 && r == 0);
            } else {
                uint64_t dec = 0ULL;
                CHECK(stagec::DecodeFrameCode(l, r, 12ULL, dec));
                CHECK(dec == ((100000ULL + (i - kMinHeadPad)) % stagec::kFrameCodeMod));
            }
        }

        const routeb::RouteBPullStatus sPadBurst2 = padConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(sPadBurst2.ok);
        CHECK(sPadBurst2.liveFramesDelivered == kFramesPerBlock);
        CHECK(sPadBurst2.readStartFrame == 100000ULL + sPadPlay.liveFramesDelivered);
        CHECK(padShm->header.underrun_events == 0ULL);
    }

    // (4g) active_tune_in reserve (steadyLeadMs=5500, reserveForBurst2=block+96000) absorbs Burst2 + two sawtooth pulls
    {
        QueryPerformanceCounter(&now);
        auto tuneShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*tuneShm, 16ULL, now.QuadPart, freq.QuadPart);
        uint64_t tuneCommit = 600000ULL;
        WriteFrameCodeSpan(*tuneShm, 0ULL, tuneCommit, 16ULL);

        routeb::RouteBIpcConsumer tuneConsumer;
        routeb::RouteBConsumerConfig tuneCfg{};
        tuneCfg.steadyLeadMs       = 5500u;
        tuneCfg.heartbeatTimeoutMs = 500u;
        tuneCfg.fadeMs             = 0u;
        tuneConsumer.Configure(tuneCfg);
        tuneConsumer.AttachExternalBlockForTest(tuneShm.get(), sizeof(*tuneShm));

        const uint32_t reserveForBurst2 = kFramesPerBlock + 96000u;
        const routeb::RouteBPullStatus tPre = tuneConsumer.FillPreloadedBlocksAtPlay(
            blk0.get(), blk1.get(), kBlockBytes, false, 0ULL, reserveForBurst2, &endFrame, 0u);
        CHECK(tPre.ok);
        CHECK(tPre.watermarkFrames == reserveForBurst2);

        const routeb::RouteBPullStatus tBurst2 = tuneConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(tBurst2.ok);
        CHECK(tBurst2.liveFramesDelivered == kFramesPerBlock);
        CHECK(tBurst2.watermarkFrames == 96000ULL);
        CHECK(tuneShm->header.underrun_events == 0ULL);

        const uint64_t sawtoothStep = kFramesPerBlock - 32768ULL;
        WriteFrameCodeSpan(*tuneShm, tuneCommit, sawtoothStep, 16ULL);
        tuneCommit += sawtoothStep;
        const routeb::RouteBPullStatus tPull1 = tuneConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(tPull1.ok && tPull1.liveFramesDelivered == kFramesPerBlock);
        CHECK(tuneShm->header.underrun_events == 0ULL);

        WriteFrameCodeSpan(*tuneShm, tuneCommit, sawtoothStep, 16ULL);
        const routeb::RouteBPullStatus tPull2 = tuneConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(tPull2.ok && tPull2.liveFramesDelivered == kFramesPerBlock);
        CHECK(tPull2.watermarkFrames >= 30000ULL);
        CHECK(tuneShm->header.underrun_events == 0ULL);
    }

    // (4h) Long 4-minute ESC pause tail preservation: snapshot pre-pause unread tail before ring lap,
    // play 100% bit-exact pre-pause frames out of RAM, and splice cleanly into post-resume live ring.
    {
        QueryPerformanceCounter(&now);
        auto longShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*longShm, 18ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*longShm, 0ULL, 600000ULL, 18ULL);

        routeb::RouteBIpcConsumer longConsumer;
        routeb::RouteBConsumerConfig longCfg{};
        longCfg.steadyLeadMs       = 5500u;
        longCfg.heartbeatTimeoutMs = 500u;
        longCfg.fadeMs             = 0u;
        longConsumer.Configure(longCfg);
        longConsumer.AttachExternalBlockForTest(longShm.get(), sizeof(*longShm));

        const routeb::RouteBPullStatus sInit = longConsumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(sInit.ok && sInit.readStartFrame == 336000ULL);
        CHECK(endFrame == 463032ULL);

        // Buffer +222,368 frames (~4.63s) of unread pre-pause music up to 685,400, then advance pause silence past lag trigger
        constexpr uint64_t kEscStart = 685400ULL;
        constexpr uint64_t kLagCommit = 463032ULL + 500000ULL; // 963,032 (lag=500,000 >= 480,000)
        WriteFrameCodeSpan(*longShm, 600000ULL, kEscStart - 600000ULL, 18ULL);
        stagec::AtomicStoreU64(&longShm->header.write_reserve_frame, kLagCommit);
        stagec::AtomicStoreU64(&longShm->header.write_commit_frame, kLagCommit);

        CHECK(longConsumer.PreserveUnreadTailIfApproachingLap());
        CHECK(!longConsumer.PreserveUnreadTailIfApproachingLap());

        // Advance writer >11x ring capacity to 12,100,000 (wipes live ring) with post-resume audio starting at 11,800,000
        constexpr uint64_t kEscEnd = 11800000ULL;
        WriteFrameCodeSpan(*longShm, kEscEnd, 300000ULL, 18ULL);

        // Pull Block 1 BEFORE QueueSkipGap arrives -> must serve exact pre-pause frames from savedTail
        const routeb::RouteBPullStatus sPauseB1 = longConsumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(sPauseB1.ok);
        CHECK(sPauseB1.liveFramesDelivered == kFramesPerBlock);
        CHECK(sPauseB1.silenceFramesDelivered == 0u);
        CHECK(sPauseB1.readStartFrame == 463032ULL);
        CHECK(endFrame == 463032ULL + kFramesPerBlock);
        CHECK(longShm->header.underrun_events == 0ULL);
        CHECK(longShm->header.overrun_events == 0ULL);
        CHECK(longShm->header.overwrite_conflict_events == 0ULL);
        for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
            int16_t l = 0, r = 0;
            uint64_t dec = 0ULL;
            ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
            CHECK(stagec::DecodeFrameCode(l, r, 18ULL, dec));
            CHECK(dec == ((463032ULL + i) % stagec::kFrameCodeMod));
        }

        // Queue ESC gap [685,400 .. 11,800,000) -> Block 2 finishes savedTail and splices cleanly into post-resume ring
        longConsumer.QueueSkipGap(kEscStart, kEscEnd);
        const routeb::RouteBPullStatus sPauseB2 = longConsumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(sPauseB2.ok);
        CHECK(sPauseB2.liveFramesDelivered == kFramesPerBlock);
        CHECK(sPauseB2.silenceFramesDelivered == 0u);
        CHECK(sPauseB2.readStartFrame == 463032ULL + kFramesPerBlock);
        const uint32_t preGapFrames = static_cast<uint32_t>(kEscStart - (463032ULL + kFramesPerBlock)); // 95,336
        const uint32_t postGapFrames = kFramesPerBlock - preGapFrames;                                  // 31,696
        CHECK(endFrame == kEscEnd + postGapFrames);
        CHECK(longShm->header.underrun_events == 0ULL);
        CHECK(longShm->header.overrun_events == 0ULL);
        CHECK(longShm->header.overwrite_conflict_events == 0ULL);
        for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
            int16_t l = 0, r = 0;
            uint64_t dec = 0ULL;
            ReadPlanarSample(blk1.get(), kBlockBytes, i, l, r);
            CHECK(stagec::DecodeFrameCode(l, r, 18ULL, dec));
            const uint64_t expectedFrame = (i < preGapFrames)
                ? (463032ULL + kFramesPerBlock + i)
                : (kEscEnd + (i - preGapFrames));
            CHECK(dec == (expectedFrame % stagec::kFrameCodeMod));
        }

        // Pull Block 3 -> continues seamlessly in live post-resume ring
        WriteFrameCodeSpan(*longShm, 12100000ULL, 100000ULL, 18ULL);
        const routeb::RouteBPullStatus sPauseB3 = longConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(sPauseB3.ok);
        CHECK(sPauseB3.liveFramesDelivered == kFramesPerBlock);
        CHECK(sPauseB3.silenceFramesDelivered == 0u);
        CHECK(sPauseB3.readStartFrame == kEscEnd + postGapFrames);
        CHECK(longShm->header.underrun_events == 0ULL);
        CHECK(longShm->header.overrun_events == 0ULL);
    }

    // (4i) SetSongEndBarrier stops outgoing Slot 1 cleanly without underrun & clears at FillPreloadedBlocksAtPlay
    {
        QueryPerformanceCounter(&now);
        auto barrierShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*barrierShm, 20ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*barrierShm, 0ULL, 600000ULL, 20ULL);

        routeb::RouteBIpcConsumer barrierConsumer;
        routeb::RouteBConsumerConfig barrierCfg{};
        barrierCfg.steadyLeadMs       = 5500u;
        barrierCfg.heartbeatTimeoutMs = 500u;
        barrierCfg.fadeMs             = 0u;
        barrierConsumer.Configure(barrierCfg);
        barrierConsumer.AttachExternalBlockForTest(barrierShm.get(), sizeof(*barrierShm));

        const routeb::RouteBPullStatus bInit = barrierConsumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(bInit.ok && bInit.readStartFrame == 336000ULL && endFrame == 463032ULL);

        WriteFrameCodeSpan(*barrierShm, 600000ULL, 400000ULL, 20ULL); // commit = 1,000,000
        constexpr uint64_t kSong2Start = 500000ULL;
        barrierConsumer.SetSongEndBarrier(kSong2Start);

        const uint32_t expectedTail = static_cast<uint32_t>(kSong2Start - 463032ULL); // 36,968
        const routeb::RouteBPullStatus b1 = barrierConsumer.FillSteadyPlanarBlock(blk0.get(), kBlockBytes, &endFrame);
        CHECK(b1.ok);
        CHECK(b1.liveFramesDelivered == expectedTail);
        CHECK(b1.silenceFramesDelivered == (kFramesPerBlock - expectedTail));
        CHECK(b1.readEndFrame == kSong2Start);
        CHECK(endFrame == kSong2Start);
        CHECK(barrierShm->header.underrun_events == 0ULL);
        for (uint32_t i = 0u; i < kFramesPerBlock; ++i) {
            int16_t l = 0, r = 0;
            ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
            if (i < expectedTail) {
                uint64_t dec = 0ULL;
                CHECK(stagec::DecodeFrameCode(l, r, 20ULL, dec));
                CHECK(dec == ((463032ULL + i) % stagec::kFrameCodeMod));
            } else {
                CHECK(l == 0 && r == 0);
            }
        }

        // Next pull while barrier is still held -> full silence with zero underrun
        const routeb::RouteBPullStatus b2 = barrierConsumer.FillSteadyPlanarBlock(blk1.get(), kBlockBytes, &endFrame);
        CHECK(b2.ok);
        CHECK(b2.liveFramesDelivered == 0u);
        CHECK(b2.silenceFramesDelivered == kFramesPerBlock);
        CHECK(endFrame == kSong2Start);
        CHECK(barrierShm->header.underrun_events == 0ULL);

        // Slot 0 Play clears barrier and starts cleanly at kSong2Start with 14400u back-to-back pad
        const routeb::RouteBPullStatus bPlay = barrierConsumer.FillPreloadedBlocksAtPlay(
            blk0.get(), blk1.get(), kBlockBytes, true, kSong2Start, kFramesPerBlock + 48000u, &endFrame, 14400u);
        CHECK(bPlay.ok);
        CHECK(bPlay.usedPrerollAnchor);
        CHECK(bPlay.silenceFramesDelivered == 14400u);
        CHECK(bPlay.liveFramesDelivered == (kFramesPerBlock * 2u - 14400u));
        CHECK(endFrame == kSong2Start + bPlay.liveFramesDelivered);
        for (uint32_t i = 0u; i < kFramesPerBlock * 2u; ++i) {
            int16_t l = 0, r = 0;
            if (i < kFramesPerBlock) {
                ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
            } else {
                ReadPlanarSample(blk1.get(), kBlockBytes, i - kFramesPerBlock, l, r);
            }
            if (i < 14400u) {
                CHECK(l == 0 && r == 0);
            } else {
                uint64_t dec = 0ULL;
                CHECK(stagec::DecodeFrameCode(l, r, 20ULL, dec));
                CHECK(dec == ((kSong2Start + (i - 14400u)) % stagec::kFrameCodeMod));
            }
        }

        const routeb::RouteBPullStatus bNext = barrierConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(bNext.ok);
        CHECK(bNext.liveFramesDelivered == kFramesPerBlock);
        CHECK(bNext.readStartFrame == kSong2Start + bPlay.liveFramesDelivered);
        CHECK(barrierShm->header.underrun_events == 0ULL);
    }

    // (4j) Gap-aware FillPreloadedBlocksAtPlay splices across ESC pause inside preroll window without re-entering silence
    {
        QueryPerformanceCounter(&now);
        auto pgShm = std::make_unique<stagec::StageCIpcSharedBlock>();
        InitTestSharedBlock(*pgShm, 22ULL, now.QuadPart, freq.QuadPart);
        WriteFrameCodeSpan(*pgShm, 0ULL, 1000000ULL, 22ULL);

        routeb::RouteBIpcConsumer pgConsumer;
        routeb::RouteBConsumerConfig pgCfg{};
        pgCfg.steadyLeadMs       = 5500u;
        pgCfg.heartbeatTimeoutMs = 500u;
        pgCfg.fadeMs             = 0u;
        pgConsumer.Configure(pgCfg);
        pgConsumer.AttachExternalBlockForTest(pgShm.get(), sizeof(*pgShm));

        pgConsumer.QueueSkipGap(240000ULL, 650000ULL);
        const routeb::RouteBPullStatus pgPlay = pgConsumer.FillPreloadedBlocksAtPlay(
            blk0.get(), blk1.get(), kBlockBytes, true, 200000ULL, kFramesPerBlock + 48000u, &endFrame, 14400u);
        CHECK(pgPlay.ok);
        CHECK(pgPlay.usedPrerollAnchor);
        CHECK(pgPlay.liveFramesDelivered == 214968u);
        CHECK(pgPlay.silenceFramesDelivered == (kFramesPerBlock * 2u - 214968u));
        CHECK(pgPlay.readStartFrame == 200000ULL);
        CHECK(endFrame == 824968ULL);
        CHECK(pgPlay.readEndFrame == 824968ULL);
        CHECK(pgPlay.watermarkFrames == (kFramesPerBlock + 48000u));

        const uint32_t pgPad = kFramesPerBlock * 2u - 214968u;
        for (uint32_t i = 0u; i < kFramesPerBlock * 2u; ++i) {
            int16_t l = 0, r = 0;
            if (i < kFramesPerBlock) {
                ReadPlanarSample(blk0.get(), kBlockBytes, i, l, r);
            } else {
                ReadPlanarSample(blk1.get(), kBlockBytes, i - kFramesPerBlock, l, r);
            }
            if (i < pgPad) {
                CHECK(l == 0 && r == 0);
            } else {
                const uint32_t liveIdx = i - pgPad;
                const uint64_t expectedFrame = (liveIdx < 40000u)
                    ? (200000ULL + liveIdx)
                    : (650000ULL + (liveIdx - 40000u));
                uint64_t dec = 0ULL;
                CHECK(stagec::DecodeFrameCode(l, r, 22ULL, dec));
                CHECK(dec == (expectedFrame % stagec::kFrameCodeMod));
            }
        }

        const routeb::RouteBPullStatus pgSteady = pgConsumer.FillSteadyPlanarBlock(blk2.get(), kBlockBytes, &endFrame);
        CHECK(pgSteady.ok);
        CHECK(pgSteady.liveFramesDelivered == kFramesPerBlock);
        CHECK(pgSteady.readStartFrame == 824968ULL);
        CHECK(pgShm->header.underrun_events == 0ULL);
    }
}

class MockSmtcTransport final : public routeb::ISmtcTransport {
public:
    bool Start() override { return true; }
    void Stop() noexcept override {}

    routeb::SmtcSnapshot GetSnapshot() const override { return snap; }

    void RequestPlay() override {
        ++playCount;
        snap.playbackStatus = routeb::kSmtcStatusPlaying;
    }
    void RequestPause() override {
        ++pauseCount;
        snap.playbackStatus = routeb::kSmtcStatusPaused;
    }
    void RequestPauseAndRewindToStart() override {
        ++pauseRewindCount;
        ++pauseCount;
        snap.playbackStatus = routeb::kSmtcStatusPaused;
    }
    void RequestPauseAndRestartTrack() override {
        ++pauseRestartCount;
        ++pauseCount;
        snap.playbackStatus = routeb::kSmtcStatusPaused;
    }
    void RequestSkipNext() override     { ++nextCount; }
    void RequestSkipPrevious() override { ++prevCount; }

    routeb::SmtcSnapshot snap{};
    uint32_t playCount         = 0u;
    uint32_t pauseCount        = 0u;
    uint32_t pauseRewindCount  = 0u;
    uint32_t pauseRestartCount = 0u;
    uint32_t nextCount         = 0u;
    uint32_t prevCount         = 0u;
};

void InitMockCtrl(routeb::RadioBridgeCtrlBlock& ctrl, int64_t nowQpc, int64_t qpcFreq) {
    std::memset(&ctrl, 0, sizeof(ctrl));
    ctrl.magic                   = routeb::kCtrlMagic;
    ctrl.version                 = routeb::kCtrlVersion;
    ctrl.total_bytes             = routeb::kCtrlBytes;
    ctrl.qpc_freq                = static_cast<uint64_t>(qpcFreq);
    ctrl.game_pid                = 4321u;
    ctrl.game_heartbeat_qpc      = nowQpc;
    ctrl.radio_mode              = 0u;
    ctrl.station_tuned           = 1u;
    ctrl.esc_paused              = 0u;
    ctrl.active_slot_active      = 1u;
    ctrl.active_slot_is_user     = 1u;
    ctrl.active_slot_play_ms     = 5000;
    ctrl.active_slot_duration_ms = 1200000;
    ctrl.slot_transition_seq     = 1u;
}

void RunSuite5CoordinatorStateMachine() {
    std::printf("[Suite 5] Coordinator State Machine (16 deterministic scenarios)\n");
    constexpr int64_t kQpcFreq = 10000000LL; // 1 ms = 10,000 ticks
    constexpr int64_t kMs      = 10000LL;

    // 1. Inactive gating (radio_mode != 0, userEnabled=false, appleMusicPresetActive=false, observeOnly=true, stale HB)
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_duration_ms = 25000;

        routeb::RouteBCoordinator coord(smtc);
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.pauseCount == 1u);

        // Switch away from Radio Mode (Sequential = 1) -> must release pause hold immediately
        ctrl.radio_mode = 1u;
        nowQpc += 10LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::Inactive);
        CHECK(!st.audioGateMuted);
        CHECK(smtc.playCount == 1u);
        CHECK(ctrl.bridge_enabled == 1u);

        // Back to Radio + user track, verify userEnabled=false, non-AM preset, ObserveOnly, stale HB
        ctrl.radio_mode = 0u;
        ctrl.active_slot_is_user = 1u;
        ctrl.active_slot_duration_ms = 1200000;
        st = coord.Tick(nowQpc, kQpcFreq, true, false, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::Inactive);
        CHECK(!st.bridgeEnabled);
        CHECK(ctrl.bridge_enabled == 0u);

        st = coord.Tick(nowQpc, kQpcFreq, false, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::Inactive);
        CHECK(!st.bridgeEnabled);

        ctrl.flags = routeb::kCtrlFlagObserveOnly;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::Inactive);
        CHECK(st.observeOnly);

        ctrl.flags = 0u;
        nowQpc += 2000LL * kMs; // Heartbeat 2000ms stale (<= 3000ms D3D9 hitch tolerance)
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(st.gameOnline);

        nowQpc += 1500LL * kMs; // Heartbeat now 3500ms stale (> 3000ms)
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::Inactive);
        CHECK(!st.gameOnline);
    }

    // 2..6. Song end -> DrainingSongEnd -> PausedForDj -> Guards -> PreRollingForDj -> Fallback & PlayingSong
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        smtc.snap.titleUtf8 = "Song A";
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);

        routeb::RouteBCoordinator coord(smtc);
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);

        // Scenario 2: Natural track end -> DrainingSongEnd (calls RequestPauseAndRewindToStart)
        nowQpc += 50LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.naturalTrackEndSeq = 1u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::DrainingSongEnd);
        CHECK(st.audioGateMuted);
        CHECK(smtc.pauseRewindCount == 1u);
        CHECK(smtc.pauseCount == 1u);
        CHECK(ctrl.request_trigger_dj_seq == 1u);
        CHECK(ctrl.song_end_commit_frame == 480000ULL);
        CHECK(ctrl.audio_gate_muted == 1u);

        // Scenario 2b: DJ crossfade window (station_crossfading=1, slot_transition_seq bumps for DJ Play,
        // user_slot_play_seq unchanged, active_slot still outgoing user track) -> must enter PausedForDj
        // and NEVER falsely unpause or accept B/N skips!
        nowQpc += 5000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.station_crossfading = 1u;
        ctrl.slot_transition_seq = 2u; // bumped by Play(DJ), NOT user_slot_play_seq!
        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.playCount == 0u);

        ctrl.pending_skip_seq = 9u;
        ctrl.pending_skip_dir = 1;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.nextCount == 0u);
        CHECK(smtc.playCount == 0u);

        // Scenario 3: Crossfade completes -> non-user DJ slot active -> PausedForDj
        nowQpc += 1000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.station_crossfading = 0u;
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_play_ms = 0;
        ctrl.active_slot_duration_ms = 0; // Uninitialized duration first!
        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 1u;
        ctrl.standby_preloaded_ready = 1u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.playCount == 0u);

        // Scenario 4: Guards hold PausedForDj when actDurMs <= 0, djRemainingMs > 9000, or stbyPreloadReady=false
        ctrl.active_slot_duration_ms = 25000;
        ctrl.active_slot_play_ms = 15000; // rem = 10000 > 9000
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.playCount == 0u);

        ctrl.active_slot_play_ms = 18000; // rem = 7000 <= 9000
        ctrl.standby_preloaded_ready = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 480000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.playCount == 0u);

        // Scenario 5: All conditions met -> PreRollingForDj & audible preroll anchor refinement
        ctrl.standby_preloaded_ready = 1u;
        nowQpc += 10LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 900000ULL, &ctrl, 899000ULL, 899000ULL);
        CHECK(st.state == routeb::BridgeState::PreRollingForDj);
        CHECK(st.prerollActive);
        CHECK(!st.audioGateMuted);
        CHECK(smtc.playCount == 1u);
        CHECK(ctrl.preroll_active == 1u);
        CHECK(ctrl.preroll_start_frame == 900000ULL);

        nowQpc += 200LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 920000ULL, &ctrl, 918000ULL, 912000ULL);
        CHECK(st.state == routeb::BridgeState::PreRollingForDj);
        CHECK(ctrl.preroll_start_frame == 912000ULL);

        // Scenario 6a: Standby becomes non-user (ad/news queued next) -> fallback to PausedForDj
        ctrl.standby_slot_is_user = 0u;
        nowQpc += 10LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 905000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(!st.prerollActive);
        CHECK(st.audioGateMuted);
        CHECK(smtc.pauseCount == 2u);

        // Scenario 6b: Pre-roll again -> user track takes over -> PlayingSong
        ctrl.standby_slot_is_user = 1u;
        ctrl.standby_preloaded_ready = 1u;
        ctrl.active_slot_play_ms = 20000;
        nowQpc += 2000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 950000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PreRollingForDj);
        CHECK(smtc.playCount == 2u);

        ctrl.active_slot_is_user = 1u;
        ctrl.active_slot_duration_ms = 1200000;
        ctrl.active_slot_play_ms = 0;
        ctrl.slot_transition_seq = 3u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 960000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(!st.prerollActive);
        CHECK(!st.audioGateMuted);
    }

    // 7. Back-to-back user songs (zero pause)
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 1u;
        ctrl.slot_transition_seq = 5u;

        routeb::RouteBCoordinator coord(smtc);
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 1200000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);

        smtc.snap.naturalTrackEndSeq = 1u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 1200000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.pauseCount == 0u);
        CHECK(st.prerollActive);
        CHECK(!st.audioGateMuted);
        CHECK(ctrl.request_trigger_dj_seq == 1u);
        CHECK(ctrl.preroll_start_frame == 1200000ULL);

        ctrl.slot_transition_seq = 6u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 1210000ULL, &ctrl);
        CHECK(!st.prerollActive);
    }

    // 7b. Back-to-back exact runFirstAudibleCommitFrame boundary + gapless fallback
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 1u;
        ctrl.slot_transition_seq = 8u;

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 1224000ULL, &ctrl, 1200000ULL, 1200000ULL);

        smtc.snap.naturalTrackEndSeq = 1u;
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 1224000ULL, &ctrl, 1200000ULL, 1200000ULL);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(st.prerollActive);
        CHECK(ctrl.song_end_commit_frame == 1200000ULL);
        CHECK(ctrl.preroll_start_frame == 1200000ULL);

        // Gapless track transition where runFirstAudibleCommitFrame is > 2s (96,000 frames) ago -> falls back cleanly
        MockSmtcTransport smtcGapless;
        smtcGapless.snap.connected = true;
        smtcGapless.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 1u;
        routeb::RouteBCoordinator coordGapless(smtcGapless);
        coordGapless.Tick(nowQpc, kQpcFreq, true, true, 1500000ULL, &ctrl, 1500000ULL, 800000ULL);
        smtcGapless.snap.naturalTrackEndSeq = 1u;
        auto stGapless = coordGapless.Tick(nowQpc, kQpcFreq, true, true, 1500000ULL, &ctrl, 1500000ULL, 800000ULL);
        CHECK(stGapless.state == routeb::BridgeState::PlayingSong);
        CHECK(ctrl.song_end_commit_frame == 1500000ULL);
        CHECK(ctrl.preroll_start_frame == 1500000ULL);
    }

    // 8. ESC pause menu during PlayingSong (with ESC silence-hole splice) vs PausedForDj
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl, 100000ULL, 50000ULL);

        // Press ESC at commit 100,000 -> debounced for 100ms, then SMTC pause takes ~120ms trailing audio (lastAudible=106,000)
        ctrl.esc_paused = 1u;
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl, 100000ULL, 50000ULL);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.pauseCount == 0u);

        nowQpc += 50LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl, 100000ULL, 50000ULL);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.pauseCount == 0u);

        nowQpc += 50LL * kMs; // +100ms total -> confirmed ESC pause
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl, 100000ULL, 50000ULL);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);
        CHECK(smtc.pauseCount == 1u);

        nowQpc += 120LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 106000ULL, &ctrl, 106000ULL, 50000ULL);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);

        // Resume from ESC at commit 300,000; AM starts producing audible frames at commit 312,000
        nowQpc += 4000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.esc_paused = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 300000ULL, &ctrl, 106000ULL, 50000ULL);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.playCount == 1u);
        CHECK(ctrl.esc_splice_seq == 0u);

        nowQpc += 250LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 315000ULL, &ctrl, 315000ULL, 312000ULL);
        CHECK(ctrl.esc_splice_seq == 1u);
        CHECK(ctrl.esc_splice_start_frame == 106000ULL);
        CHECK(ctrl.esc_splice_end_frame == 312000ULL);

        // Second ESC cycle: trailing audible updated up to +650ms (<= 800ms), resume waits at +900ms (< 1500ms)
        ctrl.esc_paused = 1u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 315000ULL, &ctrl, 315000ULL, 312000ULL);
        CHECK(st.state == routeb::BridgeState::PlayingSong);

        nowQpc += 100LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 315000ULL, &ctrl, 315000ULL, 312000ULL);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);
        CHECK(smtc.pauseCount == 2u);

        nowQpc += 650LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 346000ULL, &ctrl, 346000ULL, 312000ULL);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);

        nowQpc += 3000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.esc_paused = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 490000ULL, &ctrl, 346000ULL, 312000ULL);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.playCount == 2u);
        CHECK(ctrl.esc_splice_seq == 1u);

        nowQpc += 900LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 533000ULL, &ctrl, 346000ULL, 312000ULL);
        CHECK(ctrl.esc_splice_seq == 1u);

        nowQpc += 100LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 540000ULL, &ctrl, 540000ULL, 535000ULL);
        CHECK(ctrl.esc_splice_seq == 2u);
        CHECK(ctrl.esc_splice_start_frame == 346000ULL);
        CHECK(ctrl.esc_splice_end_frame == 535000ULL);

        // Now enter PausedForDj, then press ESC and un-ESC while still DJ
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_duration_ms = 20000;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 540000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.pauseCount == 3u);

        ctrl.esc_paused = 1u;
        coord.Tick(nowQpc, kQpcFreq, true, true, 540000ULL, &ctrl);
        nowQpc += 100LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 540000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);
        CHECK(smtc.pauseCount == 3u); // Did not re-pause

        ctrl.esc_paused = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 540000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.playCount == 2u); // Did NOT prematurely resume AM during DJ!
    }

    // 9. Car exit / station switch (station_tuned = 0) NEVER pauses in song, releases DJ hold immediately
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);

        ctrl.station_tuned = 0u;
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.pauseCount == 0u);

        // Tune back on DJ, pause AM, then exit car -> must resume AM right away
        ctrl.station_tuned = 1u;
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_duration_ms = 20000;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.pauseCount == 1u);

        ctrl.station_tuned = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(!st.audioGateMuted);
        CHECK(smtc.playCount == 1u);
    }

    // 10 & 11. B/N manual skip during PlayingSong (+ suppress + fallback) vs ignored during DJ
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        smtc.snap.trackSeq = 1u;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 300000ULL, &ctrl);

        // N key skip (+1) completed via trackSeq change
        ctrl.pending_skip_seq = 1u;
        ctrl.pending_skip_dir = 1;
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 300000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::SkippingTrack);
        CHECK(smtc.nextCount == 1u);

        nowQpc += 50LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.trackSeq = 2u;
        smtc.snap.naturalTrackEndSeq = 1u; // Side effect of skip
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 301000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(ctrl.request_skip_ack_seq == 1u);
        CHECK(ctrl.skip_audio_ready_frame == 301000ULL);
        CHECK(ctrl.request_trigger_dj_seq == 0u);

        // Suppress naturalTrackEndSeq inside 2000ms skipSuppressMs window
        nowQpc += 500LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.naturalTrackEndSeq = 2u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 310000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(ctrl.request_trigger_dj_seq == 0u);

        // B key (-1) same-track restart completed via 350ms fallback
        nowQpc += 2100LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.pending_skip_seq = 2u;
        ctrl.pending_skip_dir = -1;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 350000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::SkippingTrack);
        CHECK(smtc.prevCount == 1u);

        nowQpc += 100LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 351000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::SkippingTrack);

        nowQpc += 300LL * kMs; // +400ms total >= 350ms fallback
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 360000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(ctrl.request_skip_ack_seq == 2u);
        CHECK(ctrl.skip_audio_ready_frame == 360000ULL);

        // Scenario 11: B/N ignored during PausedForDj & PreRollingForDj
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_play_ms = 0;
        ctrl.active_slot_duration_ms = 25000;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 370000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);

        ctrl.pending_skip_seq = 3u;
        ctrl.pending_skip_dir = 1;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 370000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.nextCount == 1u);
        CHECK(ctrl.request_skip_ack_seq == 2u);

        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 1u;
        ctrl.standby_preloaded_ready = 1u;
        ctrl.active_slot_play_ms = 20000;
        nowQpc += 350LL * kMs; // Satisfy 300ms DJ pause settle guard
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 375000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PreRollingForDj);

        ctrl.pending_skip_seq = 4u;
        ctrl.pending_skip_dir = -1;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 375000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PreRollingForDj);
        CHECK(smtc.prevCount == 1u);
        CHECK(ctrl.request_skip_ack_seq == 2u);
    }

    // 12. ResetAndReleaseHolds calls RequestPlay when held and clears Host CL4..7 fields
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_duration_ms = 20000;

        routeb::RouteBCoordinator coord(smtc);
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.pauseCount == 1u);
        CHECK(ctrl.host_heartbeat_qpc == nowQpc);
        CHECK(ctrl.host_pid != 0u);

        coord.ResetAndReleaseHolds(&ctrl);
        CHECK(smtc.playCount == 1u);
        CHECK(ctrl.bridge_enabled == 0u);
        CHECK(ctrl.bridge_state == static_cast<uint32_t>(routeb::BridgeState::Inactive));
        CHECK(ctrl.audio_gate_muted == 0u);
        CHECK(ctrl.preroll_active == 0u);
        CHECK(ctrl.host_heartbeat_qpc == 0LL);
        CHECK(ctrl.host_pid == 0u);
    }

    // 13. Mid-DJ tune-in calls RequestPauseAndRestartTrack + 300ms settle before PreRollingForDj;
    //     chained DJ also calls RequestPauseAndRestartTrack; natural track end calls RequestPauseAndRewindToStart
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.station_tuned = 0u;
        ctrl.active_slot_is_user = 0u;
        ctrl.active_slot_play_ms = 20000;
        ctrl.active_slot_duration_ms = 25000; // djRemainingMs = 5000 <= 9000
        ctrl.standby_slot_active = 1u;
        ctrl.standby_slot_is_user = 1u;
        ctrl.standby_preloaded_ready = 1u;

        routeb::RouteBCoordinator coord(smtc);
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.pauseRestartCount == 0u);

        // Tune in mid-DJ while djRemainingMs == 5000 <= 9000 and preload is ready:
        // Must restart+pause first and hold PausedForDj for >= 300ms settle!
        nowQpc += 50LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.station_tuned = 1u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 105000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.pauseRestartCount == 1u);
        CHECK(smtc.pauseRewindCount == 0u);
        CHECK(smtc.playCount == 0u);

        nowQpc += 150LL * kMs; // +150ms < 300ms settle
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.active_slot_play_ms = 20150;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 110000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.playCount == 0u);

        nowQpc += 150LL * kMs; // +300ms >= 300ms settle -> PreRollingForDj
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.active_slot_play_ms = 20300;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 115000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PreRollingForDj);
        CHECK(smtc.playCount == 1u);

        // Chained 2nd DJ queued ahead -> fallback PausedForDj uses RequestPauseAndRestartTrack
        nowQpc += 100LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.standby_slot_is_user = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 120000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtc.pauseRestartCount == 2u);
        CHECK(smtc.pauseRewindCount == 0u);

        // Finish DJ -> back to user track -> PlayingSong, then natural track end calls RequestPauseAndRewindToStart
        nowQpc += 2000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.active_slot_is_user = 1u;
        ctrl.active_slot_play_ms = 500;
        ctrl.active_slot_duration_ms = 1200000;
        ctrl.standby_slot_is_user = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 130000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.playCount == 2u);

        nowQpc += 5000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.naturalTrackEndSeq = 1u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 200000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::DrainingSongEnd);
        CHECK(smtc.pauseRewindCount == 1u);
        CHECK(smtc.pauseRestartCount == 2u);
    }

    // 14. Short phantom ESC pulses (20ms, 50ms, 90ms) off-station and on-station are completely ignored
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);

        const uint32_t pulseDurationsMs[] = { 20u, 50u, 90u };
        const uint32_t tunedModes[] = { 0u, 1u };
        for (uint32_t tuned : tunedModes) {
            ctrl.station_tuned = tuned;
            for (uint32_t durMs : pulseDurationsMs) {
                nowQpc += 500LL * kMs;
                ctrl.game_heartbeat_qpc = nowQpc;
                ctrl.esc_paused = 1u;
                auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 120000ULL, &ctrl);
                CHECK(st.state == routeb::BridgeState::PlayingSong);
                CHECK(smtc.pauseCount == 0u);

                nowQpc += static_cast<int64_t>(durMs) * kMs;
                ctrl.game_heartbeat_qpc = nowQpc;
                st = coord.Tick(nowQpc, kQpcFreq, true, true, 122000ULL, &ctrl);
                CHECK(st.state == routeb::BridgeState::PlayingSong);
                CHECK(smtc.pauseCount == 0u);

                nowQpc += 1LL * kMs;
                ctrl.game_heartbeat_qpc = nowQpc;
                ctrl.esc_paused = 0u;
                st = coord.Tick(nowQpc, kQpcFreq, true, true, 122500ULL, &ctrl);
                CHECK(st.state == routeb::BridgeState::PlayingSong);
                CHECK(smtc.pauseCount == 0u);
                CHECK(smtc.playCount == 0u);
                CHECK(ctrl.esc_splice_seq == 0u);
            }
        }
    }

    // 15. Play Verify Watchdog heals late 4->5->4->5 Apple Music pause bounce after ESC resume,
    //     and does NOT fight a manual user pause after the verify window settles
    {
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);
        ctrl.station_tuned = 0u;

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);

        // Real ESC pause (>= 100ms)
        ctrl.esc_paused = 1u;
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        nowQpc += 120LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 102000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);
        CHECK(smtc.pauseCount == 1u);

        // Resume ESC -> calls RequestPlay() (playCount == 1)
        nowQpc += 1000LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.esc_paused = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 150000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.playCount == 1u);

        // Simulate Apple Music UWP pause-teardown race: bounces back to Paused (5) at +20ms
        nowQpc += 20LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPaused;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 151000ULL, &ctrl);
        CHECK(smtc.playCount == 1u); // Must not retry before playVerifyRetryMs (250ms)

        nowQpc += 180LL * kMs; // +200ms total < 250ms
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 155000ULL, &ctrl);
        CHECK(smtc.playCount == 1u);

        nowQpc += 60LL * kMs; // +260ms total >= 250ms -> watchdog retries RequestPlay()!
        ctrl.game_heartbeat_qpc = nowQpc;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 158000ULL, &ctrl);
        CHECK(smtc.playCount == 2u);
        CHECK(smtc.snap.playbackStatus == routeb::kSmtcStatusPlaying);

        // Hold Playing continuously across >= 400ms to settle verify window
        nowQpc += 50LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        coord.Tick(nowQpc, kQpcFreq, true, true, 160000ULL, &ctrl); // First observes Playing -> starts 400ms settle

        nowQpc += 450LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        coord.Tick(nowQpc, kQpcFreq, true, true, 180000ULL, &ctrl); // Settle expires -> clears playVerifyPending_

        // Manual user pause at +1000ms must NEVER be overridden!
        nowQpc += 550LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPaused;
        coord.Tick(nowQpc, kQpcFreq, true, true, 200000ULL, &ctrl);
        nowQpc += 400LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        coord.Tick(nowQpc, kQpcFreq, true, true, 210000ULL, &ctrl);
        CHECK(smtc.playCount == 2u);
        CHECK(smtc.snap.playbackStatus == routeb::kSmtcStatusPaused);
    }

    // 16. (a) ESC re-entry while playVerifyPending_ is active and SMTC bounced Paused transfers pause ownership;
    //     (b) Pause Verify Watchdog retries plain RequestPause (never RequestPauseAndRestartTrack) and does not fight manual Play
    {
        // (a) ESC re-entry ownership transfer
        int64_t nowQpc = 1000LL * kMs;
        MockSmtcTransport smtc;
        smtc.snap.connected = true;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrl{};
        InitMockCtrl(ctrl, nowQpc, kQpcFreq);

        routeb::RouteBCoordinator coord(smtc);
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);

        ctrl.esc_paused = 1u;
        coord.Tick(nowQpc, kQpcFreq, true, true, 100000ULL, &ctrl);
        nowQpc += 120LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        coord.Tick(nowQpc, kQpcFreq, true, true, 102000ULL, &ctrl);
        CHECK(smtc.pauseCount == 1u);

        // Resume first ESC -> RequestPlay() armed
        nowQpc += 500LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.esc_paused = 0u;
        coord.Tick(nowQpc, kQpcFreq, true, true, 120000ULL, &ctrl);
        CHECK(smtc.playCount == 1u);

        // Before 250ms retry fires, AM bounced Paused (5) AND user re-enters ESC for >= 100ms
        nowQpc += 20LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        smtc.snap.playbackStatus = routeb::kSmtcStatusPaused;
        ctrl.esc_paused = 1u;
        coord.Tick(nowQpc, kQpcFreq, true, true, 121000ULL, &ctrl);

        nowQpc += 110LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        auto st = coord.Tick(nowQpc, kQpcFreq, true, true, 126000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PausedForEsc);
        CHECK(smtc.pauseCount == 1u); // Already paused; ownership transferred via playVerifyPending_

        // Exit second ESC -> must resume AM!
        nowQpc += 500LL * kMs;
        ctrl.game_heartbeat_qpc = nowQpc;
        ctrl.esc_paused = 0u;
        st = coord.Tick(nowQpc, kQpcFreq, true, true, 140000ULL, &ctrl);
        CHECK(st.state == routeb::BridgeState::PlayingSong);
        CHECK(smtc.playCount == 2u);

        // (b) Pause Verify Watchdog for PausedForDj
        int64_t pQpc = 5000LL * kMs;
        MockSmtcTransport smtcP;
        smtcP.snap.connected = true;
        smtcP.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        routeb::RadioBridgeCtrlBlock ctrlP{};
        InitMockCtrl(ctrlP, pQpc, kQpcFreq);

        routeb::RouteBCoordinator coordP(smtcP);
        coordP.Tick(pQpc, kQpcFreq, true, true, 100000ULL, &ctrlP);

        ctrlP.active_slot_is_user = 0u;
        ctrlP.active_slot_duration_ms = 25000;
        st = coordP.Tick(pQpc, kQpcFreq, true, true, 105000ULL, &ctrlP);
        CHECK(st.state == routeb::BridgeState::PausedForDj);
        CHECK(smtcP.pauseRestartCount == 1u);
        CHECK(smtcP.pauseCount == 1u);

        // Simulate dropped Pause: AM stays Playing (4)
        pQpc += 50LL * kMs;
        ctrlP.game_heartbeat_qpc = pQpc;
        smtcP.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        coordP.Tick(pQpc, kQpcFreq, true, true, 107000ULL, &ctrlP);
        CHECK(smtcP.pauseCount == 1u); // Not retried before 350ms

        pQpc += 310LL * kMs; // +360ms >= 350ms -> retries plain RequestPause (NOT RequestPauseAndRestartTrack)
        ctrlP.game_heartbeat_qpc = pQpc;
        coordP.Tick(pQpc, kQpcFreq, true, true, 110000ULL, &ctrlP);
        CHECK(smtcP.pauseCount == 2u);
        CHECK(smtcP.pauseRestartCount == 1u);
        CHECK(smtcP.snap.playbackStatus == routeb::kSmtcStatusPaused);

        // Settle Paused (5) across >= 400ms
        pQpc += 50LL * kMs;
        ctrlP.game_heartbeat_qpc = pQpc;
        coordP.Tick(pQpc, kQpcFreq, true, true, 112000ULL, &ctrlP);
        pQpc += 450LL * kMs;
        ctrlP.game_heartbeat_qpc = pQpc;
        coordP.Tick(pQpc, kQpcFreq, true, true, 120000ULL, &ctrlP);

        // Manual user Play at +1000ms must NOT be overridden once settled!
        pQpc += 550LL * kMs;
        ctrlP.game_heartbeat_qpc = pQpc;
        smtcP.snap.playbackStatus = routeb::kSmtcStatusPlaying;
        coordP.Tick(pQpc, kQpcFreq, true, true, 130000ULL, &ctrlP);
        pQpc += 500LL * kMs;
        ctrlP.game_heartbeat_qpc = pQpc;
        coordP.Tick(pQpc, kQpcFreq, true, true, 140000ULL, &ctrlP);
        CHECK(smtcP.pauseCount == 2u);
        CHECK(smtcP.snap.playbackStatus == routeb::kSmtcStatusPlaying);
    }
}

} // namespace

int main() {
    std::printf("=== Route B Offline Self-Test (%s) ===\n", (sizeof(void*) == 8) ? "x64" : "Win32");
    RunSuite1CtrlProtocolLayout();
    RunSuite2OffsetsAndSignatures();
    RunSuite3PlanarFraming();
    RunSuite4ConsumerPlanarHandoff();
    RunSuite5CoordinatorStateMachine();

    if (g_failures != 0) {
        std::fprintf(stderr, "\n[FAIL] %d assertion(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("\n[PASS] All 5 Route B offline suites passed with zero failures.\n");
    return 0;
}
