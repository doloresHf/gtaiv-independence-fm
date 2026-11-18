#pragma once

#include <windows.h>
#include <wmsdk.h>
#include <cstdint>

namespace probe {

int32_t NextReaderId();
void InspectOutputMediaProps(int32_t readerId, const char* kind, const char* stage, DWORD dwOutputNum, IWMOutputMediaProps* pProps);
void InspectAttributeValue(int32_t readerId, const char* kind, const char* api, WORD streamNum, const wchar_t* attrName,
                           WMT_ATTR_DATATYPE type, const BYTE* pValue, DWORD cbLength, HRESULT hr);

class WrappedSyncReader final : public IWMSyncReader2, public IWMHeaderInfo3, public IWMProfile {
public:
    explicit WrappedSyncReader(IWMSyncReader* realReader);
    ~WrappedSyncReader();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IWMSyncReader
    HRESULT STDMETHODCALLTYPE Open(LPCWSTR pwszFilename) override;
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE SetRange(QWORD cnsStartTime, LONGLONG cnsDuration) override;
    HRESULT STDMETHODCALLTYPE SetRangeByFrame(WORD wStreamNum, QWORD qwFrameNumber, LONGLONG cFramesToRead) override;
    HRESULT STDMETHODCALLTYPE GetNextSample(WORD wStreamNum, INSSBuffer** ppSample, QWORD* pcnsSampleTime,
                                            QWORD* pcnsDuration, DWORD* pdwFlags, DWORD* pdwOutputNum, WORD* pwStreamNum) override;
    HRESULT STDMETHODCALLTYPE SetStreamsSelected(WORD cStreamCount, WORD* pwStreamNumbers, WMT_STREAM_SELECTION* pSelections) override;
    HRESULT STDMETHODCALLTYPE GetStreamSelected(WORD wStreamNum, WMT_STREAM_SELECTION* pSelection) override;
    HRESULT STDMETHODCALLTYPE SetReadStreamSamples(WORD wStreamNum, BOOL fCompressed) override;
    HRESULT STDMETHODCALLTYPE GetReadStreamSamples(WORD wStreamNum, BOOL* pfCompressed) override;
    HRESULT STDMETHODCALLTYPE GetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) override;
    HRESULT STDMETHODCALLTYPE SetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) override;
    HRESULT STDMETHODCALLTYPE GetOutputCount(DWORD* pcOutputs) override;
    HRESULT STDMETHODCALLTYPE GetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps** ppOutput) override;
    HRESULT STDMETHODCALLTYPE SetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps* pOutput) override;
    HRESULT STDMETHODCALLTYPE GetOutputFormatCount(DWORD dwOutputNum, DWORD* pcFormats) override;
    HRESULT STDMETHODCALLTYPE GetOutputFormat(DWORD dwOutputNum, DWORD dwFormatNum, IWMOutputMediaProps** ppProps) override;
    HRESULT STDMETHODCALLTYPE GetOutputNumberForStream(WORD wStreamNum, DWORD* pdwOutputNum) override;
    HRESULT STDMETHODCALLTYPE GetStreamNumberForOutput(DWORD dwOutputNum, WORD* pwStreamNum) override;
    HRESULT STDMETHODCALLTYPE GetMaxOutputSampleSize(DWORD dwOutput, DWORD* pcbMax) override;
    HRESULT STDMETHODCALLTYPE GetMaxStreamSampleSize(WORD wStream, DWORD* pcbMax) override;
    HRESULT STDMETHODCALLTYPE OpenStream(IStream* pStream) override;

    // IWMSyncReader2
    HRESULT STDMETHODCALLTYPE SetRangeByTimecode(WORD wStreamNum, WMT_TIMECODE_EXTENSION_DATA* pStart, WMT_TIMECODE_EXTENSION_DATA* pEnd) override;
    HRESULT STDMETHODCALLTYPE SetRangeByFrameEx(WORD wStreamNum, QWORD qwFrameNumber, LONGLONG cFramesToRead, QWORD* pcnsStartTime) override;
    HRESULT STDMETHODCALLTYPE SetAllocateForOutput(DWORD dwOutputNum, IWMReaderAllocatorEx* pAllocator) override;
    HRESULT STDMETHODCALLTYPE GetAllocateForOutput(DWORD dwOutputNum, IWMReaderAllocatorEx** ppAllocator) override;
    HRESULT STDMETHODCALLTYPE SetAllocateForStream(WORD wStreamNum, IWMReaderAllocatorEx* pAllocator) override;
    HRESULT STDMETHODCALLTYPE GetAllocateForStream(WORD wStreamNum, IWMReaderAllocatorEx** ppAllocator) override;

    // IWMHeaderInfo
    HRESULT STDMETHODCALLTYPE GetAttributeCount(WORD wStreamNum, WORD* pcAttributes) override;
    HRESULT STDMETHODCALLTYPE GetAttributeByIndex(WORD wIndex, WORD* pwStreamNum, WCHAR* pwszName, WORD* pcchNameLen,
                                                  WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) override;
    HRESULT STDMETHODCALLTYPE GetAttributeByName(WORD* pwStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType,
                                                 BYTE* pValue, WORD* pcbLength) override;
    HRESULT STDMETHODCALLTYPE SetAttribute(WORD wStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) override;
    HRESULT STDMETHODCALLTYPE GetMarkerCount(WORD* pcMarkers) override;
    HRESULT STDMETHODCALLTYPE GetMarker(WORD wIndex, WCHAR* pwszMarkerName, WORD* pcchMarkerNameLen, QWORD* pcnsMarkerTime) override;
    HRESULT STDMETHODCALLTYPE AddMarker(LPWSTR pwszMarkerName, QWORD cnsMarkerTime) override;
    HRESULT STDMETHODCALLTYPE RemoveMarker(WORD wIndex) override;
    HRESULT STDMETHODCALLTYPE GetScriptCount(WORD* pcScripts) override;
    HRESULT STDMETHODCALLTYPE GetScript(WORD wIndex, WCHAR* pwszType, WORD* pcchTypeLen, WCHAR* pwszCommand,
                                        WORD* pcchCommandLen, QWORD* pcnsScriptTime) override;
    HRESULT STDMETHODCALLTYPE AddScript(LPWSTR pwszType, LPWSTR pwszCommand, QWORD cnsScriptTime) override;
    HRESULT STDMETHODCALLTYPE RemoveScript(WORD wIndex) override;

    // IWMHeaderInfo2
    HRESULT STDMETHODCALLTYPE GetCodecInfoCount(DWORD* pcCodecInfos) override;
    HRESULT STDMETHODCALLTYPE GetCodecInfo(DWORD wIndex, WORD* pcchName, WCHAR* pwszName, WORD* pcchDescription,
                                           WCHAR* pwszDescription, WMT_CODEC_INFO_TYPE* pCodecType, WORD* pcbCodecInfo, BYTE* pbCodecInfo) override;

    // IWMHeaderInfo3
    HRESULT STDMETHODCALLTYPE GetAttributeCountEx(WORD wStreamNum, WORD* pcAttributes) override;
    HRESULT STDMETHODCALLTYPE GetAttributeIndices(WORD wStreamNum, LPCWSTR pwszName, WORD* pwLangIndex, WORD* pwIndices, WORD* pwCount) override;
    HRESULT STDMETHODCALLTYPE GetAttributeByIndexEx(WORD wStreamNum, WORD wIndex, LPWSTR pwszName, WORD* pwNameLen,
                                                    WMT_ATTR_DATATYPE* pType, WORD* pwLangIndex, BYTE* pValue, DWORD* pdwDataLength) override;
    HRESULT STDMETHODCALLTYPE ModifyAttribute(WORD wStreamNum, WORD wIndex, WMT_ATTR_DATATYPE Type, WORD wLangIndex,
                                              const BYTE* pValue, DWORD dwLength) override;
    HRESULT STDMETHODCALLTYPE AddAttribute(WORD wStreamNum, LPCWSTR pszName, WORD* pwIndex, WMT_ATTR_DATATYPE Type,
                                           WORD wLangIndex, const BYTE* pValue, DWORD dwLength) override;
    HRESULT STDMETHODCALLTYPE DeleteAttribute(WORD wStreamNum, WORD wIndex) override;
    HRESULT STDMETHODCALLTYPE AddCodecInfo(LPWSTR pwszName, LPWSTR pwszDescription, WMT_CODEC_INFO_TYPE codecType,
                                           WORD cbCodecInfo, BYTE* pbCodecInfo) override;

    // IWMProfile
    HRESULT STDMETHODCALLTYPE GetVersion(WMT_VERSION* pdwVersion) override;
    HRESULT STDMETHODCALLTYPE GetName(WCHAR* pwszName, DWORD* pcchName) override;
    HRESULT STDMETHODCALLTYPE SetName(const WCHAR* pwszName) override;
    HRESULT STDMETHODCALLTYPE GetDescription(WCHAR* pwszDescription, DWORD* pcchDescription) override;
    HRESULT STDMETHODCALLTYPE SetDescription(const WCHAR* pwszDescription) override;
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcStreams) override;
    HRESULT STDMETHODCALLTYPE GetStream(DWORD dwStreamIndex, IWMStreamConfig** ppConfig) override;
    HRESULT STDMETHODCALLTYPE GetStreamByNumber(WORD wStreamNum, IWMStreamConfig** ppConfig) override;
    HRESULT STDMETHODCALLTYPE RemoveStream(IWMStreamConfig* pConfig) override;
    HRESULT STDMETHODCALLTYPE RemoveStreamByNumber(WORD wStreamNum) override;
    HRESULT STDMETHODCALLTYPE AddStream(IWMStreamConfig* pConfig) override;
    HRESULT STDMETHODCALLTYPE ReconfigStream(IWMStreamConfig* pConfig) override;
    HRESULT STDMETHODCALLTYPE CreateNewStream(REFGUID guidStreamType, IWMStreamConfig** ppConfig) override;
    HRESULT STDMETHODCALLTYPE GetMutualExclusionCount(DWORD* pcME) override;
    HRESULT STDMETHODCALLTYPE GetMutualExclusion(DWORD dwMEIndex, IWMMutualExclusion** ppME) override;
    HRESULT STDMETHODCALLTYPE RemoveMutualExclusion(IWMMutualExclusion* pME) override;
    HRESULT STDMETHODCALLTYPE AddMutualExclusion(IWMMutualExclusion* pME) override;
    HRESULT STDMETHODCALLTYPE CreateNewMutualExclusion(IWMMutualExclusion** ppME) override;

private:
    bool EnsureSync2();
    bool EnsureHeaderInfo();
    bool EnsureProfile();
    bool VerifyPcmFormat();

    volatile LONG refCount_ = 1;
    int32_t readerId_ = 0;
    IWMSyncReader* real_ = nullptr;
    IWMSyncReader2* real2_ = nullptr;
    IWMHeaderInfo* realHi_ = nullptr;
    IWMHeaderInfo2* realHi2_ = nullptr;
    IWMHeaderInfo3* realHi3_ = nullptr;
    IWMProfile* realProfile_ = nullptr;

    int64_t lastSampleWallUs_ = 0;
    uint64_t sampleSeq_ = 0;
    uint64_t totalBytes_ = 0;

    // Stage B per-instance state (isolated per reader, no shared global cursor)
    DWORD openTid_ = 0;
    bool openSucceeded_ = false;
    bool isTargetTrack_ = false;
    bool pcmFormatChecked_ = false;
    bool pcmFormatValid_ = false;
    bool seekPending_ = false;
    QWORD requestedSeek100ns_ = 0;
    uint64_t frameCursor_ = 0;
    bool decisionLogged_ = false;
    bool ipcRegistered_ = false;
    uint64_t replacedSamples_ = 0;
    uint64_t replacedBytes_ = 0;
    uint64_t passthroughSamples_ = 0;
};

class WrappedReaderCallback final : public IWMReaderCallback, public IWMReaderCallbackAdvanced {
public:
    WrappedReaderCallback(int32_t readerId, IWMReaderCallback* realCb);
    ~WrappedReaderCallback();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IWMStatusCallback
    HRESULT STDMETHODCALLTYPE OnStatus(WMT_STATUS Status, HRESULT hr, WMT_ATTR_DATATYPE dwType,
                                       BYTE* pValue, void* pvContext) override;
    // IWMReaderCallback
    HRESULT STDMETHODCALLTYPE OnSample(DWORD dwOutputNum, QWORD cnsSampleTime, QWORD cnsSampleDuration,
                                       DWORD dwFlags, INSSBuffer* pSample, void* pvContext) override;

    // IWMReaderCallbackAdvanced
    HRESULT STDMETHODCALLTYPE OnStreamSample(WORD wStreamNum, QWORD cnsSampleTime, QWORD cnsSampleDuration,
                                             DWORD dwFlags, INSSBuffer* pSample, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE OnTime(QWORD cnsCurrentTime, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE OnStreamSelection(WORD wStreamCount, WORD* pStreamNumbers,
                                                WMT_STREAM_SELECTION* pSelections, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE OnOutputPropsChanged(DWORD dwOutputNum, WM_MEDIA_TYPE* pMediaType, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE AllocateForStream(WORD wStreamNum, DWORD cbBuffer, INSSBuffer** ppBuffer, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE AllocateForOutput(DWORD dwOutputNum, DWORD cbBuffer, INSSBuffer** ppBuffer, void* pvContext) override;

private:
    volatile LONG refCount_ = 1;
    int32_t readerId_ = 0;
    IWMReaderCallback* realCb_ = nullptr;
    IWMReaderCallbackAdvanced* realCbAdv_ = nullptr;
    int64_t lastSampleWallUs_ = 0;
    uint64_t sampleSeq_ = 0;
};

class WrappedAsyncReader final : public IWMReader, public IWMReaderAdvanced2, public IWMHeaderInfo3, public IWMProfile {
public:
    explicit WrappedAsyncReader(IWMReader* realReader);
    ~WrappedAsyncReader();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IWMReader
    HRESULT STDMETHODCALLTYPE Open(LPCWSTR pwszURL, IWMReaderCallback* pCallback, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE GetOutputCount(DWORD* pcOutputs) override;
    HRESULT STDMETHODCALLTYPE GetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps** ppOutput) override;
    HRESULT STDMETHODCALLTYPE SetOutputProps(DWORD dwOutputNum, IWMOutputMediaProps* pOutput) override;
    HRESULT STDMETHODCALLTYPE GetOutputFormatCount(DWORD dwOutputNum, DWORD* pcFormats) override;
    HRESULT STDMETHODCALLTYPE GetOutputFormat(DWORD dwOutputNum, DWORD dwFormatNum, IWMOutputMediaProps** ppProps) override;
    HRESULT STDMETHODCALLTYPE Start(QWORD cnsStart, QWORD cnsDuration, float fRate, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE Pause() override;
    HRESULT STDMETHODCALLTYPE Resume() override;

    // IWMReaderAdvanced
    HRESULT STDMETHODCALLTYPE SetUserProvidedClock(BOOL fUserClock) override;
    HRESULT STDMETHODCALLTYPE GetUserProvidedClock(BOOL* pfUserClock) override;
    HRESULT STDMETHODCALLTYPE DeliverTime(QWORD cnsTime) override;
    HRESULT STDMETHODCALLTYPE SetManualStreamSelection(BOOL fSelection) override;
    HRESULT STDMETHODCALLTYPE GetManualStreamSelection(BOOL* pfSelection) override;
    HRESULT STDMETHODCALLTYPE SetStreamsSelected(WORD cStreamCount, WORD* pwStreamNumbers, WMT_STREAM_SELECTION* pSelections) override;
    HRESULT STDMETHODCALLTYPE GetStreamSelected(WORD wStreamNum, WMT_STREAM_SELECTION* pSelection) override;
    HRESULT STDMETHODCALLTYPE SetReceiveSelectionCallbacks(BOOL fGetCallbacks) override;
    HRESULT STDMETHODCALLTYPE GetReceiveSelectionCallbacks(BOOL* pfGetCallbacks) override;
    HRESULT STDMETHODCALLTYPE SetReceiveStreamSamples(WORD wStreamNum, BOOL fReceiveStreamSamples) override;
    HRESULT STDMETHODCALLTYPE GetReceiveStreamSamples(WORD wStreamNum, BOOL* pfReceiveStreamSamples) override;
    HRESULT STDMETHODCALLTYPE SetAllocateForOutput(DWORD dwOutputNum, BOOL fAllocate) override;
    HRESULT STDMETHODCALLTYPE GetAllocateForOutput(DWORD dwOutputNum, BOOL* pfAllocate) override;
    HRESULT STDMETHODCALLTYPE SetAllocateForStream(WORD wStreamNum, BOOL fAllocate) override;
    HRESULT STDMETHODCALLTYPE GetAllocateForStream(WORD wStreamNum, BOOL* pfAllocate) override;
    HRESULT STDMETHODCALLTYPE GetStatistics(WM_READER_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE SetClientInfo(WM_READER_CLIENTINFO* pClientInfo) override;
    HRESULT STDMETHODCALLTYPE GetMaxOutputSampleSize(DWORD dwOutput, DWORD* pcbMax) override;
    HRESULT STDMETHODCALLTYPE GetMaxStreamSampleSize(WORD wStream, DWORD* pcbMax) override;
    HRESULT STDMETHODCALLTYPE NotifyLateDelivery(QWORD cnsLateness) override;

    // IWMReaderAdvanced2
    HRESULT STDMETHODCALLTYPE SetPlayMode(WMT_PLAY_MODE Mode) override;
    HRESULT STDMETHODCALLTYPE GetPlayMode(WMT_PLAY_MODE* pMode) override;
    HRESULT STDMETHODCALLTYPE GetBufferProgress(DWORD* pdwPercent, QWORD* pcnsBuffering) override;
    HRESULT STDMETHODCALLTYPE GetDownloadProgress(DWORD* pdwPercent, QWORD* pqwBytesDownloaded, QWORD* pcnsDownload) override;
    HRESULT STDMETHODCALLTYPE GetSaveAsProgress(DWORD* pdwPercent) override;
    HRESULT STDMETHODCALLTYPE SaveFileAs(const WCHAR* pwszFilename) override;
    HRESULT STDMETHODCALLTYPE GetProtocolName(WCHAR* pwszProtocol, DWORD* pcchProtocol) override;
    HRESULT STDMETHODCALLTYPE StartAtMarker(WORD wMarkerIndex, QWORD cnsDuration, float fRate, void* pvContext) override;
    HRESULT STDMETHODCALLTYPE GetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) override;
    HRESULT STDMETHODCALLTYPE SetOutputSetting(DWORD dwOutputNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) override;
    HRESULT STDMETHODCALLTYPE Preroll(QWORD cnsStart, QWORD cnsDuration, float fRate) override;
    HRESULT STDMETHODCALLTYPE SetLogClientID(BOOL fLogClientID) override;
    HRESULT STDMETHODCALLTYPE GetLogClientID(BOOL* pfLogClientID) override;
    HRESULT STDMETHODCALLTYPE StopBuffering() override;
    HRESULT STDMETHODCALLTYPE OpenStream(IStream* pStream, IWMReaderCallback* pCallback, void* pvContext) override;

    // IWMHeaderInfo / 2 / 3
    HRESULT STDMETHODCALLTYPE GetAttributeCount(WORD wStreamNum, WORD* pcAttributes) override;
    HRESULT STDMETHODCALLTYPE GetAttributeByIndex(WORD wIndex, WORD* pwStreamNum, WCHAR* pwszName, WORD* pcchNameLen,
                                                  WMT_ATTR_DATATYPE* pType, BYTE* pValue, WORD* pcbLength) override;
    HRESULT STDMETHODCALLTYPE GetAttributeByName(WORD* pwStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE* pType,
                                                 BYTE* pValue, WORD* pcbLength) override;
    HRESULT STDMETHODCALLTYPE SetAttribute(WORD wStreamNum, LPCWSTR pszName, WMT_ATTR_DATATYPE Type, const BYTE* pValue, WORD cbLength) override;
    HRESULT STDMETHODCALLTYPE GetMarkerCount(WORD* pcMarkers) override;
    HRESULT STDMETHODCALLTYPE GetMarker(WORD wIndex, WCHAR* pwszMarkerName, WORD* pcchMarkerNameLen, QWORD* pcnsMarkerTime) override;
    HRESULT STDMETHODCALLTYPE AddMarker(LPWSTR pwszMarkerName, QWORD cnsMarkerTime) override;
    HRESULT STDMETHODCALLTYPE RemoveMarker(WORD wIndex) override;
    HRESULT STDMETHODCALLTYPE GetScriptCount(WORD* pcScripts) override;
    HRESULT STDMETHODCALLTYPE GetScript(WORD wIndex, WCHAR* pwszType, WORD* pcchTypeLen, WCHAR* pwszCommand,
                                        WORD* pcchCommandLen, QWORD* pcnsScriptTime) override;
    HRESULT STDMETHODCALLTYPE AddScript(LPWSTR pwszType, LPWSTR pwszCommand, QWORD cnsScriptTime) override;
    HRESULT STDMETHODCALLTYPE RemoveScript(WORD wIndex) override;
    HRESULT STDMETHODCALLTYPE GetCodecInfoCount(DWORD* pcCodecInfos) override;
    HRESULT STDMETHODCALLTYPE GetCodecInfo(DWORD wIndex, WORD* pcchName, WCHAR* pwszName, WORD* pcchDescription,
                                           WCHAR* pwszDescription, WMT_CODEC_INFO_TYPE* pCodecType, WORD* pcbCodecInfo, BYTE* pbCodecInfo) override;
    HRESULT STDMETHODCALLTYPE GetAttributeCountEx(WORD wStreamNum, WORD* pcAttributes) override;
    HRESULT STDMETHODCALLTYPE GetAttributeIndices(WORD wStreamNum, LPCWSTR pwszName, WORD* pwLangIndex, WORD* pwIndices, WORD* pwCount) override;
    HRESULT STDMETHODCALLTYPE GetAttributeByIndexEx(WORD wStreamNum, WORD wIndex, LPWSTR pwszName, WORD* pwNameLen,
                                                    WMT_ATTR_DATATYPE* pType, WORD* pwLangIndex, BYTE* pValue, DWORD* pdwDataLength) override;
    HRESULT STDMETHODCALLTYPE ModifyAttribute(WORD wStreamNum, WORD wIndex, WMT_ATTR_DATATYPE Type, WORD wLangIndex,
                                              const BYTE* pValue, DWORD dwLength) override;
    HRESULT STDMETHODCALLTYPE AddAttribute(WORD wStreamNum, LPCWSTR pszName, WORD* pwIndex, WMT_ATTR_DATATYPE Type,
                                           WORD wLangIndex, const BYTE* pValue, DWORD dwLength) override;
    HRESULT STDMETHODCALLTYPE DeleteAttribute(WORD wStreamNum, WORD wIndex) override;
    HRESULT STDMETHODCALLTYPE AddCodecInfo(LPWSTR pwszName, LPWSTR pwszDescription, WMT_CODEC_INFO_TYPE codecType,
                                           WORD cbCodecInfo, BYTE* pbCodecInfo) override;

    // IWMProfile
    HRESULT STDMETHODCALLTYPE GetVersion(WMT_VERSION* pdwVersion) override;
    HRESULT STDMETHODCALLTYPE GetName(WCHAR* pwszName, DWORD* pcchName) override;
    HRESULT STDMETHODCALLTYPE SetName(const WCHAR* pwszName) override;
    HRESULT STDMETHODCALLTYPE GetDescription(WCHAR* pwszDescription, DWORD* pcchDescription) override;
    HRESULT STDMETHODCALLTYPE SetDescription(const WCHAR* pwszDescription) override;
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD* pcStreams) override;
    HRESULT STDMETHODCALLTYPE GetStream(DWORD dwStreamIndex, IWMStreamConfig** ppConfig) override;
    HRESULT STDMETHODCALLTYPE GetStreamByNumber(WORD wStreamNum, IWMStreamConfig** ppConfig) override;
    HRESULT STDMETHODCALLTYPE RemoveStream(IWMStreamConfig* pConfig) override;
    HRESULT STDMETHODCALLTYPE RemoveStreamByNumber(WORD wStreamNum) override;
    HRESULT STDMETHODCALLTYPE AddStream(IWMStreamConfig* pConfig) override;
    HRESULT STDMETHODCALLTYPE ReconfigStream(IWMStreamConfig* pConfig) override;
    HRESULT STDMETHODCALLTYPE CreateNewStream(REFGUID guidStreamType, IWMStreamConfig** ppConfig) override;
    HRESULT STDMETHODCALLTYPE GetMutualExclusionCount(DWORD* pcME) override;
    HRESULT STDMETHODCALLTYPE GetMutualExclusion(DWORD dwMEIndex, IWMMutualExclusion** ppME) override;
    HRESULT STDMETHODCALLTYPE RemoveMutualExclusion(IWMMutualExclusion* pME) override;
    HRESULT STDMETHODCALLTYPE AddMutualExclusion(IWMMutualExclusion* pME) override;
    HRESULT STDMETHODCALLTYPE CreateNewMutualExclusion(IWMMutualExclusion** ppME) override;

private:
    bool EnsureAdv();
    bool EnsureHeaderInfo();
    bool EnsureProfile();

    volatile LONG refCount_ = 1;
    int32_t readerId_ = 0;
    IWMReader* real_ = nullptr;
    IWMReaderAdvanced* realAdv_ = nullptr;
    IWMReaderAdvanced2* realAdv2_ = nullptr;
    IWMHeaderInfo* realHi_ = nullptr;
    IWMHeaderInfo2* realHi2_ = nullptr;
    IWMHeaderInfo3* realHi3_ = nullptr;
    IWMProfile* realProfile_ = nullptr;
};

} // namespace probe
