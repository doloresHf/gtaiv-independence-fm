#include "stage_d_capture.h"

#include <avrt.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <tlhelp32.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")
#endif

namespace staged {

namespace {

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, pid = 14
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

constexpr uint32_t kMinSampleRate = 8000u;
constexpr uint32_t kMaxSampleRate = 384000u;
constexpr REFERENCE_TIME kLoopbackBufferDuration100ns = 200000; // 20 ms

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const noexcept { return ptr_; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void Attach(T* p) noexcept {
        Reset();
        ptr_ = p;
    }
    T* Detach() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }
    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

template <typename T>
class CoTaskMemPtr {
public:
    CoTaskMemPtr() = default;
    ~CoTaskMemPtr() { Reset(); }
    CoTaskMemPtr(const CoTaskMemPtr&) = delete;
    CoTaskMemPtr& operator=(const CoTaskMemPtr&) = delete;

    T* Get() const noexcept { return ptr_; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void Reset() noexcept {
        if (ptr_) {
            CoTaskMemFree(ptr_);
            ptr_ = nullptr;
        }
    }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle_(h) {}
    ~ScopedHandle() { Reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const noexcept { return handle_; }
    bool IsValid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE h = nullptr) noexcept {
        if (IsValid()) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

private:
    HANDLE handle_ = nullptr;
};

class ComMtaGuard {
public:
    ComMtaGuard() noexcept {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        shouldUninit_ = (hr_ == S_OK || hr_ == S_FALSE);
    }
    ~ComMtaGuard() {
        if (shouldUninit_) {
            CoUninitialize();
        }
    }
    ComMtaGuard(const ComMtaGuard&) = delete;
    ComMtaGuard& operator=(const ComMtaGuard&) = delete;

    HRESULT HResult() const noexcept { return hr_; }
    bool Usable() const noexcept {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT hr_ = E_FAIL;
    bool shouldUninit_ = false;
};

class ProcessLoopbackCompletionHandler final
    : public IActivateAudioInterfaceCompletionHandler
    , public IAgileObject {
public:
    ProcessLoopbackCompletionHandler() noexcept
        : completedEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ProcessLoopbackCompletionHandler(const ProcessLoopbackCompletionHandler&) = delete;
    ProcessLoopbackCompletionHandler& operator=(const ProcessLoopbackCompletionHandler&) = delete;

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rc = static_cast<ULONG>(InterlockedDecrement(&refCount_));
        if (rc == 0) {
            delete this;
        }
        return rc;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppvObject = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *ppvObject = static_cast<IAgileObject*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // IActivateAudioInterfaceCompletionHandler
    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* activateOperation) override {
        if (!activateOperation) {
            activateHr_ = E_INVALIDARG;
        } else {
            HRESULT hrOp = E_FAIL;
            IUnknown* unk = nullptr;
            const HRESULT hrGet = activateOperation->GetActivateResult(&hrOp, &unk);
            if (FAILED(hrGet)) {
                activateHr_ = hrGet;
                if (unk) {
                    unk->Release();
                }
            } else {
                activateHr_ = hrOp;
                activatedUnknown_ = unk;
            }
        }
        if (completedEvent_) {
            SetEvent(completedEvent_);
        }
        return S_OK;
    }

    HANDLE EventHandle() const noexcept { return completedEvent_; }
    HRESULT ActivateResultHr() const noexcept { return activateHr_; }

    // Safe to call ONLY after WaitForSingleObject(completedEvent_) == WAIT_OBJECT_0
    IUnknown* DetachActivatedInterface() noexcept {
        IUnknown* p = activatedUnknown_;
        activatedUnknown_ = nullptr;
        return p;
    }

private:
    ~ProcessLoopbackCompletionHandler() {
        if (activatedUnknown_) {
            activatedUnknown_->Release();
            activatedUnknown_ = nullptr;
        }
        if (completedEvent_) {
            CloseHandle(completedEvent_);
            completedEvent_ = nullptr;
        }
    }

    volatile LONG refCount_ = 1;
    HANDLE completedEvent_ = nullptr;
    HRESULT activateHr_ = E_FAIL;
    IUnknown* activatedUnknown_ = nullptr;
};

int16_t FloatSampleToInt16(float sample) noexcept {
    // 1. Sanitize NaN / +Inf / -Inf to exact digital silence (0).
    if (!std::isfinite(sample)) {
        return 0;
    }
    // 2. Clamp finite out-of-range values (e.g. +/-1e30f) to [-1.0f, 1.0f] BEFORE llround
    //    so std::llround never receives an unrepresentable value (eliminating overflow UB).
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    // 3. Asymmetric linear scaling: -1.0f -> -32768, +1.0f -> +32767, +/-0.0f -> 0.
    const double scale = (clamped < 0.0f) ? 32768.0 : 32767.0;
    const long long rounded = std::llround(static_cast<double>(clamped) * scale);
    return static_cast<int16_t>(std::clamp(rounded, -32768LL, 32767LL));
}

bool QueryProcessImageFullPath(DWORD pid, std::wstring& outPath) {
    outPath.clear();
    if (pid == 0) {
        return false;
    }
    ScopedHandle hProc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!hProc.IsValid()) {
        return false;
    }
    DWORD cap = 1024;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::wstring buf(cap, L'\0');
        DWORD len = cap;
        if (QueryFullProcessImageNameW(hProc.Get(), 0, buf.data(), &len)) {
            buf.resize(len);
            outPath = std::move(buf);
            return !outPath.empty();
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
        cap *= 2;
    }
    return false;
}

bool ReadEndpointFriendlyName(IMMDevice* device, std::wstring& outName) {
    outName.clear();
    if (!device) {
        return false;
    }
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.Put())) || !props) {
        return false;
    }
    PROPVARIANT pv;
    PropVariantInit(&pv);
    const HRESULT hr = props->GetValue(kPkeyDeviceFriendlyName, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        outName = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return !outName.empty();
}

std::wstring GetDefaultEndpointId(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role) {
    if (!enumerator) {
        return {};
    }
    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, dev.Put())) || !dev) {
        return {};
    }
    CoTaskMemPtr<wchar_t> id;
    if (FAILED(dev->GetId(id.Put())) || !id) {
        return {};
    }
    return std::wstring(id.Get());
}

std::vector<uint8_t> BuildWaveFormatExBytes(uint32_t sampleRate,
                                            uint16_t channels,
                                            CaptureSampleType sampleType) {
    WAVEFORMATEX wfx{};
    const uint16_t bits = (sampleType == CaptureSampleType::Pcm16) ? 16u : 32u;
    wfx.wFormatTag = (sampleType == CaptureSampleType::Pcm16)
        ? WAVE_FORMAT_PCM
        : WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = static_cast<WORD>(channels * (bits / 8u));
    wfx.nAvgBytesPerSec = sampleRate * static_cast<uint32_t>(wfx.nBlockAlign);
    wfx.cbSize = 0;

    std::vector<uint8_t> bytes(sizeof(WAVEFORMATEX));
    std::memcpy(bytes.data(), &wfx, sizeof(WAVEFORMATEX));
    return bytes;
}

std::string FormatFloatJson(float v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    char buf[64]{};
    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 9);
    if (res.ec == std::errc{}) {
        return std::string(buf, res.ptr);
    }
    std::snprintf(buf, sizeof(buf), "%.7g", static_cast<double>(v));
    return buf;
}

// ============================================================================
// Lightweight Standards-Compliant JSON Value & Recursive-Descent Parser
// ============================================================================

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::vector<std::pair<std::string, JsonValue>> objVal;

    const JsonValue* Find(const char* key) const noexcept {
        if (type != Type::Object || !key) {
            return nullptr;
        }
        for (const auto& kv : objVal) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

void AppendUtf8CodePoint(uint32_t cp, std::string& out) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

bool ParseHex4(const char* p, uint32_t& outHex) noexcept {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (c - 'A' + 10);
        } else {
            return false;
        }
    }
    outHex = v;
    return true;
}

bool UnescapeJsonContent(const std::string& raw,
                         size_t start,
                         size_t end,
                         std::string& outUtf8,
                         std::string& outError) {
    outUtf8.clear();
    outUtf8.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        if (ch < 0x20u) {
            outError = "Unescaped control character in JSON string";
            return false;
        }
        if (ch != '\\') {
            outUtf8.push_back(static_cast<char>(ch));
            continue;
        }
        if (i + 1 >= end) {
            outError = "Dangling backslash at end of JSON string";
            return false;
        }
        const char esc = raw[++i];
        switch (esc) {
        case '"':  outUtf8.push_back('"'); break;
        case '\\': outUtf8.push_back('\\'); break;
        case '/':  outUtf8.push_back('/'); break;
        case 'b':  outUtf8.push_back('\b'); break;
        case 'f':  outUtf8.push_back('\f'); break;
        case 'n':  outUtf8.push_back('\n'); break;
        case 'r':  outUtf8.push_back('\r'); break;
        case 't':  outUtf8.push_back('\t'); break;
        case 'u': {
            if (i + 4 >= end) {
                outError = "Truncated \\uXXXX escape in JSON string";
                return false;
            }
            uint32_t u1 = 0;
            if (!ParseHex4(raw.data() + i + 1, u1)) {
                outError = "Invalid hex digits in \\uXXXX escape";
                return false;
            }
            i += 4;
            if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
                if (i + 6 < end && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                    uint32_t u2 = 0;
                    if (ParseHex4(raw.data() + i + 3, u2) && u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                        i += 6;
                        const uint32_t cp = 0x10000u + (((u1 - 0xD800u) << 10) | (u2 - 0xDC00u));
                        AppendUtf8CodePoint(cp, outUtf8);
                        break;
                    }
                }
                outError = "Unpaired UTF-16 high surrogate in JSON string";
                return false;
            }
            if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                outError = "Unexpected UTF-16 low surrogate in JSON string";
                return false;
            }
            AppendUtf8CodePoint(u1, outUtf8);
            break;
        }
        default:
            outError = std::string("Unknown JSON escape sequence \\") + esc;
            return false;
        }
    }
    return true;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& input) noexcept : s_(input) {}

    bool ParseRoot(JsonValue& out, std::string& outError) {
        SkipWs();
        if (!ParseValue(out, 0, outError)) {
            return false;
        }
        SkipWs();
        if (pos_ != s_.size()) {
            outError = "Unexpected trailing content after JSON root";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    void SkipWs() noexcept {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool ParseValue(JsonValue& out, int depth, std::string& outError) {
        if (depth > kMaxDepth) {
            outError = "JSON nesting depth exceeded";
            return false;
        }
        SkipWs();
        if (pos_ >= s_.size()) {
            outError = "Unexpected end of JSON input";
            return false;
        }
        const char c = s_[pos_];
        if (c == '{') return ParseObject(out, depth + 1, outError);
        if (c == '[') return ParseArray(out, depth + 1, outError);
        if (c == '"') {
            out = JsonValue{};
            out.type = JsonValue::Type::String;
            return ParseString(out.strVal, outError);
        }
        if (c == 't') {
            if (s_.compare(pos_, 4, "true") == 0) {
                pos_ += 4;
                out = JsonValue{};
                out.type = JsonValue::Type::Bool;
                out.boolVal = true;
                return true;
            }
            outError = "Invalid token starting with 't'";
            return false;
        }
        if (c == 'f') {
            if (s_.compare(pos_, 5, "false") == 0) {
                pos_ += 5;
                out = JsonValue{};
                out.type = JsonValue::Type::Bool;
                out.boolVal = false;
                return true;
            }
            outError = "Invalid token starting with 'f'";
            return false;
        }
        if (c == 'n') {
            if (s_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out = JsonValue{};
                out.type = JsonValue::Type::Null;
                return true;
            }
            outError = "Invalid token starting with 'n'";
            return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber(out, outError);
        }
        outError = std::string("Unexpected character in JSON: ") + c;
        return false;
    }

    bool ParseString(std::string& out, std::string& outError) {
        if (pos_ >= s_.size() || s_[pos_] != '"') {
            outError = "Expected '\"'";
            return false;
        }
        ++pos_;
        const size_t start = pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '"') {
                const size_t end = pos_;
                ++pos_;
                return UnescapeJsonContent(s_, start, end, out, outError);
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) {
                    outError = "Unterminated escape in JSON string";
                    return false;
                }
            }
            ++pos_;
        }
        outError = "Unterminated JSON string";
        return false;
    }

    bool ParseNumber(JsonValue& out, std::string& outError) {
        const size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size()) {
            outError = "Invalid JSON number";
            return false;
        }
        if (s_[pos_] == '0') {
            ++pos_;
        } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        } else {
            outError = "Invalid JSON number digits";
            return false;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
                outError = "Invalid JSON fraction";
                return false;
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
                outError = "Invalid JSON exponent";
                return false;
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        double val = 0.0;
        const auto res = std::from_chars(s_.data() + start, s_.data() + pos_, val);
        if (res.ec != std::errc{} || res.ptr != s_.data() + pos_ || !std::isfinite(val)) {
            outError = "Out-of-range JSON number";
            return false;
        }
        out = JsonValue{};
        out.type = JsonValue::Type::Number;
        out.numVal = val;
        return true;
    }

    bool ParseArray(JsonValue& out, int depth, std::string& outError) {
        ++pos_; // skip '['
        out = JsonValue{};
        out.type = JsonValue::Type::Array;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!ParseValue(elem, depth, outError)) {
                return false;
            }
            out.arrVal.push_back(std::move(elem));
            SkipWs();
            if (pos_ >= s_.size()) {
                outError = "Unterminated JSON array";
                return false;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') {
                outError = "Expected ',' or ']' in JSON array";
                return false;
            }
            ++pos_;
        }
    }

    bool ParseObject(JsonValue& out, int depth, std::string& outError) {
        ++pos_; // skip '{'
        out = JsonValue{};
        out.type = JsonValue::Type::Object;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipWs();
            std::string key;
            if (!ParseString(key, outError)) {
                return false;
            }
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                outError = "Expected ':' after JSON object key";
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!ParseValue(val, depth, outError)) {
                return false;
            }
            out.objVal.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (pos_ >= s_.size()) {
                outError = "Unterminated JSON object";
                return false;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (s_[pos_] != ',') {
                outError = "Expected ',' or '}' in JSON object";
                return false;
            }
            ++pos_;
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool StrictSessionMatch(const SessionVolumeIdentity& a,
                        const SessionVolumeIdentity& b) noexcept {
    if (a.endpointId.empty() || b.endpointId.empty()) return false;
    if (a.sessionInstanceId.empty() || b.sessionInstanceId.empty()) return false;
    if (a.exeFullPath.empty() || b.exeFullPath.empty()) return false;
    return (a.endpointId == b.endpointId) &&
           (a.sessionInstanceId == b.sessionInstanceId) &&
           EqualsIgnoreCase(a.exeFullPath, b.exeFullPath);
}

bool StrictSessionMatchEntry(const JournalSessionEntry& entry,
                             const SessionVolumeIdentity& live) noexcept {
    if (entry.endpointId.empty() || live.endpointId.empty()) return false;
    if (entry.sessionInstanceId.empty() || live.sessionInstanceId.empty()) return false;
    if (entry.exeFullPath.empty() || live.exeFullPath.empty()) return false;
    // Deliberately ignore entry.recordedPid vs live.processId: PID alone must NEVER be trusted.
    return (entry.endpointId == live.endpointId) &&
           (entry.sessionInstanceId == live.sessionInstanceId) &&
           EqualsIgnoreCase(entry.exeFullPath, live.exeFullPath);
}

bool EnumerateWin32SessionsInternal(std::vector<AudioSessionHandleInfo>& outSessions,
                                    std::string& outError) {
    outSessions.clear();
    outError.clear();

    ComMtaGuard com;
    if (!com.Usable()) {
        outError = "CoInitializeEx failed: " + FormatHResultHex(com.HResult());
        return false;
    }

    std::vector<ProcessEntry> procs;
    std::string procErr;
    SnapshotProcesses(procs, procErr);
    std::unordered_map<uint32_t, ProcessEntry> procMap;
    procMap.reserve(procs.size());
    for (const auto& p : procs) {
        procMap[p.pid] = p;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(hr) || !enumerator) {
        outError = "CoCreateInstance(MMDeviceEnumerator) failed: " + FormatHResultHex(hr);
        return false;
    }

    ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.Put());
    if (FAILED(hr) || !devices) {
        outError = "EnumAudioEndpoints(eRender) failed: " + FormatHResultHex(hr);
        return false;
    }

    UINT devCount = 0;
    hr = devices->GetCount(&devCount);
    if (FAILED(hr)) {
        outError = "IMMDeviceCollection::GetCount failed: " + FormatHResultHex(hr);
        return false;
    }

    for (UINT d = 0; d < devCount; ++d) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(d, device.Put())) || !device) {
            continue;
        }
        CoTaskMemPtr<wchar_t> epIdPtr;
        if (FAILED(device->GetId(epIdPtr.Put())) || !epIdPtr) {
            continue;
        }
        const std::wstring endpointId = epIdPtr.Get();

        ComPtr<IAudioSessionManager2> mgr;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(mgr.Put()))) || !mgr) {
            continue;
        }
        ComPtr<IAudioSessionEnumerator> sessionEnum;
        if (FAILED(mgr->GetSessionEnumerator(sessionEnum.Put())) || !sessionEnum) {
            continue;
        }
        int sessCount = 0;
        if (FAILED(sessionEnum->GetCount(&sessCount))) {
            continue;
        }
        for (int s = 0; s < sessCount; ++s) {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessionEnum->GetSession(s, ctrl.Put())) || !ctrl) {
                continue;
            }
            AudioSessionState stateVal = AudioSessionStateInactive;
            ctrl->GetState(&stateVal);

            CoTaskMemPtr<wchar_t> dispPtr;
            ctrl->GetDisplayName(dispPtr.Put());

            ComPtr<IAudioSessionControl2> ctrl2;
            if (FAILED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                            reinterpret_cast<void**>(ctrl2.Put()))) || !ctrl2) {
                continue;
            }

            CoTaskMemPtr<wchar_t> sidPtr;
            CoTaskMemPtr<wchar_t> siidPtr;
            ctrl2->GetSessionIdentifier(sidPtr.Put());
            ctrl2->GetSessionInstanceIdentifier(siidPtr.Put());

            DWORD pid = 0;
            const HRESULT hrPid = ctrl2->GetProcessId(&pid);
            const bool multiProc = (hrPid == kAudclntSNoSingleProcess || hrPid == kAudclntENoSingleProcess);
            const bool sysSounds = (ctrl2->IsSystemSoundsSession() == S_OK);

            std::wstring exeFullPath;
            if (SUCCEEDED(hrPid) && pid != 0) {
                auto it = procMap.find(pid);
                if (it != procMap.end() && !it->second.exeFullPath.empty()) {
                    exeFullPath = it->second.exeFullPath;
                } else {
                    QueryProcessImageFullPath(pid, exeFullPath);
                }
            }

            ComPtr<ISimpleAudioVolume> vol;
            if (FAILED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                            reinterpret_cast<void**>(vol.Put()))) || !vol) {
                continue;
            }
            BOOL bMute = FALSE;
            float fVol = 1.0f;
            if (FAILED(vol->GetMute(&bMute)) || FAILED(vol->GetMasterVolume(&fVol))) {
                continue;
            }

            AudioSessionHandleInfo info;
            info.identity.endpointId = endpointId;
            info.identity.sessionInstanceId = siidPtr ? siidPtr.Get() : L"";
            info.identity.exeFullPath = exeFullPath;
            info.identity.processId = pid;
            info.identity.isMultiProcessSession = multiProc;
            info.identity.isSystemSoundsSession = sysSounds;
            info.state.mute = (bMute != FALSE);
            info.state.masterVolume = fVol;
            info.sessionState = static_cast<uint32_t>(stateVal);
            info.sessionId = sidPtr ? sidPtr.Get() : L"";
            info.displayName = dispPtr ? dispPtr.Get() : L"";
            outSessions.push_back(std::move(info));
        }
    }

    return true;
}

class WavStreamWriter {
public:
    ~WavStreamWriter() { Close(); }

    bool Open(const std::wstring& path, uint32_t sampleRate, uint16_t channels, std::string& outError) {
        Close();
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::filesystem::create_directories(fsPath.parent_path(), ec);
        }
        hFile_.Reset(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!hFile_.IsValid()) {
            outError = "CreateFileW WAV failed: err=" + std::to_string(GetLastError());
            return false;
        }
        sampleRate_ = sampleRate;
        channels_ = channels;
        uint8_t hdr[44]{};
        BuildHeader(hdr, 0u);
        DWORD written = 0;
        if (!WriteFile(hFile_.Get(), hdr, sizeof(hdr), &written, nullptr) || written != sizeof(hdr)) {
            outError = "WriteFile WAV header failed: err=" + std::to_string(GetLastError());
            Close();
            return false;
        }
        return true;
    }

    bool AppendPcm16(const std::vector<int16_t>& samples, std::string& outError) {
        if (!hFile_.IsValid() || samples.empty()) {
            return true;
        }
        const uint64_t bytes64 = static_cast<uint64_t>(samples.size()) * sizeof(int16_t);
        if (dataBytes_ + bytes64 > 0xFFFFFFFFULL - 36ULL) {
            outError = "WAV file size exceeds 4 GiB RIFF limit";
            return false;
        }
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(samples.data());
        DWORD remaining = static_cast<DWORD>(bytes64);
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(hFile_.Get(), ptr, remaining, &written, nullptr) || written == 0) {
                outError = "WriteFile WAV payload failed: err=" + std::to_string(GetLastError());
                return false;
            }
            ptr += written;
            remaining -= written;
            dataBytes_ += written;
        }
        return true;
    }

    bool Finalize(std::string& outError) {
        if (!hFile_.IsValid()) {
            return true;
        }
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(hFile_.Get(), zero, nullptr, FILE_BEGIN)) {
            outError = "SetFilePointerEx WAV failed: err=" + std::to_string(GetLastError());
            Close();
            return false;
        }
        uint8_t hdr[44]{};
        BuildHeader(hdr, static_cast<uint32_t>(dataBytes_));
        DWORD written = 0;
        if (!WriteFile(hFile_.Get(), hdr, sizeof(hdr), &written, nullptr) || written != sizeof(hdr)) {
            outError = "WriteFile WAV final header failed: err=" + std::to_string(GetLastError());
            Close();
            return false;
        }
        FlushFileBuffers(hFile_.Get());
        Close();
        return true;
    }

    uint64_t DataBytes() const noexcept { return dataBytes_; }

    void Close() noexcept { hFile_.Reset(); }

private:
    void BuildHeader(uint8_t hdr[44], uint32_t dataBytes) const noexcept {
        const uint32_t riffChunkSize = 36u + dataBytes;
        const uint32_t fmtChunkSize = 16u;
        const uint16_t audioFormat = 1u;
        const uint16_t bitsPerSample = 16u;
        const uint16_t blockAlign = static_cast<uint16_t>(channels_ * 2u);
        const uint32_t byteRate = sampleRate_ * static_cast<uint32_t>(blockAlign);

        std::memcpy(hdr + 0, "RIFF", 4);
        std::memcpy(hdr + 4, &riffChunkSize, 4);
        std::memcpy(hdr + 8, "WAVE", 4);
        std::memcpy(hdr + 12, "fmt ", 4);
        std::memcpy(hdr + 16, &fmtChunkSize, 4);
        std::memcpy(hdr + 20, &audioFormat, 2);
        std::memcpy(hdr + 22, &channels_, 2);
        std::memcpy(hdr + 24, &sampleRate_, 4);
        std::memcpy(hdr + 28, &byteRate, 4);
        std::memcpy(hdr + 32, &blockAlign, 2);
        std::memcpy(hdr + 34, &bitsPerSample, 2);
        std::memcpy(hdr + 36, "data", 4);
        std::memcpy(hdr + 40, &dataBytes, 4);
    }

    ScopedHandle hFile_;
    uint32_t sampleRate_ = 48000u;
    uint16_t channels_ = 2u;
    uint64_t dataBytes_ = 0u;
};

} // namespace

// ============================================================================
// Shared String, JSON & File Utilities
// ============================================================================

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        out.data(), needed);
    return out;
}

std::string EscapeJsonString(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 16);
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

bool UnescapeJsonString(const std::string& escaped, std::string& outUtf8, std::string& outError) {
    return UnescapeJsonContent(escaped, 0, escaped.size(), outUtf8, outError);
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring ExtractLeafFilename(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string FormatHResultHex(HRESULT hr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string GetCurrentUtcIsoString() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

bool WriteTextFileAtomic(const std::wstring& path, const std::string& contentUtf8, std::string& outError) {
    outError.clear();
    if (path.empty()) {
        outError = "Target file path is empty";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path fsPath(path);
    if (fsPath.has_parent_path()) {
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    wchar_t tmpSuffix[64]{};
    std::swprintf(tmpSuffix, 64, L".tmp.%lu.%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetCurrentThreadId()));
    const std::wstring tmpPath = path + tmpSuffix;

    ScopedHandle hFile(CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!hFile.IsValid()) {
        outError = "CreateFileW temp file failed: err=" + std::to_string(GetLastError());
        return false;
    }

    const char* ptr = contentUtf8.data();
    size_t remaining = contentUtf8.size();
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(hFile.Get(), ptr, chunk, &written, nullptr) || written == 0) {
            outError = "WriteFile temp file failed: err=" + std::to_string(GetLastError());
            hFile.Reset();
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        ptr += written;
        remaining -= written;
    }

    if (!FlushFileBuffers(hFile.Get())) {
        outError = "FlushFileBuffers failed: err=" + std::to_string(GetLastError());
        hFile.Reset();
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    hFile.Reset();

    if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        outError = "MoveFileExW failed: err=" + std::to_string(GetLastError());
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

bool ReadTextFileUtf8(const std::wstring& path, std::string& outContentUtf8, std::string& outError) {
    outContentUtf8.clear();
    outError.clear();
    ScopedHandle hFile(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!hFile.IsValid()) {
        outError = "CreateFileW read failed: err=" + std::to_string(GetLastError());
        return false;
    }
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile.Get(), &fileSize)) {
        outError = "GetFileSizeEx failed: err=" + std::to_string(GetLastError());
        return false;
    }
    if (fileSize.QuadPart < 0 || fileSize.QuadPart > (16LL << 20)) {
        outError = "Unexpected file size";
        return false;
    }
    outContentUtf8.resize(static_cast<size_t>(fileSize.QuadPart));
    size_t offset = 0;
    while (offset < outContentUtf8.size()) {
        const DWORD toRead = static_cast<DWORD>(std::min<size_t>(outContentUtf8.size() - offset, 1u << 20));
        DWORD readBytes = 0;
        if (!ReadFile(hFile.Get(), outContentUtf8.data() + offset, toRead, &readBytes, nullptr)) {
            outError = "ReadFile failed: err=" + std::to_string(GetLastError());
            outContentUtf8.clear();
            return false;
        }
        if (readBytes == 0) {
            break;
        }
        offset += readBytes;
    }
    outContentUtf8.resize(offset);
    return true;
}

// ============================================================================
// Section A: Format Validation & Sample Conversion
// ============================================================================

bool ValidateCaptureWaveFormat(const WAVEFORMATEX* wfx,
                               ValidatedCaptureFormat& out,
                               std::string& outError) {
    out = ValidatedCaptureFormat{};
    outError.clear();

    if (!wfx) {
        outError = "Null WAVEFORMATEX pointer";
        return false;
    }
    if (wfx->nChannels != 1u && wfx->nChannels != 2u) {
        outError = "Unsupported channel count " + std::to_string(wfx->nChannels) +
                   " (only 1 or 2 channels supported)";
        return false;
    }
    if (wfx->nSamplesPerSec < kMinSampleRate || wfx->nSamplesPerSec > kMaxSampleRate) {
        outError = "Unsupported sample rate " + std::to_string(wfx->nSamplesPerSec) +
                   " Hz (expected 8000..384000)";
        return false;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        if (wfx->wBitsPerSample != 16u) {
            outError = "WAVE_FORMAT_PCM requires 16 bits/sample, got " +
                       std::to_string(wfx->wBitsPerSample);
            return false;
        }
        const uint16_t expectedAlign = static_cast<uint16_t>(wfx->nChannels * 2u);
        if (wfx->nBlockAlign != expectedAlign) {
            outError = "WAVE_FORMAT_PCM invalid nBlockAlign=" + std::to_string(wfx->nBlockAlign) +
                       " (expected " + std::to_string(expectedAlign) + ")";
            return false;
        }
        out.sampleRate = wfx->nSamplesPerSec;
        out.channels = wfx->nChannels;
        out.bitsPerSample = 16u;
        out.blockAlign = expectedAlign;
        out.sampleType = CaptureSampleType::Pcm16;
        out.isExtensible = false;
        out.channelMask = 0u;
        return true;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        if (wfx->wBitsPerSample != 32u) {
            outError = "WAVE_FORMAT_IEEE_FLOAT requires 32 bits/sample, got " +
                       std::to_string(wfx->wBitsPerSample);
            return false;
        }
        const uint16_t expectedAlign = static_cast<uint16_t>(wfx->nChannels * 4u);
        if (wfx->nBlockAlign != expectedAlign) {
            outError = "WAVE_FORMAT_IEEE_FLOAT invalid nBlockAlign=" + std::to_string(wfx->nBlockAlign) +
                       " (expected " + std::to_string(expectedAlign) + ")";
            return false;
        }
        out.sampleRate = wfx->nSamplesPerSec;
        out.channels = wfx->nChannels;
        out.bitsPerSample = 32u;
        out.blockAlign = expectedAlign;
        out.sampleType = CaptureSampleType::Float32;
        out.isExtensible = false;
        out.channelMask = 0u;
        return true;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wfx->cbSize < 22u) {
            outError = "WAVE_FORMAT_EXTENSIBLE cbSize=" + std::to_string(wfx->cbSize) +
                       " is less than required 22";
            return false;
        }
        const auto* wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (wfxe->Samples.wValidBitsPerSample != wfx->wBitsPerSample) {
            outError = "WAVE_FORMAT_EXTENSIBLE wValidBitsPerSample=" +
                       std::to_string(wfxe->Samples.wValidBitsPerSample) +
                       " != wBitsPerSample=" + std::to_string(wfx->wBitsPerSample);
            return false;
        }
        if (wfxe->dwChannelMask != 0u) {
            if (wfx->nChannels == 1u) {
                if (wfxe->dwChannelMask != SPEAKER_FRONT_CENTER &&
                    wfxe->dwChannelMask != SPEAKER_FRONT_LEFT) {
                    char buf[96]{};
                    std::snprintf(buf, sizeof(buf),
                                  "Unsupported 1ch dwChannelMask=0x%lX (allowed: 0, 0x1, 0x4)",
                                  static_cast<unsigned long>(wfxe->dwChannelMask));
                    outError = buf;
                    return false;
                }
            } else {
                if (wfxe->dwChannelMask != (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) {
                    char buf[96]{};
                    std::snprintf(buf, sizeof(buf),
                                  "Unsupported 2ch dwChannelMask=0x%lX (allowed: 0, 0x3)",
                                  static_cast<unsigned long>(wfxe->dwChannelMask));
                    outError = buf;
                    return false;
                }
            }
        }

        CaptureSampleType resolvedType = CaptureSampleType::Unknown;
        if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (wfx->wBitsPerSample != 16u) {
                outError = "EXTENSIBLE PCM requires 16 bits/sample, got " +
                           std::to_string(wfx->wBitsPerSample);
                return false;
            }
            resolvedType = CaptureSampleType::Pcm16;
        } else if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            if (wfx->wBitsPerSample != 32u) {
                outError = "EXTENSIBLE IEEE_FLOAT requires 32 bits/sample, got " +
                           std::to_string(wfx->wBitsPerSample);
                return false;
            }
            resolvedType = CaptureSampleType::Float32;
        } else {
            outError = "Unsupported WAVE_FORMAT_EXTENSIBLE SubFormat GUID";
            return false;
        }

        const uint16_t expectedAlign =
            static_cast<uint16_t>(wfx->nChannels * (wfx->wBitsPerSample / 8u));
        if (wfx->nBlockAlign != expectedAlign) {
            outError = "WAVE_FORMAT_EXTENSIBLE invalid nBlockAlign=" +
                       std::to_string(wfx->nBlockAlign) + " (expected " +
                       std::to_string(expectedAlign) + ")";
            return false;
        }

        out.sampleRate = wfx->nSamplesPerSec;
        out.channels = wfx->nChannels;
        out.bitsPerSample = wfx->wBitsPerSample;
        out.blockAlign = expectedAlign;
        out.sampleType = resolvedType;
        out.isExtensible = true;
        out.channelMask = wfxe->dwChannelMask;
        return true;
    }

    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "Unsupported wFormatTag=0x%04X",
                  static_cast<unsigned>(wfx->wFormatTag));
    outError = buf;
    return false;
}

std::string FormatWaveFormatSummary(const WAVEFORMATEX* wfx) {
    if (!wfx) {
        return "<null WAVEFORMATEX>";
    }
    char buf[256]{};
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22u) {
        const auto* wfxe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        const char* subName = "OTHER_GUID";
        if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            subName = "PCM";
        } else if (IsEqualGUID(wfxe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            subName = "IEEE_FLOAT";
        }
        std::snprintf(buf, sizeof(buf),
                      "%lu Hz, %u ch, %u-bit (valid=%u), EXTENSIBLE(%s), align=%u, mask=0x%lX",
                      static_cast<unsigned long>(wfx->nSamplesPerSec),
                      static_cast<unsigned>(wfx->nChannels),
                      static_cast<unsigned>(wfx->wBitsPerSample),
                      static_cast<unsigned>(wfxe->Samples.wValidBitsPerSample),
                      subName,
                      static_cast<unsigned>(wfx->nBlockAlign),
                      static_cast<unsigned long>(wfxe->dwChannelMask));
        return buf;
    }

    const char* tagName = "OTHER";
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) tagName = "PCM";
    else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) tagName = "IEEE_FLOAT";
    else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) tagName = "EXTENSIBLE_TRUNCATED";

    std::snprintf(buf, sizeof(buf),
                  "%lu Hz, %u ch, %u-bit, %s(0x%04X), align=%u, cbSize=%u",
                  static_cast<unsigned long>(wfx->nSamplesPerSec),
                  static_cast<unsigned>(wfx->nChannels),
                  static_cast<unsigned>(wfx->wBitsPerSample),
                  tagName,
                  static_cast<unsigned>(wfx->wFormatTag),
                  static_cast<unsigned>(wfx->nBlockAlign),
                  static_cast<unsigned>(wfx->cbSize));
    return buf;
}

std::string DescribeValidatedFormat(const ValidatedCaptureFormat& fmt) {
    const char* typeStr = "Unknown";
    if (fmt.sampleType == CaptureSampleType::Pcm16) typeStr = "PCM16";
    else if (fmt.sampleType == CaptureSampleType::Float32) typeStr = "Float32";
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "%u Hz / %uch / %s%s",
                  static_cast<unsigned>(fmt.sampleRate),
                  static_cast<unsigned>(fmt.channels),
                  typeStr,
                  fmt.isExtensible ? " (extensible)" : "");
    return buf;
}

void ConvertCaptureChunkToStereoInt16(const ValidatedCaptureFormat& fmt,
                                      const void* inBytes,
                                      uint32_t frameCount,
                                      bool silentFlag,
                                      std::vector<int16_t>& outStereoLR) {
    if (frameCount == 0u) {
        outStereoLR.clear();
        return;
    }
    const size_t outSamples = static_cast<size_t>(frameCount) * 2u;
    if (silentFlag || !inBytes ||
        (fmt.channels != 1u && fmt.channels != 2u) ||
        fmt.sampleType == CaptureSampleType::Unknown) {
        outStereoLR.assign(outSamples, 0);
        return;
    }

    outStereoLR.resize(outSamples);
    const auto* raw = static_cast<const uint8_t*>(inBytes);

    if (fmt.sampleType == CaptureSampleType::Pcm16) {
        if (fmt.channels == 2u) {
            std::memcpy(outStereoLR.data(), raw, static_cast<size_t>(frameCount) * 4u);
        } else {
            for (uint32_t i = 0; i < frameCount; ++i) {
                int16_t s = 0;
                std::memcpy(&s, raw + static_cast<size_t>(i) * sizeof(int16_t), sizeof(int16_t));
                outStereoLR[static_cast<size_t>(i) * 2u] = s;
                outStereoLR[static_cast<size_t>(i) * 2u + 1u] = s;
            }
        }
        return;
    }

    // Float32
    if (fmt.channels == 2u) {
        for (uint32_t i = 0; i < frameCount; ++i) {
            float l = 0.0f;
            float r = 0.0f;
            const size_t base = static_cast<size_t>(i) * 2u * sizeof(float);
            std::memcpy(&l, raw + base, sizeof(float));
            std::memcpy(&r, raw + base + sizeof(float), sizeof(float));
            outStereoLR[static_cast<size_t>(i) * 2u] = FloatSampleToInt16(l);
            outStereoLR[static_cast<size_t>(i) * 2u + 1u] = FloatSampleToInt16(r);
        }
    } else {
        for (uint32_t i = 0; i < frameCount; ++i) {
            float m = 0.0f;
            std::memcpy(&m, raw + static_cast<size_t>(i) * sizeof(float), sizeof(float));
            const int16_t q = FloatSampleToInt16(m);
            outStereoLR[static_cast<size_t>(i) * 2u] = q;
            outStereoLR[static_cast<size_t>(i) * 2u + 1u] = q;
        }
    }
}

// ============================================================================
// Section B: Topology Discovery, Process Tree & SessionMuteJournal
// ============================================================================

bool SnapshotProcesses(std::vector<ProcessEntry>& outProcesses, std::string& outError) {
    outProcesses.clear();
    outError.clear();

    ScopedHandle hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!hSnap.IsValid()) {
        outError = "CreateToolhelp32Snapshot failed: err=" + std::to_string(GetLastError());
        return false;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(hSnap.Get(), &pe)) {
        outError = "Process32FirstW failed: err=" + std::to_string(GetLastError());
        return false;
    }

    do {
        ProcessEntry entry;
        entry.pid = pe.th32ProcessID;
        entry.parentPid = pe.th32ParentProcessID;
        entry.exeName = pe.szExeFile;
        QueryProcessImageFullPath(entry.pid, entry.exeFullPath);
        outProcesses.push_back(std::move(entry));
    } while (Process32NextW(hSnap.Get(), &pe));

    return true;
}

std::vector<uint32_t> FindPidsByExeName(const std::vector<ProcessEntry>& processes,
                                        const std::wstring& exeName) {
    std::vector<uint32_t> pids;
    const std::wstring targetLeaf = ExtractLeafFilename(exeName);
    if (targetLeaf.empty()) {
        return pids;
    }
    for (const auto& p : processes) {
        if (EqualsIgnoreCase(ExtractLeafFilename(p.exeName), targetLeaf) ||
            (!p.exeFullPath.empty() && EqualsIgnoreCase(ExtractLeafFilename(p.exeFullPath), targetLeaf))) {
            pids.push_back(p.pid);
        }
    }
    return pids;
}

std::vector<uint32_t> CollectProcessTreePids(const std::vector<ProcessEntry>& processes,
                                             uint32_t rootPid) {
    std::vector<uint32_t> result;
    if (rootPid == 0) {
        return result;
    }
    std::unordered_set<uint32_t> visited;
    std::vector<uint32_t> queue;
    visited.insert(rootPid);
    queue.push_back(rootPid);
    result.push_back(rootPid);

    size_t head = 0;
    while (head < queue.size()) {
        const uint32_t cur = queue[head++];
        for (const auto& p : processes) {
            if (p.parentPid == cur && p.pid != 0 && visited.insert(p.pid).second) {
                queue.push_back(p.pid);
                result.push_back(p.pid);
            }
        }
    }
    return result;
}

bool DiscoverAudioTopology(AudioTopologyReport& outReport, std::string& outError) {
    outReport = AudioTopologyReport{};
    outError.clear();

    ComMtaGuard com;
    if (!com.Usable()) {
        outError = "CoInitializeEx failed: " + FormatHResultHex(com.HResult());
        return false;
    }

    std::string procErr;
    if (!SnapshotProcesses(outReport.processes, procErr)) {
        outReport.warnings.push_back("Process snapshot failed: " + procErr);
    }

    std::unordered_map<uint32_t, ProcessEntry> procMap;
    procMap.reserve(outReport.processes.size());
    for (const auto& p : outReport.processes) {
        procMap[p.pid] = p;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(hr) || !enumerator) {
        outError = "CoCreateInstance(MMDeviceEnumerator) failed: " + FormatHResultHex(hr);
        return false;
    }

    outReport.defaultConsoleEndpointId = GetDefaultEndpointId(enumerator.Get(), eRender, eConsole);
    outReport.defaultMultimediaEndpointId = GetDefaultEndpointId(enumerator.Get(), eRender, eMultimedia);
    outReport.defaultCommunicationsEndpointId = GetDefaultEndpointId(enumerator.Get(), eRender, eCommunications);

    ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.Put());
    if (FAILED(hr) || !devices) {
        outError = "EnumAudioEndpoints(eRender, ACTIVE) failed: " + FormatHResultHex(hr);
        return false;
    }

    UINT count = 0;
    hr = devices->GetCount(&count);
    if (FAILED(hr)) {
        outError = "IMMDeviceCollection::GetCount failed: " + FormatHResultHex(hr);
        return false;
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.Put())) || !device) {
            outReport.warnings.push_back("IMMDeviceCollection::Item(" + std::to_string(i) + ") failed");
            continue;
        }

        AudioEndpointRecord ep;
        CoTaskMemPtr<wchar_t> epIdPtr;
        if (SUCCEEDED(device->GetId(epIdPtr.Put())) && epIdPtr) {
            ep.endpointId = epIdPtr.Get();
        }
        ReadEndpointFriendlyName(device.Get(), ep.friendlyName);
        ep.isDefaultConsole = (!ep.endpointId.empty() && ep.endpointId == outReport.defaultConsoleEndpointId);
        ep.isDefaultMultimedia = (!ep.endpointId.empty() && ep.endpointId == outReport.defaultMultimediaEndpointId);
        ep.isDefaultCommunications = (!ep.endpointId.empty() && ep.endpointId == outReport.defaultCommunicationsEndpointId);

        {
            ComPtr<IAudioClient> client;
            const HRESULT hrAct = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                   reinterpret_cast<void**>(client.Put()));
            if (SUCCEEDED(hrAct) && client) {
                CoTaskMemPtr<WAVEFORMATEX> mixWfx;
                ep.mixFormatHr = client->GetMixFormat(mixWfx.Put());
                if (SUCCEEDED(ep.mixFormatHr) && mixWfx) {
                    ep.mixFormatDescription = FormatWaveFormatSummary(mixWfx.Get());
                    std::string valErr;
                    ep.mixFormatValid = ValidateCaptureWaveFormat(mixWfx.Get(), ep.validatedMixFormat, valErr);
                } else {
                    ep.mixFormatDescription = "GetMixFormat failed: " + FormatHResultHex(ep.mixFormatHr);
                }
            } else {
                ep.mixFormatHr = hrAct;
                ep.mixFormatDescription = "Activate(IAudioClient) failed: " + FormatHResultHex(hrAct);
            }
        }

        {
            ComPtr<IAudioSessionManager2> mgr;
            const HRESULT hrMgr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                                   reinterpret_cast<void**>(mgr.Put()));
            if (SUCCEEDED(hrMgr) && mgr) {
                ComPtr<IAudioSessionEnumerator> sessionEnum;
                ep.sessionEnumHr = mgr->GetSessionEnumerator(sessionEnum.Put());
                if (SUCCEEDED(ep.sessionEnumHr) && sessionEnum) {
                    int sCount = 0;
                    if (SUCCEEDED(sessionEnum->GetCount(&sCount))) {
                        for (int s = 0; s < sCount; ++s) {
                            ComPtr<IAudioSessionControl> ctrl;
                            if (FAILED(sessionEnum->GetSession(s, ctrl.Put())) || !ctrl) {
                                continue;
                            }
                            AudioSessionRecord rec;
                            rec.endpointId = ep.endpointId;
                            rec.endpointFriendlyName = ep.friendlyName;
                            rec.isDefaultConsoleEndpoint = ep.isDefaultConsole;
                            rec.isDefaultMultimediaEndpoint = ep.isDefaultMultimedia;

                            AudioSessionState st = AudioSessionStateInactive;
                            if (SUCCEEDED(ctrl->GetState(&st))) {
                                rec.sessionState = static_cast<uint32_t>(st);
                            }

                            CoTaskMemPtr<wchar_t> dispPtr;
                            if (SUCCEEDED(ctrl->GetDisplayName(dispPtr.Put())) && dispPtr) {
                                rec.displayName = dispPtr.Get();
                            }

                            ComPtr<IAudioSessionControl2> ctrl2;
                            if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                               reinterpret_cast<void**>(ctrl2.Put()))) && ctrl2) {
                                CoTaskMemPtr<wchar_t> sidPtr;
                                CoTaskMemPtr<wchar_t> siidPtr;
                                if (SUCCEEDED(ctrl2->GetSessionIdentifier(sidPtr.Put())) && sidPtr) {
                                    rec.sessionId = sidPtr.Get();
                                }
                                if (SUCCEEDED(ctrl2->GetSessionInstanceIdentifier(siidPtr.Put())) && siidPtr) {
                                    rec.sessionInstanceId = siidPtr.Get();
                                }
                                DWORD pid = 0;
                                rec.getProcessIdHr = ctrl2->GetProcessId(&pid);
                                rec.processId = pid;
                                rec.isMultiProcessSession =
                                    (rec.getProcessIdHr == kAudclntSNoSingleProcess ||
                                     rec.getProcessIdHr == kAudclntENoSingleProcess);
                                rec.isSystemSoundsSession = (ctrl2->IsSystemSoundsSession() == S_OK);

                                if (SUCCEEDED(rec.getProcessIdHr) && pid != 0) {
                                    auto it = procMap.find(pid);
                                    if (it != procMap.end()) {
                                        rec.parentProcessId = it->second.parentPid;
                                        rec.exeName = it->second.exeName;
                                        rec.exeFullPath = it->second.exeFullPath;
                                    } else {
                                        QueryProcessImageFullPath(pid, rec.exeFullPath);
                                        rec.exeName = ExtractLeafFilename(rec.exeFullPath);
                                    }
                                    if (rec.exeName.empty() && !rec.exeFullPath.empty()) {
                                        rec.exeName = ExtractLeafFilename(rec.exeFullPath);
                                    }
                                }
                            }

                            ComPtr<ISimpleAudioVolume> vol;
                            if (SUCCEEDED(ctrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                               reinterpret_cast<void**>(vol.Put()))) && vol) {
                                BOOL bMute = FALSE;
                                float fVol = 1.0f;
                                if (SUCCEEDED(vol->GetMute(&bMute)) && SUCCEEDED(vol->GetMasterVolume(&fVol))) {
                                    rec.muted = (bMute != FALSE);
                                    rec.masterVolume = fVol;
                                    rec.volumeValid = true;
                                }
                            }

                            ep.sessions.push_back(std::move(rec));
                        }
                    }
                }
            } else {
                ep.sessionEnumHr = hrMgr;
            }
        }

        outReport.endpoints.push_back(std::move(ep));
    }

    return true;
}

bool Win32AudioSessionAccess::EnumerateSessions(std::vector<AudioSessionHandleInfo>& outSessions,
                                                std::string& outError) {
    return EnumerateWin32SessionsInternal(outSessions, outError);
}

bool Win32AudioSessionAccess::SetSessionMuteAndVolume(const SessionVolumeIdentity& target,
                                                      bool mute,
                                                      float masterVolume,
                                                      std::string& outError) {
    outError.clear();
    if (target.endpointId.empty() || target.sessionInstanceId.empty() || target.exeFullPath.empty()) {
        outError = "Refusing SetSessionMuteAndVolume with incomplete target identity";
        return false;
    }

    ComMtaGuard com;
    if (!com.Usable()) {
        outError = "CoInitializeEx failed: " + FormatHResultHex(com.HResult());
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(hr) || !enumerator) {
        outError = "CoCreateInstance(MMDeviceEnumerator) failed: " + FormatHResultHex(hr);
        return false;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDevice(target.endpointId.c_str(), device.Put());
    if (FAILED(hr) || !device) {
        outError = "IMMDeviceEnumerator::GetDevice failed: " + FormatHResultHex(hr);
        return false;
    }

    ComPtr<IAudioSessionManager2> mgr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(mgr.Put()));
    if (FAILED(hr) || !mgr) {
        outError = "Activate(IAudioSessionManager2) failed: " + FormatHResultHex(hr);
        return false;
    }

    ComPtr<IAudioSessionEnumerator> sessionEnum;
    hr = mgr->GetSessionEnumerator(sessionEnum.Put());
    if (FAILED(hr) || !sessionEnum) {
        outError = "GetSessionEnumerator failed: " + FormatHResultHex(hr);
        return false;
    }

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) {
        outError = "IAudioSessionEnumerator::GetCount failed";
        return false;
    }

    const float clampedVol = std::clamp(std::isfinite(masterVolume) ? masterVolume : 1.0f, 0.0f, 1.0f);

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(sessionEnum->GetSession(i, ctrl.Put())) || !ctrl) {
            continue;
        }
        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(ctrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                        reinterpret_cast<void**>(ctrl2.Put()))) || !ctrl2) {
            continue;
        }

        CoTaskMemPtr<wchar_t> siidPtr;
        if (FAILED(ctrl2->GetSessionInstanceIdentifier(siidPtr.Put())) || !siidPtr) {
            continue;
        }
        if (target.sessionInstanceId != siidPtr.Get()) {
            continue;
        }

        DWORD pid = 0;
        const HRESULT hrPid = ctrl2->GetProcessId(&pid);
        const bool multiProc = (hrPid == kAudclntSNoSingleProcess || hrPid == kAudclntENoSingleProcess);
        if (FAILED(hrPid) || pid == 0 || multiProc || (ctrl2->IsSystemSoundsSession() == S_OK)) {
            continue;
        }

        std::wstring liveExePath;
        if (!QueryProcessImageFullPath(pid, liveExePath)) {
            continue;
        }
        if (!EqualsIgnoreCase(liveExePath, target.exeFullPath)) {
            continue;
        }

        ComPtr<ISimpleAudioVolume> vol;
        hr = ctrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                  reinterpret_cast<void**>(vol.Put()));
        if (FAILED(hr) || !vol) {
            outError = "QueryInterface(ISimpleAudioVolume) failed: " + FormatHResultHex(hr);
            return false;
        }

        BOOL prevMute = FALSE;
        float prevVol = clampedVol;
        hr = vol->GetMute(&prevMute);
        if (FAILED(hr)) {
            outError = "ISimpleAudioVolume::GetMute failed: " + FormatHResultHex(hr);
            return false;
        }
        hr = vol->GetMasterVolume(&prevVol);
        if (FAILED(hr)) {
            outError = "ISimpleAudioVolume::GetMasterVolume failed: " + FormatHResultHex(hr);
            return false;
        }

        hr = vol->SetMute(mute ? TRUE : FALSE, nullptr);
        if (FAILED(hr)) {
            outError = "ISimpleAudioVolume::SetMute failed: " + FormatHResultHex(hr);
            return false;
        }
        const bool needVolumeWrite = !std::isfinite(prevVol) || (std::fabs(prevVol - clampedVol) > 1e-6f);
        if (needVolumeWrite) {
            hr = vol->SetMasterVolume(clampedVol, nullptr);
            if (FAILED(hr)) {
                vol->SetMute(prevMute, nullptr); // Best-effort rollback of partial write
                outError = "ISimpleAudioVolume::SetMasterVolume failed: " + FormatHResultHex(hr);
                return false;
            }
        }
        return true;
    }

    outError = "Target session not found for SetSessionMuteAndVolume";
    return false;
}

const char* RestoreStatusToString(RestoreStatus status) noexcept {
    switch (status) {
    case RestoreStatus::Restored: return "Restored";
    case RestoreStatus::SkippedSessionGone: return "SkippedSessionGone";
    case RestoreStatus::SkippedUserModified: return "SkippedUserModified";
    case RestoreStatus::FailedWrite: return "FailedWrite";
    default: return "Unknown";
    }
}

std::string SessionMuteJournal::SerializeJournalJson(const JournalDocument& doc) {
    std::string out;
    out.reserve(512 + doc.entries.size() * 384);
    out += "{\n";
    out += "  \"version\": " + std::to_string(doc.version) + ",\n";
    out += "  \"createdUtcIso\": \"" + EscapeJsonString(doc.createdUtcIso) + "\",\n";
    out += "  \"entries\": [\n";
    for (size_t i = 0; i < doc.entries.size(); ++i) {
        const auto& e = doc.entries[i];
        out += "    {\n";
        out += "      \"endpointId\": \"" + EscapeJsonString(WideToUtf8(e.endpointId)) + "\",\n";
        out += "      \"sessionInstanceId\": \"" + EscapeJsonString(WideToUtf8(e.sessionInstanceId)) + "\",\n";
        out += "      \"exeFullPath\": \"" + EscapeJsonString(WideToUtf8(e.exeFullPath)) + "\",\n";
        out += "      \"recordedPid\": " + std::to_string(e.recordedPid) + ",\n";
        out += std::string("      \"origMute\": ") + (e.origMute ? "true" : "false") + ",\n";
        out += "      \"origVolume\": " + FormatFloatJson(e.origVolume) + ",\n";
        out += std::string("      \"appliedMute\": ") + (e.appliedMute ? "true" : "false") + ",\n";
        out += "      \"appliedVolume\": " + FormatFloatJson(e.appliedVolume) + "\n";
        out += (i + 1 < doc.entries.size()) ? "    },\n" : "    }\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

bool SessionMuteJournal::DeserializeJournalJson(const std::string& jsonUtf8,
                                                JournalDocument& outDoc,
                                                std::string& outError) {
    outDoc = JournalDocument{};
    outError.clear();

    JsonValue root;
    JsonParser parser(jsonUtf8);
    if (!parser.ParseRoot(root, outError)) {
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        outError = "Journal root must be a JSON object";
        return false;
    }

    const JsonValue* verVal = root.Find("version");
    if (!verVal || verVal->type != JsonValue::Type::Number ||
        !std::isfinite(verVal->numVal) || verVal->numVal < 0.0 || verVal->numVal > 65535.0) {
        outError = "Missing or invalid 'version' field in journal";
        return false;
    }
    const long long parsedVer = std::llround(verVal->numVal);
    if (parsedVer != 1LL || std::fabs(verVal->numVal - 1.0) > 1e-9) {
        outError = "Unsupported journal version";
        return false;
    }
    outDoc.version = 1u;

    if (const JsonValue* tsVal = root.Find("createdUtcIso")) {
        if (tsVal->type != JsonValue::Type::String) {
            outError = "'createdUtcIso' must be a string";
            return false;
        }
        outDoc.createdUtcIso = tsVal->strVal;
    }

    const JsonValue* entriesVal = root.Find("entries");
    if (!entriesVal || entriesVal->type != JsonValue::Type::Array) {
        outError = "Missing or invalid 'entries' array in journal";
        return false;
    }

    for (size_t i = 0; i < entriesVal->arrVal.size(); ++i) {
        const JsonValue& item = entriesVal->arrVal[i];
        if (item.type != JsonValue::Type::Object) {
            outError = "Journal entry #" + std::to_string(i) + " is not an object";
            return false;
        }

        const auto readStr = [&](const char* k, std::wstring& dst) -> bool {
            const JsonValue* v = item.Find(k);
            if (!v || v->type != JsonValue::Type::String) {
                outError = std::string("Missing/invalid string field '") + k + "'";
                return false;
            }
            if (v->strVal.empty() || v->strVal.find('\0') != std::string::npos) {
                outError = std::string("Empty or NUL-containing string field '") + k + "'";
                return false;
            }
            dst = Utf8ToWide(v->strVal);
            if (dst.empty() || dst.find(L'\0') != std::wstring::npos) {
                outError = std::string("Invalid/empty wide string field '") + k + "'";
                return false;
            }
            return true;
        };
        const auto readBool = [&](const char* k, bool& dst) -> bool {
            const JsonValue* v = item.Find(k);
            if (!v || v->type != JsonValue::Type::Bool) {
                outError = std::string("Missing/invalid bool field '") + k + "'";
                return false;
            }
            dst = v->boolVal;
            return true;
        };
        const auto readFloat = [&](const char* k, float& dst) -> bool {
            const JsonValue* v = item.Find(k);
            if (!v || v->type != JsonValue::Type::Number ||
                !std::isfinite(v->numVal) || v->numVal < 0.0 || v->numVal > 1.0) {
                outError = std::string("Missing/invalid number field '") + k + "'";
                return false;
            }
            dst = static_cast<float>(v->numVal);
            return true;
        };

        JournalSessionEntry entry;
        if (!readStr("endpointId", entry.endpointId) ||
            !readStr("sessionInstanceId", entry.sessionInstanceId) ||
            !readStr("exeFullPath", entry.exeFullPath) ||
            !readBool("origMute", entry.origMute) ||
            !readFloat("origVolume", entry.origVolume) ||
            !readBool("appliedMute", entry.appliedMute) ||
            !readFloat("appliedVolume", entry.appliedVolume)) {
            return false;
        }

        if (const JsonValue* pidVal = item.Find("recordedPid")) {
            if (pidVal->type != JsonValue::Type::Number || !std::isfinite(pidVal->numVal) ||
                pidVal->numVal < 0.0 || pidVal->numVal > 4294967295.0) {
                outError = "Invalid 'recordedPid' field";
                return false;
            }
            entry.recordedPid = static_cast<uint32_t>(std::llround(pidVal->numVal));
        }

        outDoc.entries.push_back(std::move(entry));
    }

    return true;
}

bool SessionMuteJournal::BeginApplyMute(IAudioSessionAccess& access,
                                        const std::wstring& journalPath,
                                        const std::vector<SessionVolumeIdentity>& targets,
                                        bool appliedMute,
                                        const float* optionalOverrideVolume,
                                        JournalDocument& outDoc,
                                        std::string& outError) {
    outDoc = JournalDocument{};
    outError.clear();

    if (journalPath.empty()) {
        outError = "Journal path is empty";
        return false;
    }
    if (targets.empty()) {
        outError = "No target sessions specified";
        return false;
    }

    for (const auto& t : targets) {
        if (t.endpointId.empty() || t.sessionInstanceId.empty() || t.exeFullPath.empty()) {
            outError = "Refusing BeginApplyMute on session with incomplete identity";
            return false;
        }
        if (t.isSystemSoundsSession || t.isMultiProcessSession) {
            outError = "Refusing BeginApplyMute on multi-process or system sounds session";
            return false;
        }
    }

    std::vector<AudioSessionHandleInfo> liveSessions;
    if (!access.EnumerateSessions(liveSessions, outError)) {
        return false;
    }

    JournalDocument doc;
    doc.version = 1;
    doc.createdUtcIso = GetCurrentUtcIsoString();

    for (const auto& target : targets) {
        const AudioSessionHandleInfo* found = nullptr;
        for (const auto& live : liveSessions) {
            if (StrictSessionMatch(target, live.identity)) {
                found = &live;
                break;
            }
        }
        if (!found) {
            outError = "Target session not found during BeginApplyMute";
            return false;
        }
        if (found->identity.isMultiProcessSession || found->identity.isSystemSoundsSession) {
            outError = "Live target session is multi-process or system sounds; refusing mute";
            return false;
        }

        JournalSessionEntry entry;
        entry.endpointId = found->identity.endpointId;
        entry.sessionInstanceId = found->identity.sessionInstanceId;
        entry.exeFullPath = found->identity.exeFullPath;
        entry.recordedPid = found->identity.processId;
        entry.origMute = found->state.mute;
        entry.origVolume = std::clamp(
            std::isfinite(found->state.masterVolume) ? found->state.masterVolume : 1.0f,
            0.0f, 1.0f);
        entry.appliedMute = appliedMute;
        entry.appliedVolume = optionalOverrideVolume
            ? std::clamp(std::isfinite(*optionalOverrideVolume) ? *optionalOverrideVolume : 1.0f, 0.0f, 1.0f)
            : entry.origVolume;
        doc.entries.push_back(std::move(entry));
    }

    // Refuse to clobber an existing unrecovered journal file on disk.
    const DWORD existingAttr = GetFileAttributesW(journalPath.c_str());
    if (existingAttr != INVALID_FILE_ATTRIBUTES) {
        outError = "Unrecovered mute journal already exists at '" + WideToUtf8(journalPath) +
                   "'; run RestoreFromJournal (--restore-audio) first";
        return false;
    }
    const DWORD existingErr = GetLastError();
    if (existingErr != ERROR_FILE_NOT_FOUND && existingErr != ERROR_PATH_NOT_FOUND) {
        outError = "Failed inspecting existing journal path: err=" + std::to_string(existingErr);
        return false;
    }

    // Write journal atomically BEFORE muting any live session.
    const std::string json = SerializeJournalJson(doc);
    if (!WriteTextFileAtomic(journalPath, json, outError)) {
        return false;
    }

    // Now apply SetSessionMuteAndVolume for each recorded entry.
    for (size_t i = 0; i < doc.entries.size(); ++i) {
        const auto& e = doc.entries[i];
        SessionVolumeIdentity id;
        id.endpointId = e.endpointId;
        id.sessionInstanceId = e.sessionInstanceId;
        id.exeFullPath = e.exeFullPath;
        id.processId = e.recordedPid;

        std::string setErr;
        if (!access.SetSessionMuteAndVolume(id, e.appliedMute, e.appliedVolume, setErr)) {
            // Attempt safe rollback via RestoreFromJournal before returning failure.
            JournalRestoreSummary rollbackSummary;
            RestoreFromJournal(access, journalPath, rollbackSummary);
            outError = "SetSessionMuteAndVolume failed at entry #" + std::to_string(i) + ": " + setErr;
            return false;
        }
    }

    outDoc = std::move(doc);
    return true;
}

bool SessionMuteJournal::BeginApplyMute(IAudioSessionAccess& access,
                                        const std::wstring& journalPath,
                                        const SessionVolumeIdentity& target,
                                        bool appliedMute,
                                        float appliedVolume,
                                        JournalDocument& outDoc,
                                        std::string& outError) {
    const std::vector<SessionVolumeIdentity> targets{ target };
    return BeginApplyMute(access, journalPath, targets, appliedMute, &appliedVolume, outDoc, outError);
}

bool SessionMuteJournal::RestoreFromJournal(IAudioSessionAccess& access,
                                            const std::wstring& journalPath,
                                            JournalRestoreSummary& outSummary) {
    outSummary = JournalRestoreSummary{};

    if (journalPath.empty()) {
        outSummary.error = "Journal path is empty";
        return false;
    }

    const DWORD attr = GetFileAttributesW(journalPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            outSummary.journalExisted = false;
            return true; // Idempotent no-op
        }
        outSummary.error = "GetFileAttributesW failed: err=" + std::to_string(err);
        return false;
    }

    outSummary.journalExisted = true;

    std::string jsonUtf8;
    if (!ReadTextFileUtf8(journalPath, jsonUtf8, outSummary.error)) {
        return false;
    }

    JournalDocument doc;
    if (!DeserializeJournalJson(jsonUtf8, doc, outSummary.error)) {
        outSummary.journalParsed = false;
        return false;
    }
    outSummary.journalParsed = true;

    std::vector<AudioSessionHandleInfo> liveSessions;
    if (!access.EnumerateSessions(liveSessions, outSummary.error)) {
        return false;
    }

    bool anyWriteFailed = false;
    outSummary.items.reserve(doc.entries.size());

    for (const auto& entry : doc.entries) {
        JournalRestoreItemResult itemRes;
        itemRes.entry = entry;

        const AudioSessionHandleInfo* matched = nullptr;
        for (const auto& live : liveSessions) {
            if (StrictSessionMatchEntry(entry, live.identity)) {
                matched = &live;
                break;
            }
        }

        if (!matched) {
            itemRes.status = RestoreStatus::SkippedSessionGone;
            itemRes.detail = "No matching live session for endpointId + sessionInstanceId + exeFullPath";
            outSummary.items.push_back(std::move(itemRes));
            continue;
        }

        const bool muteChanged = (matched->state.mute != entry.appliedMute);
        const bool volNotFinite = !std::isfinite(matched->state.masterVolume);
        const float volDiff = volNotFinite
            ? 1.0f
            : std::fabs(matched->state.masterVolume - entry.appliedVolume);
        const bool volChanged = (volDiff > kVolumeCompareTolerance);

        if (muteChanged || volChanged) {
            itemRes.status = RestoreStatus::SkippedUserModified;
            itemRes.detail = "Live session state differs from applied journal state (mute=" +
                             std::string(matched->state.mute ? "true" : "false") +
                             ", vol=" + FormatFloatJson(matched->state.masterVolume) +
                             "); preserving user adjustment";
            outSummary.items.push_back(std::move(itemRes));
            continue;
        }

        std::string writeErr;
        if (!access.SetSessionMuteAndVolume(matched->identity, entry.origMute, entry.origVolume, writeErr)) {
            itemRes.status = RestoreStatus::FailedWrite;
            itemRes.detail = writeErr;
            anyWriteFailed = true;
            outSummary.items.push_back(std::move(itemRes));
            continue;
        }

        itemRes.status = RestoreStatus::Restored;
        itemRes.detail = "Restored original mute and volume";
        outSummary.items.push_back(std::move(itemRes));
    }

    if (!anyWriteFailed) {
        if (DeleteFileW(journalPath.c_str())) {
            outSummary.journalRemovedOrArchived = true;
        } else {
            const DWORD delErr = GetLastError();
            if (delErr == ERROR_FILE_NOT_FOUND) {
                outSummary.journalRemovedOrArchived = true;
            } else {
                outSummary.error = "Terminal restore succeeded, but DeleteFileW(journal) failed: err=" +
                                   std::to_string(delErr);
                return false;
            }
        }
        return true;
    }

    outSummary.error = "One or more entries failed during SetSessionMuteAndVolume; keeping journal";
    return false;
}

// ============================================================================
// Section C: Process Loopback Activation, Format Probe & Capture Engine
// ============================================================================

LoopbackActivationResult ActivateProcessLoopbackClient(DWORD targetPid,
                                                       PROCESS_LOOPBACK_MODE mode,
                                                       DWORD timeoutMs) {
    LoopbackActivationResult res{};
    if (targetPid == 0) {
        res.asyncCallHr = E_INVALIDARG;
        res.activateResultHr = E_INVALIDARG;
        return res;
    }

    ComMtaGuard com;
    if (!com.Usable()) {
        res.asyncCallHr = com.HResult();
        res.activateResultHr = com.HResult();
        return res;
    }

    auto* handler = new ProcessLoopbackCompletionHandler();
    if (!handler->EventHandle()) {
        handler->Release();
        res.asyncCallHr = HRESULT_FROM_WIN32(GetLastError());
        return res;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = targetPid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = mode;

    PROPVARIANT activateParams{};
    PropVariantInit(&activateParams);
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    // Note: Do NOT call PropVariantClear(&activateParams) because pBlobData points to stack memory.

    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
    res.asyncCallHr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        static_cast<IActivateAudioInterfaceCompletionHandler*>(handler),
        asyncOp.Put());

    if (FAILED(res.asyncCallHr)) {
        handler->Release();
        return res;
    }

    const DWORD waitRes = WaitForSingleObject(handler->EventHandle(), timeoutMs);
    if (waitRes != WAIT_OBJECT_0) {
        res.timedOut = true;
        res.activateResultHr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        handler->Release();
        return res;
    }

    res.activateResultHr = handler->ActivateResultHr();
    if (SUCCEEDED(res.activateResultHr)) {
        ComPtr<IUnknown> unk;
        unk.Attach(handler->DetachActivatedInterface());
        if (unk) {
            IAudioClient* client = nullptr;
            const HRESULT qiHr = unk->QueryInterface(__uuidof(IAudioClient),
                                                     reinterpret_cast<void**>(&client));
            if (SUCCEEDED(qiHr) && client) {
                res.audioClient = client;
            } else {
                res.activateResultHr = qiHr;
            }
        } else {
            res.activateResultHr = E_NOINTERFACE;
        }
    }

    handler->Release();
    return res;
}

bool ProbeProcessLoopbackFormats(DWORD targetPid,
                                 PROCESS_LOOPBACK_MODE mode,
                                 ProcessLoopbackProbeReport& outReport,
                                 std::string& outError) {
    outReport = ProcessLoopbackProbeReport{};
    outReport.targetPid = targetPid;
    outReport.mode = mode;
    outError.clear();

    if (targetPid == 0) {
        outError = "Target PID must be non-zero";
        return false;
    }

    // Step 1: Probe GetMixFormat on a dedicated fresh IAudioClient (do not assume E_NOTIMPL).
    std::vector<uint8_t> mixFormatBytes;
    ValidatedCaptureFormat mixValidated{};
    bool hasValidMixFormat = false;
    {
        LoopbackActivationResult mixAct = ActivateProcessLoopbackClient(targetPid, mode);
        ComPtr<IAudioClient> mixClient;
        mixClient.Attach(mixAct.audioClient);
        outReport.mixFormatActivateHr = SUCCEEDED(mixAct.asyncCallHr)
            ? mixAct.activateResultHr
            : mixAct.asyncCallHr;

        if (SUCCEEDED(outReport.mixFormatActivateHr) && mixClient) {
            CoTaskMemPtr<WAVEFORMATEX> mixWfx;
            outReport.getMixFormatHr = mixClient->GetMixFormat(mixWfx.Put());
            if (SUCCEEDED(outReport.getMixFormatHr) && mixWfx) {
                outReport.mixFormatDescription = FormatWaveFormatSummary(mixWfx.Get());
                std::string valErr;
                hasValidMixFormat = ValidateCaptureWaveFormat(mixWfx.Get(), mixValidated, valErr);
                outReport.mixFormatValidated = hasValidMixFormat;
                if (hasValidMixFormat) {
                    const size_t totalBytes = sizeof(WAVEFORMATEX) + mixWfx.Get()->cbSize;
                    mixFormatBytes.resize(totalBytes);
                    std::memcpy(mixFormatBytes.data(), mixWfx.Get(), totalBytes);
                } else {
                    outReport.mixFormatDescription += " [unsupported: " + valErr + "]";
                }
            } else {
                outReport.mixFormatDescription = "GetMixFormat returned " +
                                                 FormatHResultHex(outReport.getMixFormatHr);
            }
        } else {
            outReport.mixFormatDescription = "Activation failed: " +
                                             FormatHResultHex(outReport.mixFormatActivateHr);
        }
    }

    // Step 2: Build ordered candidate list (1..8 + optional 9 from GetMixFormat).
    struct CandidateSpec {
        uint32_t priorityIndex = 0;
        std::string label;
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        CaptureSampleType sampleType = CaptureSampleType::Unknown;
        bool useAutoConvert = false;
        bool isExtensibleMix = false;
        std::vector<uint8_t> rawWfx;
    };

    std::vector<CandidateSpec> specs;
    const auto addStdSpec = [&](uint32_t idx, const char* label, uint32_t sr,
                                uint16_t ch, CaptureSampleType st, bool autoConv) {
        CandidateSpec s;
        s.priorityIndex = idx;
        s.label = label;
        s.sampleRate = sr;
        s.channels = ch;
        s.sampleType = st;
        s.useAutoConvert = autoConv;
        s.rawWfx = BuildWaveFormatExBytes(sr, ch, st);
        specs.push_back(std::move(s));
    };

    addStdSpec(1, "48000Hz_2ch_Float32_Native", 48000u, 2u, CaptureSampleType::Float32, false);
    addStdSpec(2, "48000Hz_2ch_PCM16_Native", 48000u, 2u, CaptureSampleType::Pcm16, false);
    addStdSpec(3, "48000Hz_2ch_Float32_AutoConvert", 48000u, 2u, CaptureSampleType::Float32, true);
    addStdSpec(4, "48000Hz_2ch_PCM16_AutoConvert", 48000u, 2u, CaptureSampleType::Pcm16, true);
    addStdSpec(5, "44100Hz_2ch_Float32_Native", 44100u, 2u, CaptureSampleType::Float32, false);
    addStdSpec(6, "44100Hz_2ch_PCM16_Native", 44100u, 2u, CaptureSampleType::Pcm16, false);
    addStdSpec(7, "44100Hz_2ch_Float32_AutoConvert", 44100u, 2u, CaptureSampleType::Float32, true);
    addStdSpec(8, "44100Hz_2ch_PCM16_AutoConvert", 44100u, 2u, CaptureSampleType::Pcm16, true);

    if (hasValidMixFormat && mixFormatBytes.size() >= sizeof(WAVEFORMATEX)) {
        bool alreadyTestedWithoutAutoConvert = false;
        for (const auto& existing : specs) {
            if (existing.sampleRate == mixValidated.sampleRate &&
                existing.channels == mixValidated.channels &&
                existing.sampleType == mixValidated.sampleType &&
                !existing.useAutoConvert &&
                !mixValidated.isExtensible) {
                alreadyTestedWithoutAutoConvert = true;
                break;
            }
        }
        if (!alreadyTestedWithoutAutoConvert) {
            CandidateSpec sMix;
            sMix.priorityIndex = 9;
            sMix.label = "GetMixFormat_" + DescribeValidatedFormat(mixValidated);
            sMix.sampleRate = mixValidated.sampleRate;
            sMix.channels = mixValidated.channels;
            sMix.sampleType = mixValidated.sampleType;
            sMix.useAutoConvert = false;
            sMix.isExtensibleMix = mixValidated.isExtensible;
            sMix.rawWfx = mixFormatBytes;
            specs.push_back(std::move(sMix));
        }
    }

    // Step 3: Test EVERY candidate on its own fresh IAudioClient so prior Initialize failures
    // cannot poison subsequent candidates.
    for (const auto& spec : specs) {
        FormatCandidateResult cr;
        cr.priorityIndex = spec.priorityIndex;
        cr.label = spec.label;
        cr.sampleRate = spec.sampleRate;
        cr.channels = spec.channels;
        cr.sampleType = spec.sampleType;
        cr.useAutoConvert = spec.useAutoConvert;
        cr.isExtensibleMixFormat = spec.isExtensibleMix;
        cr.rawWaveFormatBytes = spec.rawWfx;

        const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(spec.rawWfx.data());
        std::string valErr;
        cr.formatValidated = ValidateCaptureWaveFormat(wfx, cr.validatedFormat, valErr);
        if (!cr.formatValidated) {
            cr.error = "Local format validation failed: " + valErr;
            outReport.candidates.push_back(std::move(cr));
            continue;
        }

        LoopbackActivationResult act = ActivateProcessLoopbackClient(targetPid, mode);
        ComPtr<IAudioClient> client;
        client.Attach(act.audioClient);
        cr.activateHr = SUCCEEDED(act.asyncCallHr) ? act.activateResultHr : act.asyncCallHr;

        if (FAILED(cr.activateHr) || !client) {
            cr.error = "ActivateProcessLoopbackClient failed: " + FormatHResultHex(cr.activateHr);
            outReport.candidates.push_back(std::move(cr));
            continue;
        }

        DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (spec.useAutoConvert) {
            streamFlags |= (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
        }

        cr.initializeHr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            kLoopbackBufferDuration100ns,
            0,
            wfx,
            nullptr);

        if (SUCCEEDED(cr.initializeHr)) {
            UINT32 bufFrames = 0;
            if (SUCCEEDED(client->GetBufferSize(&bufFrames))) {
                cr.bufferFrames = bufFrames;
            }
            cr.succeeded = true;
            if (!outReport.hasSelectedCandidate) {
                outReport.hasSelectedCandidate = true;
                outReport.selectedCandidateIndex = outReport.candidates.size();
            }
        } else {
            cr.error = "IAudioClient::Initialize failed: " + FormatHResultHex(cr.initializeHr);
        }

        outReport.candidates.push_back(std::move(cr));
    }

    if (!outReport.hasSelectedCandidate) {
        outError = "No valid 1ch/2ch process loopback format succeeded for PID " +
                   std::to_string(targetPid);
        return false;
    }
    return true;
}

bool RunCaptureSession(const CaptureSessionOptions& options,
                       CaptureSessionStats& outStats) {
    outStats = CaptureSessionStats{};
    outStats.requestedDurationSec = options.durationSec;

    if (options.targetPid == 0) {
        outStats.error = "Target PID must be non-zero";
        return false;
    }
    if (!std::isfinite(options.durationSec) ||
        (options.durationSec <= 0.0 && !options.externalStopEvent)) {
        outStats.error = "RunCaptureSession requires positive durationSec or externalStopEvent";
        return false;
    }
    if (options.waveFormatBytes.size() < sizeof(WAVEFORMATEX)) {
        outStats.error = "waveFormatBytes is smaller than WAVEFORMATEX";
        return false;
    }

    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(options.waveFormatBytes.data());
    if (options.waveFormatBytes.size() < sizeof(WAVEFORMATEX) + wfx->cbSize) {
        outStats.error = "waveFormatBytes is truncated relative to cbSize";
        return false;
    }

    if (!ValidateCaptureWaveFormat(wfx, outStats.validatedFormat, outStats.error)) {
        return false;
    }

    ComMtaGuard com;
    if (!com.Usable()) {
        outStats.error = "CoInitializeEx failed: " + FormatHResultHex(com.HResult());
        return false;
    }

    LoopbackActivationResult act = ActivateProcessLoopbackClient(options.targetPid, options.mode);
    ComPtr<IAudioClient> client;
    client.Attach(act.audioClient);
    outStats.activateHr = SUCCEEDED(act.asyncCallHr) ? act.activateResultHr : act.asyncCallHr;
    if (FAILED(outStats.activateHr) || !client) {
        outStats.error = "ActivateProcessLoopbackClient failed: " + FormatHResultHex(outStats.activateHr);
        return false;
    }

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (options.useAutoConvert) {
        streamFlags |= (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
    }

    outStats.initializeHr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        kLoopbackBufferDuration100ns,
        0,
        wfx,
        nullptr);
    if (FAILED(outStats.initializeHr)) {
        outStats.error = "IAudioClient::Initialize failed: " + FormatHResultHex(outStats.initializeHr);
        return false;
    }

    ScopedHandle sampleEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!sampleEvent.IsValid()) {
        outStats.error = "CreateEventW failed: err=" + std::to_string(GetLastError());
        return false;
    }

    HRESULT hr = client->SetEventHandle(sampleEvent.Get());
    if (FAILED(hr)) {
        outStats.error = "SetEventHandle failed: " + FormatHResultHex(hr);
        return false;
    }

    UINT32 bufferFrames = 0;
    if (SUCCEEDED(client->GetBufferSize(&bufferFrames))) {
        outStats.bufferFrames = bufferFrames;
    }

    ComPtr<IAudioCaptureClient> captureClient;
    hr = client->GetService(__uuidof(IAudioCaptureClient),
                            reinterpret_cast<void**>(captureClient.Put()));
    if (FAILED(hr) || !captureClient) {
        outStats.error = "GetService(IAudioCaptureClient) failed: " + FormatHResultHex(hr);
        return false;
    }

    WavStreamWriter wavWriter;
    const bool wantWav = !options.dumpWavPath.empty();
    if (wantWav) {
        if (!wavWriter.Open(options.dumpWavPath, outStats.validatedFormat.sampleRate, 2u, outStats.error)) {
            return false;
        }
    }

    DWORD mmcssTaskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    outStats.mmcssRegistered = (hMmcss != nullptr);

    hr = client->Start();
    if (FAILED(hr)) {
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        outStats.error = "IAudioClient::Start failed: " + FormatHResultHex(hr);
        return false;
    }
    outStats.initialized = true;

    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER qpcStart{};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);

    std::vector<int16_t> stereoScratch;
    double sumSquares = 0.0;
    int32_t peakAbs = 0;
    bool captureFailed = false;

    const double targetDuration = std::max(0.0, options.durationSec);

    while (true) {
        if (options.externalStopEvent &&
            WaitForSingleObject(options.externalStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        LARGE_INTEGER qpcNow{};
        QueryPerformanceCounter(&qpcNow);
        const double elapsedSec = static_cast<double>(qpcNow.QuadPart - qpcStart.QuadPart) /
                                  static_cast<double>(qpcFreq.QuadPart);
        if (targetDuration > 0.0 && elapsedSec >= targetDuration) {
            break;
        }

        DWORD waitTimeoutMs = 50u;
        if (targetDuration > 0.0) {
            const double remMs = (targetDuration - elapsedSec) * 1000.0;
            if (remMs < 1.0) {
                waitTimeoutMs = 1u;
            } else if (remMs < 50.0) {
                waitTimeoutMs = static_cast<DWORD>(remMs);
            }
        }

        if (options.externalStopEvent) {
            HANDLE waits[2] = { sampleEvent.Get(), options.externalStopEvent };
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, waitTimeoutMs);
            if (w == WAIT_OBJECT_0 + 1) {
                break;
            }
        } else {
            WaitForSingleObject(sampleEvent.Get(), waitTimeoutMs);
        }

        while (true) {
            UINT32 nextPacketFrames = 0;
            hr = captureClient->GetNextPacketSize(&nextPacketFrames);
            if (FAILED(hr)) {
                outStats.lastCaptureHr = hr;
                outStats.error = "GetNextPacketSize failed: " + FormatHResultHex(hr);
                captureFailed = true;
                break;
            }
            if (nextPacketFrames == 0u) {
                break;
            }

            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;
            hr = captureClient->GetBuffer(&pData, &numFrames, &flags, &devPos, &qpcPos);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            if (FAILED(hr)) {
                outStats.lastCaptureHr = hr;
                outStats.error = "IAudioCaptureClient::GetBuffer failed: " + FormatHResultHex(hr);
                captureFailed = true;
                break;
            }

            ++outStats.packetCount;
            outStats.totalFrames += numFrames;

            const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (isSilent) {
                ++outStats.silentPacketCount;
                outStats.silentFrames += numFrames;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                ++outStats.dataDiscontinuityCount;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
                ++outStats.timestampErrorCount;
            }

            if (numFrames > 0u) {
                if (!outStats.hasDevicePosition) {
                    outStats.hasDevicePosition = true;
                    outStats.firstDevicePosition = devPos;
                }
                outStats.lastDevicePosition = devPos;

                if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                    if (!outStats.hasQpcPosition) {
                        outStats.hasQpcPosition = true;
                        outStats.firstQpcPosition = qpcPos;
                    }
                    outStats.lastQpcPosition = qpcPos;
                }

                ConvertCaptureChunkToStereoInt16(
                    outStats.validatedFormat, pData, numFrames, isSilent, stereoScratch);

                for (uint32_t f = 0; f < numFrames; ++f) {
                    const int16_t l = stereoScratch[static_cast<size_t>(f) * 2u];
                    const int16_t r = stereoScratch[static_cast<size_t>(f) * 2u + 1u];
                    if (l != 0 || r != 0) {
                        ++outStats.nonZeroFrames;
                    }
                    const int32_t absL = std::abs(static_cast<int32_t>(l));
                    const int32_t absR = std::abs(static_cast<int32_t>(r));
                    peakAbs = std::max({ peakAbs, absL, absR });
                    const double dl = static_cast<double>(l);
                    const double dr = static_cast<double>(r);
                    sumSquares += dl * dl + dr * dr;
                }

                if (wantWav) {
                    std::string wavErr;
                    if (!wavWriter.AppendPcm16(stereoScratch, wavErr)) {
                        outStats.error = wavErr;
                        captureFailed = true;
                    }
                }
            }

            const HRESULT relHr = captureClient->ReleaseBuffer(numFrames);
            if (FAILED(relHr)) {
                outStats.lastCaptureHr = relHr;
                outStats.error = "ReleaseBuffer failed: " + FormatHResultHex(relHr);
                captureFailed = true;
                break;
            }
            if (captureFailed) {
                break;
            }
        }

        if (captureFailed) {
            break;
        }
    }

    client->Stop();
    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }

    LARGE_INTEGER qpcEnd{};
    QueryPerformanceCounter(&qpcEnd);
    outStats.elapsedSec = static_cast<double>(qpcEnd.QuadPart - qpcStart.QuadPart) /
                          static_cast<double>(qpcFreq.QuadPart);

    outStats.peakAbsInt16 = peakAbs;
    if (peakAbs > 0) {
        const double peakNorm = static_cast<double>(peakAbs) / 32768.0;
        outStats.peakDbfs = std::max(-144.0, 20.0 * std::log10(peakNorm));
    } else {
        outStats.peakDbfs = -144.0;
    }

    if (outStats.totalFrames > 0 && sumSquares > 0.0) {
        const double totalScalars = static_cast<double>(outStats.totalFrames) * 2.0;
        const double rmsNorm = std::sqrt(sumSquares / totalScalars) / 32768.0;
        outStats.rmsDbfs = (rmsNorm > 0.0) ? std::max(-144.0, 20.0 * std::log10(rmsNorm)) : -144.0;
    } else {
        outStats.rmsDbfs = -144.0;
    }

    if (wantWav) {
        std::string finErr;
        if (wavWriter.Finalize(finErr)) {
            outStats.wavWritten = true;
            outStats.wavPcmBytes = wavWriter.DataBytes();
        } else if (!captureFailed) {
            outStats.error = finErr;
            captureFailed = true;
        }
    }

    outStats.completed = !captureFailed;
    return !captureFailed;
}

} // namespace staged
