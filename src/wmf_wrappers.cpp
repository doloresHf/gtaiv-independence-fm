#include "wmf_wrappers.h"
#include "async_logger.h"
#include "stage_b_source.h"
#include "stage_c_ipc.h"

#include <mmsystem.h>
#include <mmreg.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace probe {

namespace {

volatile LONG g_nextReaderId = 0;

const char* KnownIidName(REFIID riid) {
    if (IsEqualIID(riid, IID_IUnknown)) return "IID_IUnknown";
    if (IsEqualIID(riid, IID_IWMSyncReader)) return "IID_IWMSyncReader";
    if (IsEqualIID(riid, IID_IWMSyncReader2)) return "IID_IWMSyncReader2";
    if (IsEqualIID(riid, IID_IWMReader)) return "IID_IWMReader";
    if (IsEqualIID(riid, IID_IWMReaderAdvanced)) return "IID_IWMReaderAdvanced";
    if (IsEqualIID(riid, IID_IWMReaderAdvanced2)) return "IID_IWMReaderAdvanced2";
    if (IsEqualIID(riid, IID_IWMHeaderInfo)) return "IID_IWMHeaderInfo";
    if (IsEqualIID(riid, IID_IWMHeaderInfo2)) return "IID_IWMHeaderInfo2";
    if (IsEqualIID(riid, IID_IWMHeaderInfo3)) return "IID_IWMHeaderInfo3";
    if (IsEqualIID(riid, IID_IWMProfile)) return "IID_IWMProfile";
    if (IsEqualIID(riid, IID_IWMProfile2)) return "IID_IWMProfile2";
    if (IsEqualIID(riid, IID_IWMProfile3)) return "IID_IWMProfile3";
    if (IsEqualIID(riid, IID_IWMReaderCallback)) return "IID_IWMReaderCallback";
    if (IsEqualIID(riid, IID_IWMReaderCallbackAdvanced)) return "IID_IWMReaderCallbackAdvanced";
    if (IsEqualIID(riid, IID_IWMStatusCallback)) return "IID_IWMStatusCallback";
    return nullptr;
}

bool ShouldLogSampleDetail(uint64_t seq, HRESULT hr, int64_t deltaWallUs) {
    if (seq <= 250) return true;
    if (FAILED(hr)) return true;
    if (deltaWallUs > 150000) return true; // >150ms gap (pause/stall/station switch)
    if ((seq % 25) == 0) return true;
    return false;
}

} // namespace

int32_t NextReaderId() {
    return static_cast<int32_t>(InterlockedIncrement(&g_nextReaderId));
}

void InspectOutputMediaProps(int32_t readerId, const char* kind, const char* stage, DWORD dwOutputNum, IWMOutputMediaProps* pProps) {
    if (!pProps) {
        return;
    }
    DWORD cbType = 0;
    HRESULT hr = pProps->GetMediaType(nullptr, &cbType);
    if (FAILED(hr) || cbType < sizeof(WM_MEDIA_TYPE)) {
        return;
    }
    std::vector<BYTE> buf(cbType, 0);
    auto* pmt = reinterpret_cast<WM_MEDIA_TYPE*>(buf.data());
    hr = pProps->GetMediaType(pmt, &cbType);
    if (FAILED(hr)) {
        return;
    }

    std::string subGuid = AsyncLogger::GuidToString(pmt->subtype);
    if (IsEqualGUID(pmt->formattype, WMFORMAT_WaveFormatEx) &&
        pmt->pbFormat != nullptr &&
        pmt->cbFormat >= sizeof(WAVEFORMATEX)) {
        const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(pmt->pbFormat);
        AsyncLogger::Instance().LogJsonEvent(
            stage, readerId, kind,
            "\"output_num\":%lu,\"subtype\":\"%s\",\"format_tag\":%u,\"channels\":%u,\"sample_rate\":%lu,\"avg_bytes_sec\":%lu,\"block_align\":%u,\"bits_per_sample\":%u",
            static_cast<unsigned long>(dwOutputNum),
            subGuid.c_str(),
            static_cast<unsigned>(wfx->wFormatTag),
            static_cast<unsigned>(wfx->nChannels),
            static_cast<unsigned long>(wfx->nSamplesPerSec),
            static_cast<unsigned long>(wfx->nAvgBytesPerSec),
            static_cast<unsigned>(wfx->nBlockAlign),
            static_cast<unsigned>(wfx->wBitsPerSample));
    } else {
        std::string majorGuid = AsyncLogger::GuidToString(pmt->majortype);
        AsyncLogger::Instance().LogJsonEvent(
            stage, readerId, kind,
            "\"output_num\":%lu,\"major\":\"%s\",\"subtype\":\"%s\",\"cb_format\":%lu",
            static_cast<unsigned long>(dwOutputNum),
            majorGuid.c_str(), subGuid.c_str(),
            static_cast<unsigned long>(pmt->cbFormat));
    }
}

void InspectAttributeValue(int32_t readerId, const char* kind, const char* api, WORD streamNum, const wchar_t* attrName,
                           WMT_ATTR_DATATYPE type, const BYTE* pValue, DWORD cbLength, HRESULT hr) {
    std::string nameUtf8 = AsyncLogger::WideToUtf8Escaped(attrName);
    char valDesc[256]{};
    if (SUCCEEDED(hr) && pValue != nullptr && cbLength > 0) {
        switch (type) {
        case WMT_TYPE_DWORD:
            if (cbLength >= sizeof(DWORD)) {
                std::snprintf(valDesc, sizeof(valDesc), "%lu", static_cast<unsigned long>(*reinterpret_cast<const DWORD*>(pValue)));
            }
            break;
        case WMT_TYPE_QWORD:
            if (cbLength >= sizeof(QWORD)) {
                std::snprintf(valDesc, sizeof(valDesc), "%llu", static_cast<unsigned long long>(*reinterpret_cast<const QWORD*>(pValue)));
            }
            break;
        case WMT_TYPE_WORD:
            if (cbLength >= sizeof(WORD)) {
                std::snprintf(valDesc, sizeof(valDesc), "%u", static_cast<unsigned>(*reinterpret_cast<const WORD*>(pValue)));
            }
            break;
        case WMT_TYPE_BOOL:
            if (cbLength >= sizeof(BOOL)) {
                std::snprintf(valDesc, sizeof(valDesc), "%s", (*reinterpret_cast<const BOOL*>(pValue)) ? "true" : "false");
            }
            break;
        case WMT_TYPE_STRING: {
            std::wstring ws(reinterpret_cast<const wchar_t*>(pValue), cbLength / sizeof(wchar_t));
            if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
            std::string s = AsyncLogger::WideToUtf8Escaped(ws.c_str());
            std::snprintf(valDesc, sizeof(valDesc), "%s", s.c_str());
            break;
        }
        default:
            std::snprintf(valDesc, sizeof(valDesc), "<binary:%lu>", static_cast<unsigned long>(cbLength));
            break;
        }
    } else {
        std::snprintf(valDesc, sizeof(valDesc), "<null_or_size_query:%lu>", static_cast<unsigned long>(cbLength));
    }

    AsyncLogger::Instance().LogJsonEvent(
        api, readerId, kind,
        "\"stream\":%u,\"attr\":\"%s\",\"attr_type\":%d,\"val\":\"%s\",\"cb\":%lu,\"hr\":\"0x%08lX\"",
        static_cast<unsigned>(streamNum), nameUtf8.c_str(), static_cast<int>(type),
        valDesc, static_cast<unsigned long>(cbLength), static_cast<unsigned long>(hr));
}

// ============================================================================
// WrappedSyncReader
// ============================================================================

WrappedSyncReader::WrappedSyncReader(IWMSyncReader* realReader)
    : readerId_(NextReaderId()), real_(realReader) {
    StageBManager::Instance().EnsureInitialized();
    AsyncLogger::Instance().LogJsonEvent("SyncReader_Created", readerId_, "sync", "");
}

WrappedSyncReader::~WrappedSyncReader() {
    if (ipcRegistered_) {
        StageCIpcManager::Instance().NotifyReaderEofOrClosed(readerId_);
        ipcRegistered_ = false;
    }
    AsyncLogger::Instance().LogJsonEvent(
        "SyncReader_Destroyed", readerId_, "sync",
        "\"total_samples\":%llu,\"total_bytes\":%llu,\"replaced_samples\":%llu,\"replaced_bytes\":%llu,\"passthrough_samples\":%llu",
        static_cast<unsigned long long>(sampleSeq_),
        static_cast<unsigned long long>(totalBytes_),
        static_cast<unsigned long long>(replacedSamples_),
        static_cast<unsigned long long>(replacedBytes_),
        static_cast<unsigned long long>(passthroughSamples_));
    if (realProfile_) realProfile_->Release();
    if (realHi3_) realHi3_->Release();
    if (realHi2_) realHi2_->Release();
    if (realHi_) realHi_->Release();
    if (real2_) real2_->Release();
    if (real_) real_->Release();
}

bool WrappedSyncReader::EnsureSync2() {
    if (real2_) return true;
    return SUCCEEDED(real_->QueryInterface(IID_IWMSyncReader2, reinterpret_cast<void**>(&real2_)));
}

bool WrappedSyncReader::EnsureHeaderInfo() {
    if (realHi_) return true;
    real_->QueryInterface(IID_IWMHeaderInfo, reinterpret_cast<void**>(&realHi_));
    real_->QueryInterface(IID_IWMHeaderInfo2, reinterpret_cast<void**>(&realHi2_));
    real_->QueryInterface(IID_IWMHeaderInfo3, reinterpret_cast<void**>(&realHi3_));
    return realHi_ != nullptr;
}

bool WrappedSyncReader::EnsureProfile() {
    if (realProfile_) return true;
    return SUCCEEDED(real_->QueryInterface(IID_IWMProfile, reinterpret_cast<void**>(&realProfile_)));
}

bool WrappedSyncReader::VerifyPcmFormat() {
    if (pcmFormatChecked_) {
        return pcmFormatValid_;
    }

    bool valid = false;
    IWMOutputMediaProps* props = nullptr;
    HRESULT hr = real_->GetOutputProps(0, &props);
    if (SUCCEEDED(hr) && props != nullptr) {
        DWORD cbType = 0;
        if (SUCCEEDED(props->GetMediaType(nullptr, &cbType)) && cbType >= sizeof(WM_MEDIA_TYPE)) {
            std::vector<BYTE> buf(cbType, 0);
            auto* pmt = reinterpret_cast<WM_MEDIA_TYPE*>(buf.data());
            if (SUCCEEDED(props->GetMediaType(pmt, &cbType))) {
                if (IsEqualGUID(pmt->majortype, WMMEDIATYPE_Audio) &&
                    IsEqualGUID(pmt->formattype, WMFORMAT_WaveFormatEx) &&
                    pmt->pbFormat != nullptr &&
                    pmt->cbFormat >= sizeof(WAVEFORMATEX)) {
                    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(pmt->pbFormat);
                    valid = (wfx->wFormatTag == WAVE_FORMAT_PCM &&
                             wfx->nSamplesPerSec == StageBManager::kExpectedSampleRate &&
                             wfx->nChannels == StageBManager::kExpectedChannels &&
                             wfx->wBitsPerSample == StageBManager::kExpectedBitsPerSample &&
                             wfx->nBlockAlign == StageBManager::kExpectedBlockAlign);
                }
            }
        }
        props->Release();
    }

    pcmFormatChecked_ = true;
    pcmFormatValid_ = valid;
    return pcmFormatValid_;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;

    const char* known = KnownIidName(riid);
    std::string guidStr = known ? known : AsyncLogger::GuidToString(riid);

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IWMSyncReader)) {
        *ppvObject = static_cast<IWMSyncReader*>(this);
        AddRef();
        AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "sync", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IWMSyncReader2)) {
        if (EnsureSync2()) {
            *ppvObject = static_cast<IWMSyncReader2*>(this);
            AddRef();
            AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "sync", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    if (IsEqualIID(riid, IID_IWMHeaderInfo) || IsEqualIID(riid, IID_IWMHeaderInfo2) || IsEqualIID(riid, IID_IWMHeaderInfo3)) {
        EnsureHeaderInfo();
        if ((IsEqualIID(riid, IID_IWMHeaderInfo) && realHi_) ||
            (IsEqualIID(riid, IID_IWMHeaderInfo2) && realHi2_) ||
            (IsEqualIID(riid, IID_IWMHeaderInfo3) && realHi3_)) {
            if (IsEqualIID(riid, IID_IWMHeaderInfo)) *ppvObject = static_cast<IWMHeaderInfo*>(this);
            else if (IsEqualIID(riid, IID_IWMHeaderInfo2)) *ppvObject = static_cast<IWMHeaderInfo2*>(this);
            else *ppvObject = static_cast<IWMHeaderInfo3*>(this);
            AddRef();
            AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "sync", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    if (IsEqualIID(riid, IID_IWMProfile)) {
        if (EnsureProfile()) {
            *ppvObject = static_cast<IWMProfile*>(this);
            AddRef();
            AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "sync", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    HRESULT hr = real_->QueryInterface(riid, ppvObject);
    AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "sync",
                                         "\"iid\":\"%s\",\"wrapped\":false,\"hr\":\"0x%08lX\"",
                                         guidStr.c_str(), static_cast<unsigned long>(hr));
    return hr;
}

ULONG STDMETHODCALLTYPE WrappedSyncReader::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

ULONG STDMETHODCALLTYPE WrappedSyncReader::Release() {
    ULONG r = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (r == 0) {
        delete this;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::Open(LPCWSTR pwszFilename) {
    int64_t t0 = AsyncLogger::Instance().NowMicros();
    HRESULT hr = real_->Open(pwszFilename);
    int64_t dt = AsyncLogger::Instance().NowMicros() - t0;
    if (SUCCEEDED(hr)) {
        if (ipcRegistered_) {
            StageCIpcManager::Instance().NotifyReaderEofOrClosed(readerId_);
            ipcRegistered_ = false;
        }
        openTid_ = GetCurrentThreadId();
        openSucceeded_ = true;
        isTargetTrack_ = StageBManager::Instance().MatchesActiveTargetTrack(pwszFilename);
        pcmFormatChecked_ = false;
        pcmFormatValid_ = false;
        decisionLogged_ = false;
        frameCursor_ = 0;
        seekPending_ = false;
        requestedSeek100ns_ = 0;
    } else {
        openSucceeded_ = false;
    }
    std::string pathUtf8 = AsyncLogger::WideToUtf8Escaped(pwszFilename);
    AsyncLogger::Instance().LogJsonEvent(
        "Open", readerId_, "sync",
        "\"path\":\"%s\",\"open_dur_us\":%lld,\"hr\":\"0x%08lX\"",
        pathUtf8.c_str(), static_cast<long long>(dt), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::Close() {
    if (ipcRegistered_) {
        StageCIpcManager::Instance().NotifyReaderEofOrClosed(readerId_);
        ipcRegistered_ = false;
    }
    HRESULT hr = real_->Close();
    AsyncLogger::Instance().LogJsonEvent(
        "Close", readerId_, "sync",
        "\"samples_so_far\":%llu,\"bytes_so_far\":%llu,\"replaced_samples\":%llu,\"replaced_bytes\":%llu,\"passthrough_samples\":%llu,\"hr\":\"0x%08lX\"",
        static_cast<unsigned long long>(sampleSeq_),
        static_cast<unsigned long long>(totalBytes_),
        static_cast<unsigned long long>(replacedSamples_),
        static_cast<unsigned long long>(replacedBytes_),
        static_cast<unsigned long long>(passthroughSamples_),
        static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetRange(QWORD cnsStartTime, LONGLONG cnsDuration) {
    HRESULT hr = real_->SetRange(cnsStartTime, cnsDuration);
    if (SUCCEEDED(hr)) {
        seekPending_ = true;
        requestedSeek100ns_ = cnsStartTime;
    }
    AsyncLogger::Instance().LogJsonEvent(
        "SetRange", readerId_, "sync",
        "\"start_100ns\":%llu,\"dur_100ns\":%lld,\"hr\":\"0x%08lX\"",
        static_cast<unsigned long long>(cnsStartTime),
        static_cast<long long>(cnsDuration),
        static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetRangeByFrame(WORD wStreamNum, QWORD qwFrameNumber, LONGLONG cFramesToRead) {
    HRESULT hr = real_->SetRangeByFrame(wStreamNum, qwFrameNumber, cFramesToRead);
    AsyncLogger::Instance().LogJsonEvent(
        "SetRangeByFrame", readerId_, "sync",
        "\"stream\":%u,\"frame\":%llu,\"count\":%lld,\"hr\":\"0x%08lX\"",
        static_cast<unsigned>(wStreamNum),
        static_cast<unsigned long long>(qwFrameNumber),
        static_cast<long long>(cFramesToRead),
        static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetNextSample(WORD wStreamNum, INSSBuffer** ppSample, QWORD* pcnsSampleTime,
                                                           QWORD* pcnsDuration, DWORD* pdwFlags, DWORD* pdwOutputNum, WORD* pwStreamNum) {
    int64_t nowUs = AsyncLogger::Instance().NowMicros();
    int64_t deltaWallUs = (lastSampleWallUs_ == 0) ? 0 : (nowUs - lastSampleWallUs_);
    lastSampleWallUs_ = nowUs;

    HRESULT hr = real_->GetNextSample(wStreamNum, ppSample, pcnsSampleTime, pcnsDuration, pdwFlags, pdwOutputNum, pwStreamNum);
    ++sampleSeq_;

    if ((hr == NS_E_NO_MORE_SAMPLES || FAILED(hr)) && ipcRegistered_) {
        StageCIpcManager::Instance().NotifyReaderEofOrClosed(readerId_);
        ipcRegistered_ = false;
    }

    DWORD sampleLen = 0;
    if (SUCCEEDED(hr) && ppSample && *ppSample) {
        (*ppSample)->GetLength(&sampleLen);
        totalBytes_ += sampleLen;
    }

    if (SUCCEEDED(hr)) {
        const StageBConfigSnapshot& cfg = StageBManager::Instance().GetConfig();
        const DWORD sampleTid = GetCurrentThreadId();
        const char* reason = "mode_off";
        bool eligible = false;

        if (cfg.effectiveMode != StageBMode::Synth &&
            cfg.effectiveMode != StageBMode::Wav &&
            cfg.effectiveMode != StageBMode::Ipc) {
            reason = (cfg.requestedMode == StageBMode::Wav)
                ? StageBManager::StatusToString(cfg.status)
                : "mode_off";
        } else if (!openSucceeded_ || !isTargetTrack_) {
            reason = openSucceeded_ ? "non_target_file" : "open_not_succeeded";
        } else if (openTid_ == 0 || sampleTid == openTid_) {
            reason = "same_thread_scan";
        } else if (!VerifyPcmFormat()) {
            reason = "unsupported_pcm_format";
        } else {
            eligible = true;
            if (cfg.effectiveMode == StageBMode::Wav) {
                reason = "wav_active";
            } else if (cfg.effectiveMode == StageBMode::Ipc) {
                reason = "ipc_active";
            } else {
                reason = "synth_active";
            }
        }

        if (!decisionLogged_) {
            decisionLogged_ = true;
            AsyncLogger::Instance().LogJsonEvent(
                "StageB_ReaderDecision", readerId_, "sync",
                "\"replace_active\":%s,\"mode\":\"%s\",\"reason\":\"%s\",\"open_tid\":%lu,\"sample_tid\":%lu,\"target_match\":%s",
                eligible ? "true" : "false",
                StageBManager::ModeToString(cfg.effectiveMode),
                reason,
                static_cast<unsigned long>(openTid_),
                static_cast<unsigned long>(sampleTid),
                isTargetTrack_ ? "true" : "false");
        }

        if (eligible) {
            if (cfg.effectiveMode == StageBMode::Ipc && !ipcRegistered_) {
                StageCIpcManager::Instance().NotifyReaderOpened(readerId_);
                ipcRegistered_ = true;
            }
            BYTE* pBuf = nullptr;
            DWORD cbLen = 0;
            bool bufOk = (ppSample && *ppSample &&
                          SUCCEEDED((*ppSample)->GetBufferAndLength(&pBuf, &cbLen)) &&
                          pBuf != nullptr &&
                          cbLen > 0 &&
                          (cbLen % StageBManager::kExpectedBlockAlign) == 0);
            if (!bufOk) {
                AsyncLogger::Instance().LogJsonEvent(
                    "StageB_SampleGuardSkip", readerId_, "sync",
                    "\"seq\":%llu,\"guard_reason\":\"bad_buffer_or_align\",\"bytes\":%lu",
                    static_cast<unsigned long long>(sampleSeq_),
                    static_cast<unsigned long>(cbLen));
                ++passthroughSamples_;
            } else {
                const uint32_t frameCount = cbLen / StageBManager::kExpectedBlockAlign;
                if (cfg.effectiveMode == StageBMode::Ipc) {
                    seekPending_ = false;
                    StageCPullResult pullRes = StageCIpcManager::Instance().PullFramesForReader(
                        readerId_, sampleSeq_, frameCount, reinterpret_cast<int16_t*>(pBuf));
                    if (pullRes.filled) {
                        ++replacedSamples_;
                        replacedBytes_ += cbLen;
                    } else {
                        ++passthroughSamples_;
                    }
                } else {
                    if (seekPending_) {
                        QWORD actualPts = pcnsSampleTime ? *pcnsSampleTime : requestedSeek100ns_;
                        frameCursor_ = (actualPts * 48000ULL + 5000000ULL) / 10000000ULL;
                        seekPending_ = false;
                        AsyncLogger::Instance().LogJsonEvent(
                            "StageB_SeekAligned", readerId_, "sync",
                            "\"req_start_100ns\":%llu,\"actual_pts_100ns\":%llu,\"aligned_frame\":%llu",
                            static_cast<unsigned long long>(requestedSeek100ns_),
                            static_cast<unsigned long long>(actualPts),
                            static_cast<unsigned long long>(frameCursor_));
                    }
                    bool wroteReplacement = false;
                    if (cfg.effectiveMode == StageBMode::Wav) {
                        wroteReplacement = StageBManager::Instance().CopyWavFrames(
                            frameCursor_, frameCount, reinterpret_cast<int16_t*>(pBuf));
                        if (!wroteReplacement) {
                            AsyncLogger::Instance().LogJsonEvent(
                                "StageB_SampleGuardSkip", readerId_, "sync",
                                "\"seq\":%llu,\"guard_reason\":\"wav_out_of_range\",\"bytes\":%lu",
                                static_cast<unsigned long long>(sampleSeq_),
                                static_cast<unsigned long>(cbLen));
                            ++passthroughSamples_;
                        }
                    } else {
                        StageBManager::RenderSynthFrames(frameCursor_, frameCount, reinterpret_cast<int16_t*>(pBuf));
                        wroteReplacement = true;
                    }

                    if (wroteReplacement) {
                        frameCursor_ += frameCount;
                        ++replacedSamples_;
                        replacedBytes_ += cbLen;
                    }
                }
            }
        } else {
            ++passthroughSamples_;
        }
    }

    if (ShouldLogSampleDetail(sampleSeq_, hr, deltaWallUs)) {
        AsyncLogger::Instance().LogJsonEvent(
            "GetNextSample", readerId_, "sync",
            "\"seq\":%llu,\"delta_wall_us\":%lld,\"req_stream\":%u,\"out_num\":%lu,\"out_stream\":%u,\"pts_100ns\":%llu,\"dur_100ns\":%llu,\"bytes\":%lu,\"flags\":%lu,\"total_bytes\":%llu,\"hr\":\"0x%08lX\"",
            static_cast<unsigned long long>(sampleSeq_),
            static_cast<long long>(deltaWallUs),
            static_cast<unsigned>(wStreamNum),
            static_cast<unsigned long>(pdwOutputNum ? *pdwOutputNum : 0),
            static_cast<unsigned>(pwStreamNum ? *pwStreamNum : 0),
            static_cast<unsigned long long>(pcnsSampleTime ? *pcnsSampleTime : 0),
            static_cast<unsigned long long>(pcnsDuration ? *pcnsDuration : 0),
            static_cast<unsigned long>(sampleLen),
            static_cast<unsigned long>(pdwFlags ? *pdwFlags : 0),
            static_cast<unsigned long long>(totalBytes_),
            static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetStreamsSelected(WORD cStreamCount, WORD* pwStreamNumbers, WMT_STREAM_SELECTION* pSelections) {
    HRESULT hr = real_->SetStreamsSelected(cStreamCount, pwStreamNumbers, pSelections);
    AsyncLogger::Instance().LogJsonEvent("SetStreamsSelected", readerId_, "sync", "\"count\":%u,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned>(cStreamCount), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetStreamSelected(WORD wStreamNum, WMT_STREAM_SELECTION* pSelection) {
    return real_->GetStreamSelected(wStreamNum, pSelection);
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetReadStreamSamples(WORD wStreamNum, BOOL fCompressed) {
    HRESULT hr = real_->SetReadStreamSamples(wStreamNum, fCompressed);
    AsyncLogger::Instance().LogJsonEvent("SetReadStreamSamples", readerId_, "sync",
                                         "\"stream\":%u,\"compressed\":%s,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned>(wStreamNum), fCompressed ? "true" : "false",
                                         static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetReadStreamSamples(WORD wStreamNum, BOOL* pfCompressed) {
    return real_->GetReadStreamSamples(wStreamNum, pfCompressed);
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) {
    HRESULT hr = real_->GetOutputSetting(dwOutputNum, pszName, pType, pValue, pcbLength);
    std::string s = AsyncLogger::WideToUtf8Escaped(pszName);
    AsyncLogger::Instance().LogJsonEvent("GetOutputSetting", readerId_, "sync", "\"out\":%lu,\"name\":\"%s\",\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(dwOutputNum), s.c_str(), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) {
    HRESULT hr = real_->SetOutputSetting(dwOutputNum, pszName, Type, pValue, cbLength);
    std::string s = AsyncLogger::WideToUtf8Escaped(pszName);
    AsyncLogger::Instance().LogJsonEvent("SetOutputSetting", readerId_, "sync", "\"out\":%lu,\"name\":\"%s\",\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(dwOutputNum), s.c_str(), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetOutputCount(DWORD* pcOutputs) {
    HRESULT hr = real_->GetOutputCount(pcOutputs);
    AsyncLogger::Instance().LogJsonEvent("GetOutputCount", readerId_, "sync", "\"count\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(pcOutputs ? *pcOutputs : 0), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps** ppOutput) {
    HRESULT hr = real_->GetOutputProps(dwOutputNum, ppOutput);
    if (SUCCEEDED(hr) && ppOutput && *ppOutput) {
        InspectOutputMediaProps(readerId_, "sync", "GetOutputProps", dwOutputNum, *ppOutput);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps* pOutput) {
    InspectOutputMediaProps(readerId_, "sync", "SetOutputProps_Req", dwOutputNum, pOutput);
    HRESULT hr = real_->SetOutputProps(dwOutputNum, pOutput);
    if (dwOutputNum == 0 && SUCCEEDED(hr)) {
        pcmFormatChecked_ = false;
        pcmFormatValid_ = false;
        VerifyPcmFormat();
    }
    AsyncLogger::Instance().LogJsonEvent("SetOutputProps", readerId_, "sync", "\"output_num\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(dwOutputNum), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetOutputFormatCount(DWORD dwOutputNum, DWORD* pcFormats) {
    HRESULT hr = real_->GetOutputFormatCount(dwOutputNum, pcFormats);
    AsyncLogger::Instance().LogJsonEvent("GetOutputFormatCount", readerId_, "sync", "\"output_num\":%lu,\"count\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(dwOutputNum),
                                         static_cast<unsigned long>(pcFormats ? *pcFormats : 0),
                                         static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetOutputFormat(DWORD dwOutputNum, DWORD dwFormatNum, IWMOutputMediaProps** ppProps) {
    return real_->GetOutputFormat(dwOutputNum, dwFormatNum, ppProps);
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetOutputNumberForStream(WORD wStreamNum, DWORD* pdwOutputNum) {
    return real_->GetOutputNumberForStream(wStreamNum, pdwOutputNum);
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetStreamNumberForOutput(DWORD dwOutputNum, WORD* pwStreamNum) {
    return real_->GetStreamNumberForOutput(dwOutputNum, pwStreamNum);
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetMaxOutputSampleSize(DWORD dwOutput, DWORD* pcbMax) {
    HRESULT hr = real_->GetMaxOutputSampleSize(dwOutput, pcbMax);
    AsyncLogger::Instance().LogJsonEvent("GetMaxOutputSampleSize", readerId_, "sync", "\"out\":%lu,\"max_bytes\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(dwOutput),
                                         static_cast<unsigned long>(pcbMax ? *pcbMax : 0),
                                         static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetMaxStreamSampleSize(WORD wStream, DWORD* pcbMax) {
    HRESULT hr = real_->GetMaxStreamSampleSize(wStream, pcbMax);
    AsyncLogger::Instance().LogJsonEvent("GetMaxStreamSampleSize", readerId_, "sync", "\"stream\":%u,\"max_bytes\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned>(wStream),
                                         static_cast<unsigned long>(pcbMax ? *pcbMax : 0),
                                         static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedSyncReader::OpenStream(IStream* pStream) {
    HRESULT hr = real_->OpenStream(pStream);
    AsyncLogger::Instance().LogJsonEvent("OpenStream", readerId_, "sync", "\"stream_ptr\":\"0x%p\",\"hr\":\"0x%08lX\"",
                                         pStream, static_cast<unsigned long>(hr));
    return hr;
}

// IWMSyncReader2
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetRangeByTimecode(WORD wStreamNum, WMT_TIMECODE_EXTENSION_DATA* pStart, WMT_TIMECODE_EXTENSION_DATA* pEnd) {
    if (!EnsureSync2()) return E_NOINTERFACE;
    return real2_->SetRangeByTimecode(wStreamNum, pStart, pEnd);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetRangeByFrameEx(WORD wStreamNum, QWORD qwFrameNumber, LONGLONG cFramesToRead, QWORD* pcnsStartTime) {
    if (!EnsureSync2()) return E_NOINTERFACE;
    return real2_->SetRangeByFrameEx(wStreamNum, qwFrameNumber, cFramesToRead, pcnsStartTime);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetAllocateForOutput(DWORD dwOutputNum, IWMReaderAllocatorEx* pAllocator) {
    if (!EnsureSync2()) return E_NOINTERFACE;
    AsyncLogger::Instance().LogJsonEvent("SetAllocateForOutput", readerId_, "sync", "\"out\":%lu", static_cast<unsigned long>(dwOutputNum));
    return real2_->SetAllocateForOutput(dwOutputNum, pAllocator);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAllocateForOutput(DWORD dwOutputNum, IWMReaderAllocatorEx** ppAllocator) {
    if (!EnsureSync2()) return E_NOINTERFACE;
    return real2_->GetAllocateForOutput(dwOutputNum, ppAllocator);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetAllocateForStream(WORD wStreamNum, IWMReaderAllocatorEx* pAllocator) {
    if (!EnsureSync2()) return E_NOINTERFACE;
    return real2_->SetAllocateForStream(wStreamNum, pAllocator);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAllocateForStream(WORD wStreamNum, IWMReaderAllocatorEx** ppAllocator) {
    if (!EnsureSync2()) return E_NOINTERFACE;
    return real2_->GetAllocateForStream(wStreamNum, ppAllocator);
}

// IWMHeaderInfo / 2 / 3 on SyncReader
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAttributeCount(WORD wStreamNum, WORD* pcAttributes) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->GetAttributeCount(wStreamNum, pcAttributes);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAttributeByIndex(WORD wIndex, WORD* pwStreamNum, WCHAR* pwszName, WORD* pcchNameLen,
                                                                 WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    HRESULT hr = realHi_->GetAttributeByIndex(wIndex, pwStreamNum, pwszName, pcchNameLen, pType, pValue, pcbLength);
    InspectAttributeValue(readerId_, "sync", "GetAttributeByIndex", pwStreamNum ? *pwStreamNum : 0, pwszName,
                          pType ? *pType : WMT_TYPE_BINARY, pValue, pcbLength ? *pcbLength : 0, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAttributeByName(WORD* pwStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType,
                                                                BYTE* pValue, WORD* pcbLength) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    HRESULT hr = realHi_->GetAttributeByName(pwStreamNum, pszName, pType, pValue, pcbLength);
    InspectAttributeValue(readerId_, "sync", "GetAttributeByName", pwStreamNum ? *pwStreamNum : 0, pszName,
                          pType ? *pType : WMT_TYPE_BINARY, pValue, pcbLength ? *pcbLength : 0, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetAttribute(WORD wStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->SetAttribute(wStreamNum, pszName, Type, pValue, cbLength);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetMarkerCount(WORD* pcMarkers) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->GetMarkerCount(pcMarkers);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetMarker(WORD wIndex, WCHAR* pwszMarkerName, WORD* pcchMarkerNameLen, QWORD* pcnsMarkerTime) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->GetMarker(wIndex, pwszMarkerName, pcchMarkerNameLen, pcnsMarkerTime);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::AddMarker(LPWSTR pwszMarkerName, QWORD cnsMarkerTime) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->AddMarker(pwszMarkerName, cnsMarkerTime);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::RemoveMarker(WORD wIndex) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->RemoveMarker(wIndex);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetScriptCount(WORD* pcScripts) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->GetScriptCount(pcScripts);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetScript(WORD wIndex, WCHAR* pwszType, WORD* pcchTypeLen, WCHAR* pwszCommand,
                                                       WORD* pcchCommandLen, QWORD* pcnsScriptTime) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->GetScript(wIndex, pwszType, pcchTypeLen, pwszCommand, pcchCommandLen, pcnsScriptTime);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::AddScript(LPWSTR pwszType, LPWSTR pwszCommand, QWORD cnsScriptTime) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->AddScript(pwszType, pwszCommand, cnsScriptTime);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::RemoveScript(WORD wIndex) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    return realHi_->RemoveScript(wIndex);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetCodecInfoCount(DWORD* pcCodecInfos) {
    if (!EnsureHeaderInfo() || !realHi2_) return E_NOINTERFACE;
    return realHi2_->GetCodecInfoCount(pcCodecInfos);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetCodecInfo(DWORD wIndex, WORD* pcchName, WCHAR* pwszName, WORD* pcchDescription,
                                                          WCHAR* pwszDescription, WMT_CODEC_INFO_TYPE* pCodecType, WORD* pcbCodecInfo, BYTE* pbCodecInfo) {
    if (!EnsureHeaderInfo() || !realHi2_) return E_NOINTERFACE;
    return realHi2_->GetCodecInfo(wIndex, pcchName, pwszName, pcchDescription, pwszDescription, pCodecType, pcbCodecInfo, pbCodecInfo);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAttributeCountEx(WORD wStreamNum, WORD* pcAttributes) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    return realHi3_->GetAttributeCountEx(wStreamNum, pcAttributes);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAttributeIndices(WORD wStreamNum, LPCWSTR pwszName, WORD* pwLangIndex, WORD* pwIndices, WORD* pwCount) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    HRESULT hr = realHi3_->GetAttributeIndices(wStreamNum, pwszName, pwLangIndex, pwIndices, pwCount);
    std::string s = AsyncLogger::WideToUtf8Escaped(pwszName);
    AsyncLogger::Instance().LogJsonEvent(
        "GetAttributeIndices", readerId_, "sync",
        "\"stream\":%u,\"attr\":\"%s\",\"count\":%u,\"first_idx\":%u,\"hr\":\"0x%08lX\"",
        static_cast<unsigned>(wStreamNum), s.c_str(),
        static_cast<unsigned>(pwCount ? *pwCount : 0),
        static_cast<unsigned>((pwIndices && pwCount && *pwCount > 0) ? pwIndices[0] : 65535),
        static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetAttributeByIndexEx(WORD wStreamNum, WORD wIndex, LPWSTR pwszName, WORD* pwNameLen,
                                                                   WMT_ATTR_DATATYPE* pType, WORD* pwLangIndex, BYTE* pValue, DWORD* pdwDataLength) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    HRESULT hr = realHi3_->GetAttributeByIndexEx(wStreamNum, wIndex, pwszName, pwNameLen, pType, pwLangIndex, pValue, pdwDataLength);
    InspectAttributeValue(readerId_, "sync", "GetAttributeByIndexEx", wStreamNum, pwszName,
                          pType ? *pType : WMT_TYPE_BINARY, pValue, pdwDataLength ? *pdwDataLength : 0, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::ModifyAttribute(WORD wStreamNum, WORD wIndex, WMT_ATTR_DATATYPE Type, WORD wLangIndex,
                                                             const BYTE* pValue, DWORD dwLength) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    return realHi3_->ModifyAttribute(wStreamNum, wIndex, Type, wLangIndex, pValue, dwLength);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::AddAttribute(WORD wStreamNum, LPCWSTR pszName, WORD* pwIndex, WMT_ATTR_DATATYPE Type,
                                                          WORD wLangIndex, const BYTE* pValue, DWORD dwLength) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    return realHi3_->AddAttribute(wStreamNum, pszName, pwIndex, Type, wLangIndex, pValue, dwLength);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::DeleteAttribute(WORD wStreamNum, WORD wIndex) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    return realHi3_->DeleteAttribute(wStreamNum, wIndex);
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::AddCodecInfo(LPWSTR pwszName, LPWSTR pwszDescription, WMT_CODEC_INFO_TYPE codecType,
                                                          WORD cbCodecInfo, BYTE* pbCodecInfo) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    return realHi3_->AddCodecInfo(pwszName, pwszDescription, codecType, cbCodecInfo, pbCodecInfo);
}

// IWMProfile on SyncReader
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetVersion(WMT_VERSION* pdwVersion) { return EnsureProfile() ? realProfile_->GetVersion(pdwVersion) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetName(WCHAR* pwszName, DWORD* pcchName) { return EnsureProfile() ? realProfile_->GetName(pwszName, pcchName) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetName(const WCHAR* pwszName) { return EnsureProfile() ? realProfile_->SetName(pwszName) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetDescription(WCHAR* pwszDescription, DWORD* pcchDescription) { return EnsureProfile() ? realProfile_->GetDescription(pwszDescription, pcchDescription) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::SetDescription(const WCHAR* pwszDescription) { return EnsureProfile() ? realProfile_->SetDescription(pwszDescription) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetStreamCount(DWORD* pcStreams) {
    if (!EnsureProfile()) return E_NOINTERFACE;
    HRESULT hr = realProfile_->GetStreamCount(pcStreams);
    AsyncLogger::Instance().LogJsonEvent("Profile_GetStreamCount", readerId_, "sync", "\"count\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(pcStreams ? *pcStreams : 0), static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetStream(DWORD dwStreamIndex, IWMStreamConfig** ppConfig) { return EnsureProfile() ? realProfile_->GetStream(dwStreamIndex, ppConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetStreamByNumber(WORD wStreamNum, IWMStreamConfig** ppConfig) { return EnsureProfile() ? realProfile_->GetStreamByNumber(wStreamNum, ppConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::RemoveStream(IWMStreamConfig* pConfig) { return EnsureProfile() ? realProfile_->RemoveStream(pConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::RemoveStreamByNumber(WORD wStreamNum) { return EnsureProfile() ? realProfile_->RemoveStreamByNumber(wStreamNum) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::AddStream(IWMStreamConfig* pConfig) { return EnsureProfile() ? realProfile_->AddStream(pConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::ReconfigStream(IWMStreamConfig* pConfig) { return EnsureProfile() ? realProfile_->ReconfigStream(pConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::CreateNewStream(REFGUID guidStreamType, IWMStreamConfig** ppConfig) { return EnsureProfile() ? realProfile_->CreateNewStream(guidStreamType, ppConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetMutualExclusionCount(DWORD* pcME) { return EnsureProfile() ? realProfile_->GetMutualExclusionCount(pcME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::GetMutualExclusion(DWORD dwMEIndex, IWMMutualExclusion** ppME) { return EnsureProfile() ? realProfile_->GetMutualExclusion(dwMEIndex, ppME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::RemoveMutualExclusion(IWMMutualExclusion* pME) { return EnsureProfile() ? realProfile_->RemoveMutualExclusion(pME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::AddMutualExclusion(IWMMutualExclusion* pME) { return EnsureProfile() ? realProfile_->AddMutualExclusion(pME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedSyncReader::CreateNewMutualExclusion(IWMMutualExclusion** ppME) { return EnsureProfile() ? realProfile_->CreateNewMutualExclusion(ppME) : E_NOINTERFACE; }

// ============================================================================
// WrappedReaderCallback (for Async IWMReader)
// ============================================================================

WrappedReaderCallback::WrappedReaderCallback(int32_t readerId, IWMReaderCallback* realCb)
    : readerId_(readerId), realCb_(realCb) {
    if (realCb_) {
        realCb_->AddRef();
        realCb_->QueryInterface(IID_IWMReaderCallbackAdvanced, reinterpret_cast<void**>(&realCbAdv_));
    }
}

WrappedReaderCallback::~WrappedReaderCallback() {
    if (realCbAdv_) realCbAdv_->Release();
    if (realCb_) realCb_->Release();
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IWMStatusCallback) || IsEqualIID(riid, IID_IWMReaderCallback)) {
        *ppvObject = static_cast<IWMReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IWMReaderCallbackAdvanced) && realCbAdv_) {
        *ppvObject = static_cast<IWMReaderCallbackAdvanced*>(this);
        AddRef();
        return S_OK;
    }
    return realCb_ ? realCb_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedReaderCallback::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

ULONG STDMETHODCALLTYPE WrappedReaderCallback::Release() {
    ULONG r = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (r == 0) delete this;
    return r;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::OnStatus(WMT_STATUS Status, HRESULT hr, WMT_ATTR_DATATYPE dwType,
                                                          BYTE* pValue, void* pvContext) {
    AsyncLogger::Instance().LogJsonEvent("Async_OnStatus", readerId_, "async",
                                         "\"status\":%d,\"hr\":\"0x%08lX\",\"attr_type\":%d",
                                         static_cast<int>(Status), static_cast<unsigned long>(hr), static_cast<int>(dwType));
    return realCb_ ? realCb_->OnStatus(Status, hr, dwType, pValue, pvContext) : S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::OnSample(DWORD dwOutputNum, QWORD cnsSampleTime, QWORD cnsSampleDuration,
                                                          DWORD dwFlags, INSSBuffer* pSample, void* pvContext) {
    int64_t nowUs = AsyncLogger::Instance().NowMicros();
    int64_t deltaWallUs = (lastSampleWallUs_ == 0) ? 0 : (nowUs - lastSampleWallUs_);
    lastSampleWallUs_ = nowUs;
    ++sampleSeq_;

    DWORD sampleLen = 0;
    if (pSample) pSample->GetLength(&sampleLen);

    if (ShouldLogSampleDetail(sampleSeq_, S_OK, deltaWallUs)) {
        AsyncLogger::Instance().LogJsonEvent(
            "Async_OnSample", readerId_, "async",
            "\"seq\":%llu,\"delta_wall_us\":%lld,\"out_num\":%lu,\"pts_100ns\":%llu,\"dur_100ns\":%llu,\"bytes\":%lu,\"flags\":%lu",
            static_cast<unsigned long long>(sampleSeq_),
            static_cast<long long>(deltaWallUs),
            static_cast<unsigned long>(dwOutputNum),
            static_cast<unsigned long long>(cnsSampleTime),
            static_cast<unsigned long long>(cnsSampleDuration),
            static_cast<unsigned long>(sampleLen),
            static_cast<unsigned long>(dwFlags));
    }
    return realCb_ ? realCb_->OnSample(dwOutputNum, cnsSampleTime, cnsSampleDuration, dwFlags, pSample, pvContext) : S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::OnStreamSample(WORD wStreamNum, QWORD cnsSampleTime, QWORD cnsSampleDuration,
                                                                DWORD dwFlags, INSSBuffer* pSample, void* pvContext) {
    DWORD sampleLen = 0;
    if (pSample) pSample->GetLength(&sampleLen);
    ++sampleSeq_;
    if (sampleSeq_ <= 200 || (sampleSeq_ % 25) == 0) {
        AsyncLogger::Instance().LogJsonEvent(
            "Async_OnStreamSample", readerId_, "async",
            "\"seq\":%llu,\"stream\":%u,\"pts_100ns\":%llu,\"dur_100ns\":%llu,\"bytes\":%lu,\"flags\":%lu",
            static_cast<unsigned long long>(sampleSeq_), static_cast<unsigned>(wStreamNum),
            static_cast<unsigned long long>(cnsSampleTime), static_cast<unsigned long long>(cnsSampleDuration),
            static_cast<unsigned long>(sampleLen), static_cast<unsigned long>(dwFlags));
    }
    return realCbAdv_ ? realCbAdv_->OnStreamSample(wStreamNum, cnsSampleTime, cnsSampleDuration, dwFlags, pSample, pvContext) : S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::OnTime(QWORD cnsCurrentTime, void* pvContext) {
    AsyncLogger::Instance().LogJsonEvent("Async_OnTime", readerId_, "async", "\"time_100ns\":%llu",
                                         static_cast<unsigned long long>(cnsCurrentTime));
    return realCbAdv_ ? realCbAdv_->OnTime(cnsCurrentTime, pvContext) : S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::OnStreamSelection(WORD wStreamCount, WORD* pStreamNumbers,
                                                                   WMT_STREAM_SELECTION* pSelections, void* pvContext) {
    return realCbAdv_ ? realCbAdv_->OnStreamSelection(wStreamCount, pStreamNumbers, pSelections, pvContext) : S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::OnOutputPropsChanged(DWORD dwOutputNum, WM_MEDIA_TYPE* pMediaType, void* pvContext) {
    return realCbAdv_ ? realCbAdv_->OnOutputPropsChanged(dwOutputNum, pMediaType, pvContext) : S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::AllocateForStream(WORD wStreamNum, DWORD cbBuffer, INSSBuffer** ppBuffer, void* pvContext) {
    return realCbAdv_ ? realCbAdv_->AllocateForStream(wStreamNum, cbBuffer, ppBuffer, pvContext) : E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE WrappedReaderCallback::AllocateForOutput(DWORD dwOutputNum, DWORD cbBuffer, INSSBuffer** ppBuffer, void* pvContext) {
    return realCbAdv_ ? realCbAdv_->AllocateForOutput(dwOutputNum, cbBuffer, ppBuffer, pvContext) : E_NOTIMPL;
}

// ============================================================================
// WrappedAsyncReader
// ============================================================================

WrappedAsyncReader::WrappedAsyncReader(IWMReader* realReader)
    : readerId_(NextReaderId()), real_(realReader) {
    AsyncLogger::Instance().LogJsonEvent("AsyncReader_Created", readerId_, "async", "");
}

WrappedAsyncReader::~WrappedAsyncReader() {
    AsyncLogger::Instance().LogJsonEvent("AsyncReader_Destroyed", readerId_, "async", "");
    if (realProfile_) realProfile_->Release();
    if (realHi3_) realHi3_->Release();
    if (realHi2_) realHi2_->Release();
    if (realHi_) realHi_->Release();
    if (realAdv2_) realAdv2_->Release();
    if (realAdv_) realAdv_->Release();
    if (real_) real_->Release();
}

bool WrappedAsyncReader::EnsureAdv() {
    if (realAdv_) return true;
    real_->QueryInterface(IID_IWMReaderAdvanced, reinterpret_cast<void**>(&realAdv_));
    real_->QueryInterface(IID_IWMReaderAdvanced2, reinterpret_cast<void**>(&realAdv2_));
    return realAdv_ != nullptr;
}

bool WrappedAsyncReader::EnsureHeaderInfo() {
    if (realHi_) return true;
    real_->QueryInterface(IID_IWMHeaderInfo, reinterpret_cast<void**>(&realHi_));
    real_->QueryInterface(IID_IWMHeaderInfo2, reinterpret_cast<void**>(&realHi2_));
    real_->QueryInterface(IID_IWMHeaderInfo3, reinterpret_cast<void**>(&realHi3_));
    return realHi_ != nullptr;
}

bool WrappedAsyncReader::EnsureProfile() {
    if (realProfile_) return true;
    return SUCCEEDED(real_->QueryInterface(IID_IWMProfile, reinterpret_cast<void**>(&realProfile_)));
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;

    const char* known = KnownIidName(riid);
    std::string guidStr = known ? known : AsyncLogger::GuidToString(riid);

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IWMReader)) {
        *ppvObject = static_cast<IWMReader*>(this);
        AddRef();
        AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "async", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IWMReaderAdvanced) || IsEqualIID(riid, IID_IWMReaderAdvanced2)) {
        EnsureAdv();
        if ((IsEqualIID(riid, IID_IWMReaderAdvanced) && realAdv_) ||
            (IsEqualIID(riid, IID_IWMReaderAdvanced2) && realAdv2_)) {
            if (IsEqualIID(riid, IID_IWMReaderAdvanced)) *ppvObject = static_cast<IWMReaderAdvanced*>(this);
            else *ppvObject = static_cast<IWMReaderAdvanced2*>(this);
            AddRef();
            AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "async", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    if (IsEqualIID(riid, IID_IWMHeaderInfo) || IsEqualIID(riid, IID_IWMHeaderInfo2) || IsEqualIID(riid, IID_IWMHeaderInfo3)) {
        EnsureHeaderInfo();
        if ((IsEqualIID(riid, IID_IWMHeaderInfo) && realHi_) ||
            (IsEqualIID(riid, IID_IWMHeaderInfo2) && realHi2_) ||
            (IsEqualIID(riid, IID_IWMHeaderInfo3) && realHi3_)) {
            if (IsEqualIID(riid, IID_IWMHeaderInfo)) *ppvObject = static_cast<IWMHeaderInfo*>(this);
            else if (IsEqualIID(riid, IID_IWMHeaderInfo2)) *ppvObject = static_cast<IWMHeaderInfo2*>(this);
            else *ppvObject = static_cast<IWMHeaderInfo3*>(this);
            AddRef();
            AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "async", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    if (IsEqualIID(riid, IID_IWMProfile)) {
        if (EnsureProfile()) {
            *ppvObject = static_cast<IWMProfile*>(this);
            AddRef();
            AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "async", "\"iid\":\"%s\",\"wrapped\":true,\"hr\":\"0x00000000\"", guidStr.c_str());
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    HRESULT hr = real_->QueryInterface(riid, ppvObject);
    AsyncLogger::Instance().LogJsonEvent("QueryInterface", readerId_, "async",
                                         "\"iid\":\"%s\",\"wrapped\":false,\"hr\":\"0x%08lX\"",
                                         guidStr.c_str(), static_cast<unsigned long>(hr));
    return hr;
}

ULONG STDMETHODCALLTYPE WrappedAsyncReader::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

ULONG STDMETHODCALLTYPE WrappedAsyncReader::Release() {
    ULONG r = static_cast<ULONG>(InterlockedDecrement(&refCount_));
    if (r == 0) delete this;
    return r;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Open(LPCWSTR pwszURL, IWMReaderCallback* pCallback, void* pvContext) {
    std::string urlUtf8 = AsyncLogger::WideToUtf8Escaped(pwszURL);
    auto* wrappedCb = pCallback ? new WrappedReaderCallback(readerId_, pCallback) : nullptr;
    HRESULT hr = real_->Open(pwszURL, wrappedCb, pvContext);
    if (wrappedCb) wrappedCb->Release(); // real_ holds its own reference if Open succeeded
    AsyncLogger::Instance().LogJsonEvent("Open", readerId_, "async", "\"url\":\"%s\",\"hr\":\"0x%08lX\"",
                                         urlUtf8.c_str(), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Close() {
    HRESULT hr = real_->Close();
    AsyncLogger::Instance().LogJsonEvent("Close", readerId_, "async", "\"hr\":\"0x%08lX\"", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetOutputCount(DWORD* pcOutputs) {
    HRESULT hr = real_->GetOutputCount(pcOutputs);
    AsyncLogger::Instance().LogJsonEvent("GetOutputCount", readerId_, "async", "\"count\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(pcOutputs ? *pcOutputs : 0), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps** ppOutput) {
    HRESULT hr = real_->GetOutputProps(dwOutputNum, ppOutput);
    if (SUCCEEDED(hr) && ppOutput && *ppOutput) {
        InspectOutputMediaProps(readerId_, "async", "GetOutputProps", dwOutputNum, *ppOutput);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps* pOutput) {
    InspectOutputMediaProps(readerId_, "async", "SetOutputProps_Req", dwOutputNum, pOutput);
    HRESULT hr = real_->SetOutputProps(dwOutputNum, pOutput);
    AsyncLogger::Instance().LogJsonEvent("SetOutputProps", readerId_, "async", "\"output_num\":%lu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long>(dwOutputNum), static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetOutputFormatCount(DWORD dwOutputNum, DWORD* pcFormats) {
    return real_->GetOutputFormatCount(dwOutputNum, pcFormats);
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetOutputFormat(DWORD dwOutputNum, DWORD dwFormatNum, IWMOutputMediaProps** ppProps) {
    return real_->GetOutputFormat(dwOutputNum, dwFormatNum, ppProps);
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Start(QWORD cnsStart, QWORD cnsDuration, float fRate, void* pvContext) {
    HRESULT hr = real_->Start(cnsStart, cnsDuration, fRate, pvContext);
    AsyncLogger::Instance().LogJsonEvent("Start", readerId_, "async",
                                         "\"start_100ns\":%llu,\"dur_100ns\":%llu,\"rate\":%.3f,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long long>(cnsStart),
                                         static_cast<unsigned long long>(cnsDuration),
                                         static_cast<double>(fRate),
                                         static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Stop() {
    HRESULT hr = real_->Stop();
    AsyncLogger::Instance().LogJsonEvent("Stop", readerId_, "async", "\"hr\":\"0x%08lX\"", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Pause() {
    HRESULT hr = real_->Pause();
    AsyncLogger::Instance().LogJsonEvent("Pause", readerId_, "async", "\"hr\":\"0x%08lX\"", static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Resume() {
    HRESULT hr = real_->Resume();
    AsyncLogger::Instance().LogJsonEvent("Resume", readerId_, "async", "\"hr\":\"0x%08lX\"", static_cast<unsigned long>(hr));
    return hr;
}

// IWMReaderAdvanced / 2
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetUserProvidedClock(BOOL fUserClock) {
    if (!EnsureAdv()) return E_NOINTERFACE;
    HRESULT hr = realAdv_->SetUserProvidedClock(fUserClock);
    AsyncLogger::Instance().LogJsonEvent("SetUserProvidedClock", readerId_, "async", "\"user_clock\":%s,\"hr\":\"0x%08lX\"",
                                         fUserClock ? "true" : "false", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetUserProvidedClock(BOOL* pfUserClock) {
    return EnsureAdv() ? realAdv_->GetUserProvidedClock(pfUserClock) : E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::DeliverTime(QWORD cnsTime) {
    if (!EnsureAdv()) return E_NOINTERFACE;
    HRESULT hr = realAdv_->DeliverTime(cnsTime);
    AsyncLogger::Instance().LogJsonEvent("DeliverTime", readerId_, "async", "\"time_100ns\":%llu,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned long long>(cnsTime), static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetManualStreamSelection(BOOL fSelection) { return EnsureAdv() ? realAdv_->SetManualStreamSelection(fSelection) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetManualStreamSelection(BOOL* pfSelection) { return EnsureAdv() ? realAdv_->GetManualStreamSelection(pfSelection) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetStreamsSelected(WORD cStreamCount, WORD* pwStreamNumbers, WMT_STREAM_SELECTION* pSelections) { return EnsureAdv() ? realAdv_->SetStreamsSelected(cStreamCount, pwStreamNumbers, pSelections) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetStreamSelected(WORD wStreamNum, WMT_STREAM_SELECTION* pSelection) { return EnsureAdv() ? realAdv_->GetStreamSelected(wStreamNum, pSelection) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetReceiveSelectionCallbacks(BOOL fGetCallbacks) { return EnsureAdv() ? realAdv_->SetReceiveSelectionCallbacks(fGetCallbacks) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetReceiveSelectionCallbacks(BOOL* pfGetCallbacks) { return EnsureAdv() ? realAdv_->GetReceiveSelectionCallbacks(pfGetCallbacks) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetReceiveStreamSamples(WORD wStreamNum, BOOL fReceiveStreamSamples) {
    if (!EnsureAdv()) return E_NOINTERFACE;
    HRESULT hr = realAdv_->SetReceiveStreamSamples(wStreamNum, fReceiveStreamSamples);
    AsyncLogger::Instance().LogJsonEvent("SetReceiveStreamSamples", readerId_, "async", "\"stream\":%u,\"compressed\":%s,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned>(wStreamNum), fReceiveStreamSamples ? "true" : "false", static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetReceiveStreamSamples(WORD wStreamNum, BOOL* pfReceiveStreamSamples) { return EnsureAdv() ? realAdv_->GetReceiveStreamSamples(wStreamNum, pfReceiveStreamSamples) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetAllocateForOutput(DWORD dwOutputNum, BOOL fAllocate) { return EnsureAdv() ? realAdv_->SetAllocateForOutput(dwOutputNum, fAllocate) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAllocateForOutput(DWORD dwOutputNum, BOOL* pfAllocate) { return EnsureAdv() ? realAdv_->GetAllocateForOutput(dwOutputNum, pfAllocate) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetAllocateForStream(WORD wStreamNum, BOOL fAllocate) { return EnsureAdv() ? realAdv_->SetAllocateForStream(wStreamNum, fAllocate) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAllocateForStream(WORD wStreamNum, BOOL* pfAllocate) { return EnsureAdv() ? realAdv_->GetAllocateForStream(wStreamNum, pfAllocate) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetStatistics(WM_READER_STATISTICS* pStats) { return EnsureAdv() ? realAdv_->GetStatistics(pStats) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetClientInfo(WM_READER_CLIENTINFO* pClientInfo) { return EnsureAdv() ? realAdv_->SetClientInfo(pClientInfo) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetMaxOutputSampleSize(DWORD dwOutput, DWORD* pcbMax) { return EnsureAdv() ? realAdv_->GetMaxOutputSampleSize(dwOutput, pcbMax) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetMaxStreamSampleSize(WORD wStream, DWORD* pcbMax) { return EnsureAdv() ? realAdv_->GetMaxStreamSampleSize(wStream, pcbMax) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::NotifyLateDelivery(QWORD cnsLateness) { return EnsureAdv() ? realAdv_->NotifyLateDelivery(cnsLateness) : E_NOINTERFACE; }

HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetPlayMode(WMT_PLAY_MODE Mode) { return (EnsureAdv() && realAdv2_) ? realAdv2_->SetPlayMode(Mode) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetPlayMode(WMT_PLAY_MODE* pMode) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetPlayMode(pMode) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetBufferProgress(DWORD* pdwPercent, QWORD* pcnsBuffering) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetBufferProgress(pdwPercent, pcnsBuffering) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetDownloadProgress(DWORD* pdwPercent, QWORD* pqwBytesDownloaded, QWORD* pcnsDownload) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetDownloadProgress(pdwPercent, pqwBytesDownloaded, pcnsDownload) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetSaveAsProgress(DWORD* pdwPercent) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetSaveAsProgress(pdwPercent) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SaveFileAs(const WCHAR* pwszFilename) { return (EnsureAdv() && realAdv2_) ? realAdv2_->SaveFileAs(pwszFilename) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetProtocolName(WCHAR* pwszProtocol, DWORD* pcchProtocol) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetProtocolName(pwszProtocol, pcchProtocol) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::StartAtMarker(WORD wMarkerIndex, QWORD cnsDuration, float fRate, void* pvContext) { return (EnsureAdv() && realAdv2_) ? realAdv2_->StartAtMarker(wMarkerIndex, cnsDuration, fRate, pvContext) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetOutputSetting(dwOutputNum, pszName, pType, pValue, pcbLength) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) { return (EnsureAdv() && realAdv2_) ? realAdv2_->SetOutputSetting(dwOutputNum, pszName, Type, pValue, cbLength) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::Preroll(QWORD cnsStart, QWORD cnsDuration, float fRate) { return (EnsureAdv() && realAdv2_) ? realAdv2_->Preroll(cnsStart, cnsDuration, fRate) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetLogClientID(BOOL fLogClientID) { return (EnsureAdv() && realAdv2_) ? realAdv2_->SetLogClientID(fLogClientID) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetLogClientID(BOOL* pfLogClientID) { return (EnsureAdv() && realAdv2_) ? realAdv2_->GetLogClientID(pfLogClientID) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::StopBuffering() { return (EnsureAdv() && realAdv2_) ? realAdv2_->StopBuffering() : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::OpenStream(IStream* pStream, IWMReaderCallback* pCallback, void* pvContext) {
    if (!EnsureAdv() || !realAdv2_) return E_NOINTERFACE;
    auto* wrappedCb = pCallback ? new WrappedReaderCallback(readerId_, pCallback) : nullptr;
    HRESULT hr = realAdv2_->OpenStream(pStream, wrappedCb, pvContext);
    if (wrappedCb) wrappedCb->Release();
    AsyncLogger::Instance().LogJsonEvent("OpenStream", readerId_, "async", "\"stream_ptr\":\"0x%p\",\"hr\":\"0x%08lX\"",
                                         pStream, static_cast<unsigned long>(hr));
    return hr;
}

// IWMHeaderInfo / 2 / 3 on AsyncReader
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAttributeCount(WORD wStreamNum, WORD* pcAttributes) { return EnsureHeaderInfo() ? realHi_->GetAttributeCount(wStreamNum, pcAttributes) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAttributeByIndex(WORD wIndex, WORD* pwStreamNum, WCHAR* pwszName, WORD* pcchNameLen,
                                                                  WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    HRESULT hr = realHi_->GetAttributeByIndex(wIndex, pwStreamNum, pwszName, pcchNameLen, pType, pValue, pcbLength);
    InspectAttributeValue(readerId_, "async", "GetAttributeByIndex", pwStreamNum ? *pwStreamNum : 0, pwszName,
                          pType ? *pType : WMT_TYPE_BINARY, pValue, pcbLength ? *pcbLength : 0, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAttributeByName(WORD* pwStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType,
                                                                 BYTE* pValue, WORD* pcbLength) {
    if (!EnsureHeaderInfo()) return E_NOINTERFACE;
    HRESULT hr = realHi_->GetAttributeByName(pwStreamNum, pszName, pType, pValue, pcbLength);
    InspectAttributeValue(readerId_, "async", "GetAttributeByName", pwStreamNum ? *pwStreamNum : 0, pszName,
                          pType ? *pType : WMT_TYPE_BINARY, pValue, pcbLength ? *pcbLength : 0, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetAttribute(WORD wStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) { return EnsureHeaderInfo() ? realHi_->SetAttribute(wStreamNum, pszName, Type, pValue, cbLength) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetMarkerCount(WORD* pcMarkers) { return EnsureHeaderInfo() ? realHi_->GetMarkerCount(pcMarkers) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetMarker(WORD wIndex, WCHAR* pwszMarkerName, WORD* pcchMarkerNameLen, QWORD* pcnsMarkerTime) { return EnsureHeaderInfo() ? realHi_->GetMarker(wIndex, pwszMarkerName, pcchMarkerNameLen, pcnsMarkerTime) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::AddMarker(LPWSTR pwszMarkerName, QWORD cnsMarkerTime) { return EnsureHeaderInfo() ? realHi_->AddMarker(pwszMarkerName, cnsMarkerTime) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::RemoveMarker(WORD wIndex) { return EnsureHeaderInfo() ? realHi_->RemoveMarker(wIndex) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetScriptCount(WORD* pcScripts) { return EnsureHeaderInfo() ? realHi_->GetScriptCount(pcScripts) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetScript(WORD wIndex, WCHAR* pwszType, WORD* pcchTypeLen, WCHAR* pwszCommand, WORD* pcchCommandLen, QWORD* pcnsScriptTime) { return EnsureHeaderInfo() ? realHi_->GetScript(wIndex, pwszType, pcchTypeLen, pwszCommand, pcchCommandLen, pcnsScriptTime) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::AddScript(LPWSTR pwszType, LPWSTR pwszCommand, QWORD cnsScriptTime) { return EnsureHeaderInfo() ? realHi_->AddScript(pwszType, pwszCommand, cnsScriptTime) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::RemoveScript(WORD wIndex) { return EnsureHeaderInfo() ? realHi_->RemoveScript(wIndex) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetCodecInfoCount(DWORD* pcCodecInfos) { return (EnsureHeaderInfo() && realHi2_) ? realHi2_->GetCodecInfoCount(pcCodecInfos) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetCodecInfo(DWORD wIndex, WORD* pcchName, WCHAR* pwszName, WORD* pcchDescription, WCHAR* pwszDescription, WMT_CODEC_INFO_TYPE* pCodecType, WORD* pcbCodecInfo, BYTE* pbCodecInfo) { return (EnsureHeaderInfo() && realHi2_) ? realHi2_->GetCodecInfo(wIndex, pcchName, pwszName, pcchDescription, pwszDescription, pCodecType, pcbCodecInfo, pbCodecInfo) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAttributeCountEx(WORD wStreamNum, WORD* pcAttributes) { return (EnsureHeaderInfo() && realHi3_) ? realHi3_->GetAttributeCountEx(wStreamNum, pcAttributes) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAttributeIndices(WORD wStreamNum, LPCWSTR pwszName, WORD* pwLangIndex, WORD* pwIndices, WORD* pwCount) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    HRESULT hr = realHi3_->GetAttributeIndices(wStreamNum, pwszName, pwLangIndex, pwIndices, pwCount);
    std::string s = AsyncLogger::WideToUtf8Escaped(pwszName);
    AsyncLogger::Instance().LogJsonEvent("GetAttributeIndices", readerId_, "async",
                                         "\"stream\":%u,\"attr\":\"%s\",\"count\":%u,\"hr\":\"0x%08lX\"",
                                         static_cast<unsigned>(wStreamNum), s.c_str(),
                                         static_cast<unsigned>(pwCount ? *pwCount : 0),
                                         static_cast<unsigned long>(hr));
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetAttributeByIndexEx(WORD wStreamNum, WORD wIndex, LPWSTR pwszName, WORD* pwNameLen,
                                                                    WMT_ATTR_DATATYPE* pType, WORD* pwLangIndex, BYTE* pValue, DWORD* pdwDataLength) {
    if (!EnsureHeaderInfo() || !realHi3_) return E_NOINTERFACE;
    HRESULT hr = realHi3_->GetAttributeByIndexEx(wStreamNum, wIndex, pwszName, pwNameLen, pType, pwLangIndex, pValue, pdwDataLength);
    InspectAttributeValue(readerId_, "async", "GetAttributeByIndexEx", wStreamNum, pwszName,
                          pType ? *pType : WMT_TYPE_BINARY, pValue, pdwDataLength ? *pdwDataLength : 0, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::ModifyAttribute(WORD wStreamNum, WORD wIndex, WMT_ATTR_DATATYPE Type, WORD wLangIndex, const BYTE* pValue, DWORD dwLength) { return (EnsureHeaderInfo() && realHi3_) ? realHi3_->ModifyAttribute(wStreamNum, wIndex, Type, wLangIndex, pValue, dwLength) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::AddAttribute(WORD wStreamNum, LPCWSTR pszName, WORD* pwIndex, WMT_ATTR_DATATYPE Type, WORD wLangIndex, const BYTE* pValue, DWORD dwLength) { return (EnsureHeaderInfo() && realHi3_) ? realHi3_->AddAttribute(wStreamNum, pszName, pwIndex, Type, wLangIndex, pValue, dwLength) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::DeleteAttribute(WORD wStreamNum, WORD wIndex) { return (EnsureHeaderInfo() && realHi3_) ? realHi3_->DeleteAttribute(wStreamNum, wIndex) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::AddCodecInfo(LPWSTR pwszName, LPWSTR pwszDescription, WMT_CODEC_INFO_TYPE codecType, WORD cbCodecInfo, BYTE* pbCodecInfo) { return (EnsureHeaderInfo() && realHi3_) ? realHi3_->AddCodecInfo(pwszName, pwszDescription, codecType, cbCodecInfo, pbCodecInfo) : E_NOINTERFACE; }

// IWMProfile on AsyncReader
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetVersion(WMT_VERSION* pdwVersion) { return EnsureProfile() ? realProfile_->GetVersion(pdwVersion) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetName(WCHAR* pwszName, DWORD* pcchName) { return EnsureProfile() ? realProfile_->GetName(pwszName, pcchName) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetName(const WCHAR* pwszName) { return EnsureProfile() ? realProfile_->SetName(pwszName) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetDescription(WCHAR* pwszDescription, DWORD* pcchDescription) { return EnsureProfile() ? realProfile_->GetDescription(pwszDescription, pcchDescription) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::SetDescription(const WCHAR* pwszDescription) { return EnsureProfile() ? realProfile_->SetDescription(pwszDescription) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetStreamCount(DWORD* pcStreams) { return EnsureProfile() ? realProfile_->GetStreamCount(pcStreams) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetStream(DWORD dwStreamIndex, IWMStreamConfig** ppConfig) { return EnsureProfile() ? realProfile_->GetStream(dwStreamIndex, ppConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetStreamByNumber(WORD wStreamNum, IWMStreamConfig** ppConfig) { return EnsureProfile() ? realProfile_->GetStreamByNumber(wStreamNum, ppConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::RemoveStream(IWMStreamConfig* pConfig) { return EnsureProfile() ? realProfile_->RemoveStream(pConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::RemoveStreamByNumber(WORD wStreamNum) { return EnsureProfile() ? realProfile_->RemoveStreamByNumber(wStreamNum) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::AddStream(IWMStreamConfig* pConfig) { return EnsureProfile() ? realProfile_->AddStream(pConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::ReconfigStream(IWMStreamConfig* pConfig) { return EnsureProfile() ? realProfile_->ReconfigStream(pConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::CreateNewStream(REFGUID guidStreamType, IWMStreamConfig** ppConfig) { return EnsureProfile() ? realProfile_->CreateNewStream(guidStreamType, ppConfig) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetMutualExclusionCount(DWORD* pcME) { return EnsureProfile() ? realProfile_->GetMutualExclusionCount(pcME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::GetMutualExclusion(DWORD dwMEIndex, IWMMutualExclusion** ppME) { return EnsureProfile() ? realProfile_->GetMutualExclusion(dwMEIndex, ppME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::RemoveMutualExclusion(IWMMutualExclusion* pME) { return EnsureProfile() ? realProfile_->RemoveMutualExclusion(pME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::AddMutualExclusion(IWMMutualExclusion* pME) { return EnsureProfile() ? realProfile_->AddMutualExclusion(pME) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedAsyncReader::CreateNewMutualExclusion(IWMMutualExclusion** ppME) { return EnsureProfile() ? realProfile_->CreateNewMutualExclusion(ppME) : E_NOINTERFACE; }

} // namespace probe
