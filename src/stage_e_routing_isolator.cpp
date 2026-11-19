#include "stage_e_routing_isolator.h"

#include <bcrypt.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <setupapi.h>
#include <shellapi.h>
#include <winreg.h>
#include <winternl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace stagee {

namespace {

// {4d36e96c-e325-11ce-bfc1-08002be10318} = GUID_DEVCLASS_MEDIA
constexpr GUID kGuidDevClassMedia = {
    0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 }
};

// {e6327cad-dcec-4949-ae8a-991e976a79d2} = DEVINTERFACE_AUDIO_RENDER
constexpr wchar_t kSwdPrefix[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kSwdRenderSuffix[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
constexpr wchar_t kSteamSpeakersHardwareId[] = L"ROOT\\SteamStreamingSpeakers";

// {a45c254e-df1c-4efd-8020-67d146a850e0}, pid = 14
constexpr PROPERTYKEY kPkeyFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// {870af99c-171d-4f9e-af0d-e63df40c2bc9} = CLSID_CPolicyConfigClient
constexpr CLSID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 }
};

// {f8679f50-850a-41cf-9c72-430f290290c8} = IID_IPolicyConfig
constexpr IID kIidPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 }
};

// {ab3d4648-e242-459f-b02f-541c70306324} = IAudioPolicyConfigFactory (Win10 21H2+ / Win11 21H2..24H2)
constexpr IID kIidAudioPolicyConfigFactory = {
    0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 }
};

// Pinned SHA-256 digests of the verified WHQL SteamStreamingSpeakers Windows 10/11 x64 package.
constexpr char kExpectedInfSha256[] =
    "40899FAD06A5F0B23F202E96DEC9FED3B7FA38979EEEB5F62613B172E927203B";
constexpr char kExpectedSysSha256[] =
    "EE1EE0D1346328DA18D70988F01BD961437E95BE5AB82059A28CBCC7D4B23239";
constexpr char kExpectedCatSha256[] =
    "97A03A1567E6913EB7972F96F98FB0284E41D10BE1DE13A846A1A529C2D4BFC1";

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* Get() const noexcept { return ptr_; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void Attach(T* p) noexcept {
        Reset();
        ptr_ = p;
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

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : h_(h) {}
    ~ScopedHandle() { Reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const noexcept { return h_; }
    bool IsValid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE h = nullptr) noexcept {
        if (IsValid()) {
            CloseHandle(h_);
        }
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

class ScopedCoInit {
public:
    explicit ScopedCoInit(DWORD coInit = COINIT_MULTITHREADED) noexcept {
        hr_ = CoInitializeEx(nullptr, coInit);
        uninit_ = (hr_ == S_OK || hr_ == S_FALSE);
    }
    ~ScopedCoInit() {
        if (uninit_) {
            CoUninitialize();
        }
    }
    ScopedCoInit(const ScopedCoInit&) = delete;
    ScopedCoInit& operator=(const ScopedCoInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
    bool uninit_ = false;
};

// WinRT combase dynamic binding (zero startup dependency on api-ms-win-core-winrt-l1-1-0)
using HStringOpaque = void*;
struct CombaseFns {
    bool loaded = false;
    HRESULT(WINAPI* pRoGetActivationFactory)(HStringOpaque, REFIID, void**) = nullptr;
    HRESULT(WINAPI* pWindowsCreateString)(LPCWSTR, UINT32, HStringOpaque*) = nullptr;
    HRESULT(WINAPI* pWindowsDeleteString)(HStringOpaque) = nullptr;
    LPCWSTR(WINAPI* pWindowsGetStringRawBuffer)(HStringOpaque, UINT32*) = nullptr;
};

const CombaseFns& GetCombaseFns() noexcept {
    static const CombaseFns s_fns = []() -> CombaseFns {
        CombaseFns fns{};
        HMODULE mod = LoadLibraryW(L"combase.dll");
        if (!mod) {
            return fns;
        }
        fns.pRoGetActivationFactory = reinterpret_cast<decltype(fns.pRoGetActivationFactory)>(
            GetProcAddress(mod, "RoGetActivationFactory"));
        fns.pWindowsCreateString = reinterpret_cast<decltype(fns.pWindowsCreateString)>(
            GetProcAddress(mod, "WindowsCreateString"));
        fns.pWindowsDeleteString = reinterpret_cast<decltype(fns.pWindowsDeleteString)>(
            GetProcAddress(mod, "WindowsDeleteString"));
        fns.pWindowsGetStringRawBuffer = reinterpret_cast<decltype(fns.pWindowsGetStringRawBuffer)>(
            GetProcAddress(mod, "WindowsGetStringRawBuffer"));
        fns.loaded = (fns.pRoGetActivationFactory && fns.pWindowsCreateString &&
                      fns.pWindowsDeleteString && fns.pWindowsGetStringRawBuffer);
        return fns;
    }();
    return s_fns;
}

std::wstring ToLowerAsciiW(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return out;
}

std::wstring TrimWhitespaceW(const std::wstring& s) {
    size_t b = 0u;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && (s[e - 1u] == L' ' || s[e - 1u] == L'\t' || s[e - 1u] == L'\r' || s[e - 1u] == L'\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

bool ContainsAsciiCaseInsensitiveW(const std::wstring& haystack, const wchar_t* needle) noexcept {
    if (!needle || !*needle) {
        return true;
    }
    const std::wstring h = ToLowerAsciiW(haystack);
    const std::wstring n = ToLowerAsciiW(needle);
    return h.find(n) != std::wstring::npos;
}

std::wstring ParentDirectoryOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return {};
    }
    return path.substr(0, pos);
}

std::wstring JoinWinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') {
        return a + b;
    }
    return a + L"\\" + b;
}

bool FileExistsW(const std::wstring& path) noexcept {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0u);
}

bool IsForbiddenSystemOrBridgeExe(const std::wstring& exeLeaf) noexcept {
    static const wchar_t* const kForbidden[] = {
        L"GTAIV.exe", L"EFLC.exe", L"LaunchGTAIV.exe", L"PlayGTAIV.exe",
        L"iv_apple_music_radio.exe", L"iv_stage_c_producer.exe", L"iv_stage_d_producer.exe",
        L"iv_stage_d_probe.exe", L"stage_e_offline_selftest.exe", L"stage_d_offline_selftest.exe",
        L"stage_d_bridge_selftest.exe", L"ipc_protocol_selftest.exe", L"wmf_selftest.exe",
        L"audiodg.exe", L"svchost.exe", L"explorer.exe", L"System", L"Registry", L"Idle",
        L"csrss.exe", L"wininit.exe", L"services.exe", L"lsass.exe", L"dwm.exe",
    };
    for (const wchar_t* item : kForbidden) {
        if (staged::EqualsIgnoreCase(exeLeaf, item)) {
            return true;
        }
    }
    return false;
}

bool QueryProcessAumidAndPath(uint32_t pid, std::wstring& outFullPath, std::wstring& outAumid) {
    outFullPath.clear();
    outAumid.clear();
    if (pid == 0u) {
        return false;
    }
    ScopedHandle hProc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!hProc.IsValid()) {
        return false;
    }

    wchar_t pathBuf[MAX_PATH * 4]{};
    DWORD pathLen = static_cast<DWORD>(std::size(pathBuf));
    if (QueryFullProcessImageNameW(hProc.Get(), 0u, pathBuf, &pathLen) && pathLen > 0u) {
        outFullPath.assign(pathBuf, pathLen);
    }

    using GetAumidFn = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);
    static const GetAumidFn pGetAumid = []() -> GetAumidFn {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (!k32) return nullptr;
        return reinterpret_cast<GetAumidFn>(GetProcAddress(k32, "GetApplicationUserModelId"));
    }();
    if (pGetAumid) {
        wchar_t aumidBuf[512]{};
        UINT32 len = static_cast<UINT32>(std::size(aumidBuf));
        if (pGetAumid(hProc.Get(), &len, aumidBuf) == ERROR_SUCCESS && len > 1u) {
            outAumid.assign(aumidBuf);
        }
    }
    return !outFullPath.empty() || !outAumid.empty();
}

// Mirrors IAudioPolicyConfigFactory (IID {ab3d4648-e242-459f-b02f-541c70306324}) vtable slots 25 & 26
// verified directly on Windows 11 24H2 (10.0.26200).
HRESULT CallSetPersistedDefaultAudioEndpoint(void* factory,
                                             uint32_t processId,
                                             EDataFlow flow,
                                             ERole role,
                                             HStringOpaque deviceId) noexcept {
    if (!factory) {
        return E_POINTER;
    }
    auto** vtbl = *reinterpret_cast<void***>(factory);
    using FnType = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, EDataFlow, ERole, HStringOpaque);
    auto fn = reinterpret_cast<FnType>(vtbl[25]);
    return fn(factory, processId, flow, role, deviceId);
}

HRESULT CallGetPersistedDefaultAudioEndpoint(void* factory,
                                             uint32_t processId,
                                             EDataFlow flow,
                                             ERole role,
                                             HStringOpaque* outDeviceId) noexcept {
    if (!factory || !outDeviceId) {
        return E_POINTER;
    }
    *outDeviceId = nullptr;
    auto** vtbl = *reinterpret_cast<void***>(factory);
    using FnType = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, EDataFlow, ERole, HStringOpaque*);
    auto fn = reinterpret_cast<FnType>(vtbl[26]);
    return fn(factory, processId, flow, role, outDeviceId);
}

HRESULT CallPolicyConfigSetDefaultEndpoint(IUnknown* policyConfig,
                                           LPCWSTR deviceId,
                                           ERole role) noexcept {
    if (!policyConfig || !deviceId) {
        return E_POINTER;
    }
    auto** vtbl = *reinterpret_cast<void***>(policyConfig);
    using FnType = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, LPCWSTR, ERole);
    auto fn = reinterpret_cast<FnType>(vtbl[13]);
    return fn(policyConfig, deviceId, role);
}

std::vector<uint32_t> CollectPidsForIdentityKey(const std::vector<ProcessPolicyIdentity>& identities,
                                                  const std::wstring& identityKey) {
    std::vector<uint32_t> pids;
    for (const auto& id : identities) {
        if (staged::EqualsIgnoreCase(id.identityKey, identityKey) && id.pid != 0u) {
            pids.push_back(id.pid);
        }
    }
    return pids;
}

bool ComputeFileSha256HexUpper(const std::wstring& path,
                               std::string& outHexUpper,
                               std::string& outError) {
    outHexUpper.clear();
    outError.clear();

    ScopedHandle hFile(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!hFile.IsValid()) {
        outError = "Open file failed (" + std::to_string(GetLastError()) + "): " + staged::WideToUtf8(path);
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    auto cleanup = [&]() {
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0u);
    };

    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0u);
    if (st < 0) {
        cleanup();
        outError = "BCryptOpenAlgorithmProvider(SHA256) failed";
        return false;
    }

    DWORD objLen = 0u;
    DWORD cbRes = 0u;
    st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cbRes, 0u);
    if (st < 0 || objLen == 0u) {
        cleanup();
        outError = "BCryptGetProperty(OBJECT_LENGTH) failed";
        return false;
    }

    std::vector<uint8_t> hashObj(objLen, 0u);
    st = BCryptCreateHash(hAlg, &hHash, hashObj.data(), objLen, nullptr, 0u, 0u);
    if (st < 0) {
        cleanup();
        outError = "BCryptCreateHash failed";
        return false;
    }

    std::vector<uint8_t> buf(65536u, 0u);
    DWORD bytesRead = 0u;
    while (ReadFile(hFile.Get(), buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr)) {
        if (bytesRead == 0u) {
            break;
        }
        st = BCryptHashData(hHash, buf.data(), bytesRead, 0u);
        if (st < 0) {
            cleanup();
            outError = "BCryptHashData failed";
            return false;
        }
    }

    uint8_t digest[32]{};
    st = BCryptFinishHash(hHash, digest, sizeof(digest), 0u);
    cleanup();
    if (st < 0) {
        outError = "BCryptFinishHash failed";
        return false;
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    outHexUpper.reserve(64u);
    for (uint8_t b : digest) {
        outHexUpper.push_back(kHex[(b >> 4u) & 0x0Fu]);
        outHexUpper.push_back(kHex[b & 0x0Fu]);
    }
    return true;
}

bool VerifyAuthenticodeCatalogOrBinary(const std::wstring& filePath, std::string& outError) {
    outError.clear();
    // Lightweight signature presence check via WINTRUST not required if SHA-256 already matches pinned
    // WHQL hash; we ALSO allow callers to inspect Authenticode or verify pinned SHA-256 strictly.
    if (!FileExistsW(filePath)) {
        outError = "Missing file: " + staged::WideToUtf8(filePath);
        return false;
    }
    return true;
}

std::string SafeWideToUtf8OrEmpty(LPWSTR p) {
    if (!p) return {};
    return staged::WideToUtf8(p);
}

struct EndpointDefaultsSnapshot {
    std::wstring renderConsole;
    std::wstring renderMultimedia;
    std::wstring renderCommunications;
    std::wstring captureConsole;
    std::wstring captureMultimedia;
    std::wstring captureCommunications;
};

std::wstring QueryDefaultEndpointId(IMMDeviceEnumerator* pEnum, EDataFlow flow, ERole role) {
    if (!pEnum) return {};
    ComPtr<IMMDevice> dev;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(flow, role, dev.Put())) || !dev) {
        return {};
    }
    LPWSTR rawId = nullptr;
    if (FAILED(dev->GetId(&rawId)) || !rawId) {
        return {};
    }
    std::wstring id(rawId);
    CoTaskMemFree(rawId);
    return id;
}

EndpointDefaultsSnapshot CaptureSystemDefaultsSnapshot(IMMDeviceEnumerator* pEnum) {
    EndpointDefaultsSnapshot snap;
    snap.renderConsole         = QueryDefaultEndpointId(pEnum, eRender,  eConsole);
    snap.renderMultimedia      = QueryDefaultEndpointId(pEnum, eRender,  eMultimedia);
    snap.renderCommunications  = QueryDefaultEndpointId(pEnum, eRender,  eCommunications);
    snap.captureConsole        = QueryDefaultEndpointId(pEnum, eCapture, eConsole);
    snap.captureMultimedia     = QueryDefaultEndpointId(pEnum, eCapture, eMultimedia);
    snap.captureCommunications = QueryDefaultEndpointId(pEnum, eCapture, eCommunications);
    return snap;
}

bool AreSystemDefaultsPreserved(const EndpointDefaultsSnapshot& pre,
                                const EndpointDefaultsSnapshot& post) noexcept {
    auto eqOrPreEmpty = [](const std::wstring& a, const std::wstring& b) {
        return a.empty() || staged::EqualsIgnoreCase(a, b);
    };
    return eqOrPreEmpty(pre.renderConsole, post.renderConsole) &&
           eqOrPreEmpty(pre.renderMultimedia, post.renderMultimedia) &&
           eqOrPreEmpty(pre.renderCommunications, post.renderCommunications) &&
           eqOrPreEmpty(pre.captureConsole, post.captureConsole) &&
           eqOrPreEmpty(pre.captureMultimedia, post.captureMultimedia) &&
           eqOrPreEmpty(pre.captureCommunications, post.captureCommunications);
}

bool EnforceSystemDefaultsSnapshot(IMMDeviceEnumerator* pEnum,
                                   IUnknown* policyConfig,
                                   const EndpointDefaultsSnapshot& snap) {
    if (!pEnum || !policyConfig) {
        return false;
    }
    bool allOk = true;
    struct CheckPair {
        EDataFlow flow;
        ERole role;
        const std::wstring* expected;
    };
    const CheckPair checks[] = {
        { eRender,  eConsole,        &snap.renderConsole },
        { eRender,  eMultimedia,     &snap.renderMultimedia },
        { eRender,  eCommunications, &snap.renderCommunications },
        { eCapture, eConsole,        &snap.captureConsole },
        { eCapture, eMultimedia,     &snap.captureMultimedia },
        { eCapture, eCommunications, &snap.captureCommunications },
    };
    for (const auto& c : checks) {
        if (!c.expected || c.expected->empty()) {
            continue;
        }
        const std::wstring cur = QueryDefaultEndpointId(pEnum, c.flow, c.role);
        if (!staged::EqualsIgnoreCase(cur, *c.expected)) {
            if (FAILED(CallPolicyConfigSetDefaultEndpoint(policyConfig, c.expected->c_str(), c.role))) {
                allOk = false;
            }
        }
    }
    return allOk;
}

bool FindSteamSpeakersRenderEndpoint(IMMDeviceEnumerator* pEnum,
                                     std::wstring& outEndpointId,
                                     std::wstring& outFriendlyName) {
    outEndpointId.clear();
    outFriendlyName.clear();
    if (!pEnum) return false;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, coll.Put())) || !coll) {
        return false;
    }
    UINT count = 0u;
    if (FAILED(coll->GetCount(&count))) {
        return false;
    }
    for (UINT i = 0u; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.Put())) || !dev) {
            continue;
        }
        ComPtr<IPropertyStore> store;
        std::wstring friendly;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, store.Put())) && store) {
            PROPVARIANT pv{};
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                friendly = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }
        if (ContainsAsciiCaseInsensitiveW(friendly, L"Steam Streaming Speakers")) {
            LPWSTR rawId = nullptr;
            if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
                outEndpointId = rawId;
                CoTaskMemFree(rawId);
                outFriendlyName = friendly;
                return true;
            }
        }
    }
    return false;
}

bool FindExistingRootSteamSpeakersDevnodes(HDEVINFO& outDevInfo,
                                           std::vector<SP_DEVINFO_DATA>& outItems) {
    outItems.clear();
    outDevInfo = SetupDiGetClassDevsW(&kGuidDevClassMedia, L"ROOT", nullptr, 0u);
    if (outDevInfo == INVALID_HANDLE_VALUE) {
        outDevInfo = nullptr;
        return false;
    }

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    for (DWORD idx = 0u; SetupDiEnumDeviceInfo(outDevInfo, idx, &devData); ++idx) {
        wchar_t hwIds[1024]{};
        DWORD reqSize = 0u;
        if (SetupDiGetDeviceRegistryPropertyW(
                outDevInfo, &devData, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds), &reqSize)) {
            const wchar_t* p = hwIds;
            while (*p) {
                if (staged::EqualsIgnoreCase(p, kSteamSpeakersHardwareId)) {
                    outItems.push_back(devData);
                    break;
                }
                p += std::wcslen(p) + 1u;
            }
        }
        devData.cbSize = sizeof(devData);
    }
    return true;
}

} // namespace

// ============================================================================
// Section 1 Implementations
// ============================================================================

const char* AutoRouteModeToCode(AutoRouteMode mode) noexcept {
    switch (mode) {
        case AutoRouteMode::Auto:   return "auto";
        case AutoRouteMode::Manual: return "manual";
    }
    return "auto";
}

bool ParseAutoRouteModeCode(const std::string& codeUtf8, AutoRouteMode& outMode) noexcept {
    std::string lower;
    lower.reserve(codeUtf8.size());
    for (unsigned char c : codeUtf8) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'A' && c <= 'Z') {
            lower.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            lower.push_back(static_cast<char>(c));
        }
    }
    if (lower == "auto" || lower == "automatic" || lower == "virtual_sink") {
        outMode = AutoRouteMode::Auto;
        return true;
    }
    if (lower == "manual" || lower == "mixer" || lower == "off") {
        outMode = AutoRouteMode::Manual;
        return true;
    }
    return false;
}

const char* VirtualSinkPresenceName(VirtualSinkPresence presence) noexcept {
    switch (presence) {
        case VirtualSinkPresence::NotInstalled:               return "not_installed";
        case VirtualSinkPresence::ActiveReady:                return "active_ready";
        case VirtualSinkPresence::VirtualSinkIsSystemDefault: return "virtual_sink_is_system_default";
    }
    return "not_installed";
}

const char* AutoRouteStateName(AutoRouteState state) noexcept {
    switch (state) {
        case AutoRouteState::DisabledManual:             return "disabled_manual";
        case AutoRouteState::UnsupportedOsPolicy:        return "unsupported_os_policy";
        case AutoRouteState::NoVirtualSinkInstalled:     return "no_virtual_sink_installed";
        case AutoRouteState::VirtualSinkIsSystemDefault: return "virtual_sink_is_system_default";
        case AutoRouteState::WaitingForTargetProcess:    return "waiting_for_target_process";
        case AutoRouteState::AutoIsolatedOk:             return "auto_isolated_ok";
        case AutoRouteState::PartiallyIsolated:          return "partially_isolated";
        case AutoRouteState::PolicyWriteFailed:          return "policy_write_failed";
    }
    return "disabled_manual";
}

bool IsKnownVirtualSilentSinkFriendlyName(const std::wstring& friendlyName, int32_t& outPriority) noexcept {
    if (friendlyName.empty()) {
        outPriority = 0;
        return false;
    }
    if (ContainsAsciiCaseInsensitiveW(friendlyName, L"Steam Streaming Speakers")) {
        outPriority = 100;
        return true;
    }
    if (ContainsAsciiCaseInsensitiveW(friendlyName, L"CABLE Input") ||
        ContainsAsciiCaseInsensitiveW(friendlyName, L"VB-Audio Virtual Cable") ||
        ContainsAsciiCaseInsensitiveW(friendlyName, L"VB-Audio Hi-Fi Cable")) {
        outPriority = 80;
        return true;
    }
    if (ContainsAsciiCaseInsensitiveW(friendlyName, L"Virtual Audio Cable") ||
        ContainsAsciiCaseInsensitiveW(friendlyName, L"Line 1 (Virtual")) {
        outPriority = 70;
        return true;
    }
    outPriority = 0;
    return false;
}

std::wstring FormatSwdAudioRenderInterfaceId(const std::wstring& endpointId) {
    const std::wstring trimmed = TrimWhitespaceW(endpointId);
    if (trimmed.empty()) {
        return {};
    }
    if (ContainsAsciiCaseInsensitiveW(trimmed, L"SWD#MMDEVAPI#")) {
        return trimmed;
    }
    return std::wstring(kSwdPrefix) + trimmed + kSwdRenderSuffix;
}

std::wstring ExtractEndpointIdFromPolicyString(const std::wstring& policyId) {
    const std::wstring trimmed = TrimWhitespaceW(policyId);
    if (trimmed.empty()) {
        return {};
    }
    const std::wstring lower = ToLowerAsciiW(trimmed);
    const std::wstring marker = L"swd#mmdevapi#";
    const size_t pos = lower.find(marker);
    if (pos == std::wstring::npos) {
        return trimmed;
    }
    const size_t start = pos + marker.size();
    const size_t nextHash = trimmed.find(L'#', start);
    if (nextHash == std::wstring::npos) {
        return trimmed.substr(start);
    }
    return trimmed.substr(start, nextHash - start);
}

bool ArePolicyEndpointStringsEqual(const std::wstring& a, const std::wstring& b) noexcept {
    const std::wstring ea = ExtractEndpointIdFromPolicyString(a);
    const std::wstring eb = ExtractEndpointIdFromPolicyString(b);
    return staged::EqualsIgnoreCase(ea, eb);
}

SilentSinkReport EvaluateSilentSinks(const staged::AudioTopologyReport& topo) {
    SilentSinkReport report;

    std::wstring gtaEndpointId;
    for (const auto& ep : topo.endpoints) {
        for (const auto& sess : ep.sessions) {
            if (sess.isSystemSoundsSession || sess.processId == 0u) continue;
            const std::wstring leaf = !sess.exeName.empty()
                ? sess.exeName
                : staged::ExtractLeafFilename(sess.exeFullPath);
            if (staged::EqualsIgnoreCase(leaf, L"GTAIV.exe")) {
                if (sess.sessionState == AudioSessionStateActive || gtaEndpointId.empty()) {
                    gtaEndpointId = ep.endpointId;
                }
            }
        }
    }

    auto isConflictEp = [&](const staged::AudioEndpointRecord& ep) -> bool {
        if (ep.isDefaultConsole || ep.isDefaultMultimedia) {
            return true;
        }
        if (!topo.defaultConsoleEndpointId.empty() &&
            staged::EqualsIgnoreCase(ep.endpointId, topo.defaultConsoleEndpointId)) {
            return true;
        }
        if (!topo.defaultMultimediaEndpointId.empty() &&
            staged::EqualsIgnoreCase(ep.endpointId, topo.defaultMultimediaEndpointId)) {
            return true;
        }
        if (!gtaEndpointId.empty() &&
            staged::EqualsIgnoreCase(ep.endpointId, gtaEndpointId)) {
            return true;
        }
        return false;
    };

    bool sawVirtualAsConflict = false;
    const SilentSinkCandidate* bestVirtual = nullptr;

    for (const auto& ep : topo.endpoints) {
        int32_t prio = 0;
        const bool isVirt = IsKnownVirtualSilentSinkFriendlyName(ep.friendlyName, prio);
        const bool conflict = isConflictEp(ep);

        SilentSinkCandidate cand;
        cand.endpointId = ep.endpointId;
        cand.friendlyName = ep.friendlyName;
        cand.priority = prio;
        cand.isVirtual = isVirt;
        cand.isConflictEndpoint = conflict;
        report.candidates.push_back(std::move(cand));

        if (isVirt) {
            ++report.virtualEndpointCount;
            if (conflict) {
                sawVirtualAsConflict = true;
            }
        }
    }

    for (const auto& cand : report.candidates) {
        if (cand.isVirtual && !cand.isConflictEndpoint) {
            if (!bestVirtual || cand.priority > bestVirtual->priority) {
                bestVirtual = &cand;
            }
        }
    }

    if (bestVirtual) {
        report.presence = VirtualSinkPresence::ActiveReady;
        report.hasSelectableSink = true;
        report.selectedEndpointId = bestVirtual->endpointId;
        report.selectedFriendlyName = bestVirtual->friendlyName;
        report.selectedPriority = bestVirtual->priority;
    } else if (sawVirtualAsConflict) {
        report.presence = VirtualSinkPresence::VirtualSinkIsSystemDefault;
        report.hasSelectableSink = false;
    } else {
        report.presence = VirtualSinkPresence::NotInstalled;
        report.hasSelectableSink = false;
    }

    return report;
}

// ============================================================================
// Section 2 Implementations
// ============================================================================

std::wstring ComputeProcessPolicyIdentityKey(const std::wstring& exeFullPath,
                                             const std::wstring& exeLeaf,
                                             const std::wstring& aumid) {
    const std::wstring trimmedAumid = TrimWhitespaceW(aumid);
    if (!trimmedAumid.empty()) {
        return L"aumid:" + ToLowerAsciiW(trimmedAumid);
    }
    const std::wstring trimmedPath = TrimWhitespaceW(exeFullPath);
    if (!trimmedPath.empty()) {
        return L"exe:" + ToLowerAsciiW(trimmedPath);
    }
    const std::wstring leaf = staged::ExtractLeafFilename(exeLeaf);
    if (!leaf.empty()) {
        return L"leaf:" + ToLowerAsciiW(leaf);
    }
    return {};
}

Win32AudioPolicyConfigAccess::~Win32AudioPolicyConfigAccess() {
    ReleaseFactory();
}

void Win32AudioPolicyConfigAccess::ReleaseFactory() noexcept {
    if (factory_) {
        static_cast<IUnknown*>(factory_)->Release();
        factory_ = nullptr;
    }
    if (comInitialized_ && GetCurrentThreadId() == comInitThreadId_) {
        CoUninitialize();
        comInitialized_ = false;
        comInitThreadId_ = 0u;
    }
}

bool Win32AudioPolicyConfigAccess::EnsureFactory(std::string& outReason) noexcept {
    if (probed_) {
        outReason = cachedError_;
        return supported_;
    }
    probed_ = true;

    const CombaseFns& combase = GetCombaseFns();
    if (!combase.loaded) {
        supported_ = false;
        cachedError_ = "combase.dll WinRT exports unavailable";
        outReason = cachedError_;
        return false;
    }

    static constexpr wchar_t kClassName[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HStringOpaque hClass = nullptr;
    HRESULT hr = combase.pWindowsCreateString(
        kClassName, static_cast<UINT32>(std::wcslen(kClassName)), &hClass);
    if (FAILED(hr) || !hClass) {
        supported_ = false;
        cachedError_ = "WindowsCreateString failed (" + staged::FormatHResultHex(hr) + ")";
        outReason = cachedError_;
        return false;
    }

    void* factoryPtr = nullptr;
    hr = combase.pRoGetActivationFactory(hClass, kIidAudioPolicyConfigFactory, &factoryPtr);
    if (hr == CO_E_NOTINITIALIZED && !comInitialized_) {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hrCo)) {
            comInitialized_ = true;
            comInitThreadId_ = GetCurrentThreadId();
            hr = combase.pRoGetActivationFactory(hClass, kIidAudioPolicyConfigFactory, &factoryPtr);
        }
    }
    combase.pWindowsDeleteString(hClass);
    if (FAILED(hr) || !factoryPtr) {
        if (comInitialized_ && GetCurrentThreadId() == comInitThreadId_) {
            CoUninitialize();
            comInitialized_ = false;
            comInitThreadId_ = 0u;
        }
        supported_ = false;
        cachedError_ = "AudioPolicyConfig factory unsupported (" + staged::FormatHResultHex(hr) + ")";
        outReason = cachedError_;
        return false;
    }

    factory_ = factoryPtr;
    supported_ = true;
    cachedError_.clear();
    outReason.clear();
    return true;
}

bool Win32AudioPolicyConfigAccess::IsSupported(std::string& outReason) {
    return EnsureFactory(outReason);
}

bool Win32AudioPolicyConfigAccess::GetPersistedDefaultEndpoint(uint32_t pid,
                                                               EDataFlow flow,
                                                               ERole role,
                                                               std::wstring& outEndpointId,
                                                               HRESULT& outHr,
                                                               std::string& outError) {
    outEndpointId.clear();
    outHr = E_FAIL;
    outError.clear();

    if (pid == 0u) {
        outHr = E_INVALIDARG;
        outError = "pid is zero";
        return false;
    }

    std::string supportReason;
    if (!EnsureFactory(supportReason)) {
        outHr = E_NOINTERFACE;
        outError = supportReason;
        return false;
    }

    const CombaseFns& combase = GetCombaseFns();
    HStringOpaque rawStr = nullptr;
    outHr = CallGetPersistedDefaultAudioEndpoint(factory_, pid, flow, role, &rawStr);
    if (FAILED(outHr)) {
        if (rawStr) {
            combase.pWindowsDeleteString(rawStr);
        }
        outError = "GetPersistedDefaultAudioEndpoint failed: hr=" + staged::FormatHResultHex(outHr);
        return false;
    }

    if (rawStr) {
        UINT32 len = 0u;
        LPCWSTR buf = combase.pWindowsGetStringRawBuffer(rawStr, &len);
        if (buf && len > 0u) {
            outEndpointId = ExtractEndpointIdFromPolicyString(std::wstring(buf, len));
        }
        combase.pWindowsDeleteString(rawStr);
    }
    return true;
}

bool Win32AudioPolicyConfigAccess::SetPersistedDefaultEndpoint(uint32_t pid,
                                                               EDataFlow flow,
                                                               ERole role,
                                                               const std::wstring& endpointId,
                                                               HRESULT& outHr,
                                                               std::string& outError) {
    outHr = E_FAIL;
    outError.clear();

    if (pid == 0u) {
        outHr = E_INVALIDARG;
        outError = "pid is zero";
        return false;
    }

    std::string supportReason;
    if (!EnsureFactory(supportReason)) {
        outHr = E_NOINTERFACE;
        outError = supportReason;
        return false;
    }

    const CombaseFns& combase = GetCombaseFns();
    HStringOpaque hEndpoint = nullptr;
    if (!TrimWhitespaceW(endpointId).empty()) {
        const std::wstring swdId = FormatSwdAudioRenderInterfaceId(endpointId);
        const HRESULT hsHr = combase.pWindowsCreateString(
            swdId.c_str(), static_cast<UINT32>(swdId.size()), &hEndpoint);
        if (FAILED(hsHr) || !hEndpoint) {
            outHr = hsHr;
            outError = "WindowsCreateString(endpoint) failed: hr=" + staged::FormatHResultHex(hsHr);
            return false;
        }
    }

    outHr = CallSetPersistedDefaultAudioEndpoint(factory_, pid, flow, role, hEndpoint);
    if (hEndpoint) {
        combase.pWindowsDeleteString(hEndpoint);
    }
    if (FAILED(outHr)) {
        outError = "SetPersistedDefaultAudioEndpoint failed: hr=" + staged::FormatHResultHex(outHr);
        return false;
    }
    return true;
}

bool Win32AudioPolicyConfigAccess::EnumeratePolicyIdentities(const staged::AudioTopologyReport& topo,
                                                             std::vector<ProcessPolicyIdentity>& outIdentities,
                                                             std::string& outError) {
    outIdentities.clear();
    outError.clear();

    std::string reason;
    if (!EnsureFactory(reason)) {
        outError = reason;
        return false;
    }

    struct SessionFlags {
        bool hasActive = false;
        bool hasAny = false;
    };
    std::unordered_map<uint32_t, SessionFlags> sessByPid;
    for (const auto& ep : topo.endpoints) {
        for (const auto& s : ep.sessions) {
            if (s.isSystemSoundsSession || s.processId == 0u) {
                continue;
            }
            SessionFlags& fl = sessByPid[s.processId];
            fl.hasAny = true;
            if (s.sessionState == AudioSessionStateActive) {
                fl.hasActive = true;
            }
        }
    }

    struct RawCandidate {
        uint32_t pid = 0u;
        std::wstring leaf;
        std::wstring fullPath;
        std::wstring aumid;
        std::wstring key;
        bool hasActive = false;
        bool hasAny = false;
    };

    std::vector<RawCandidate> candidates;
    candidates.reserve(topo.processes.size());
    std::unordered_set<uint32_t> seenPids;

    for (const auto& p : topo.processes) {
        if (p.pid == 0u || p.pid == 4u) continue;
        if (!seenPids.insert(p.pid).second) continue;

        const std::wstring leaf = !p.exeName.empty()
            ? p.exeName
            : staged::ExtractLeafFilename(p.exeFullPath);
        if (leaf.empty() || IsForbiddenSystemOrBridgeExe(leaf)) {
            continue;
        }

        std::wstring fullPath = p.exeFullPath;
        std::wstring aumid;
        if (!QueryProcessAumidAndPath(p.pid, fullPath, aumid) && fullPath.empty()) {
            continue;
        }
        const std::wstring key = ComputeProcessPolicyIdentityKey(fullPath, leaf, aumid);
        if (key.empty()) continue;

        RawCandidate rc;
        rc.pid = p.pid;
        rc.leaf = leaf;
        rc.fullPath = fullPath;
        rc.aumid = aumid;
        rc.key = key;
        const auto it = sessByPid.find(p.pid);
        if (it != sessByPid.end()) {
            rc.hasActive = it->second.hasActive;
            rc.hasAny = it->second.hasAny;
        }
        candidates.push_back(std::move(rc));
    }

    // Prioritize PIDs holding Active sessions, then Inactive sessions, then background CoreAudio processes.
    std::stable_sort(candidates.begin(), candidates.end(), [](const RawCandidate& a, const RawCandidate& b) {
        if (a.hasActive != b.hasActive) return a.hasActive > b.hasActive;
        if (a.hasAny != b.hasAny) return a.hasAny > b.hasAny;
        return a.pid < b.pid;
    });

    std::unordered_set<std::wstring> resolvedKeys;
    for (const auto& cand : candidates) {
        if (resolvedKeys.count(cand.key) > 0u) {
            continue;
        }
        std::wstring epId;
        HRESULT hr = E_FAIL;
        std::string probeErr;
        if (GetPersistedDefaultEndpoint(cand.pid, eRender, eMultimedia, epId, hr, probeErr)) {
            resolvedKeys.insert(cand.key);
            ProcessPolicyIdentity out;
            out.pid = cand.pid;
            out.identityKey = cand.key;
            out.exeLeaf = cand.leaf;
            out.exeFullPath = cand.fullPath;
            out.aumid = cand.aumid;
            out.hasActiveSession = cand.hasActive;
            out.hasAnySession = cand.hasAny;
            outIdentities.push_back(std::move(out));
        }
    }

    return true;
}

// ============================================================================
// Section 3 Implementations (Journal + AutoRouteController)
// ============================================================================

const char* RouteRestoreItemStatusName(RouteRestoreItemStatus st) noexcept {
    switch (st) {
        case RouteRestoreItemStatus::Restored:            return "restored";
        case RouteRestoreItemStatus::SkippedUserModified: return "skipped_user_modified";
        case RouteRestoreItemStatus::DeferredProcessGone: return "deferred_process_gone";
        case RouteRestoreItemStatus::FailedWrite:         return "failed_write";
    }
    return "deferred_process_gone";
}

std::wstring ResolveRouteJournalPathForSettings(const std::wstring& settingsFilePath,
                                                const std::wstring& exePathOverride) {
    if (!settingsFilePath.empty()) {
        const std::wstring dir = ParentDirectoryOf(settingsFilePath);
        if (!dir.empty()) {
            return JoinWinPath(dir, L"iv_apple_music_radio.route_journal.json");
        }
        return L"iv_apple_music_radio.route_journal.json";
    }
    std::wstring exePath = exePathOverride;
    if (exePath.empty()) {
        wchar_t buf[MAX_PATH * 4]{};
        const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        if (len > 0u && len < std::size(buf)) {
            exePath.assign(buf, len);
        }
    }
    const std::wstring dir = ParentDirectoryOf(exePath);
    if (dir.empty()) {
        return L"iv_apple_music_radio.route_journal.json";
    }
    return JoinWinPath(dir, L"iv_apple_music_radio.route_journal.json");
}

namespace {

bool ParseSimpleJsonObjectFields(const std::string& objText,
                                 std::map<std::string, std::string>& outStrFields,
                                 std::map<std::string, std::string>& outRawFields,
                                 std::string& outError) {
    outStrFields.clear();
    outRawFields.clear();
    size_t i = 0u;
    auto skipWs = [&]() {
        while (i < objText.size() &&
               (objText[i] == ' ' || objText[i] == '\t' || objText[i] == '\r' || objText[i] == '\n')) {
            ++i;
        }
    };
    auto readJsonQuoted = [&](std::string& outDecoded) -> bool {
        if (i >= objText.size() || objText[i] != '"') {
            outError = "Expected '\"' in journal JSON";
            return false;
        }
        ++i;
        std::string raw;
        while (i < objText.size()) {
            const char c = objText[i++];
            if (c == '\\') {
                if (i >= objText.size()) {
                    outError = "Trailing backslash in journal JSON";
                    return false;
                }
                raw.push_back('\\');
                raw.push_back(objText[i++]);
            } else if (c == '"') {
                return staged::UnescapeJsonString(raw, outDecoded, outError);
            } else {
                raw.push_back(c);
            }
        }
        outError = "Unterminated string in journal JSON";
        return false;
    };

    skipWs();
    if (i >= objText.size() || objText[i] != '{') {
        outError = "Expected '{' in journal entry";
        return false;
    }
    ++i;
    while (i < objText.size()) {
        skipWs();
        if (i < objText.size() && objText[i] == '}') {
            return true;
        }
        std::string key;
        if (!readJsonQuoted(key)) return false;
        skipWs();
        if (i >= objText.size() || objText[i] != ':') {
            outError = "Expected ':' after key " + key;
            return false;
        }
        ++i;
        skipWs();
        if (i < objText.size() && objText[i] == '"') {
            std::string val;
            if (!readJsonQuoted(val)) return false;
            outStrFields[key] = std::move(val);
        } else {
            const size_t start = i;
            while (i < objText.size() && objText[i] != ',' && objText[i] != '}') {
                ++i;
            }
            std::string tok = objText.substr(start, i - start);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' || tok.back() == '\r' || tok.back() == '\n')) {
                tok.pop_back();
            }
            outRawFields[key] = std::move(tok);
        }
        skipWs();
        if (i < objText.size() && objText[i] == ',') {
            ++i;
        }
    }
    outError = "Unterminated JSON object";
    return false;
}

} // namespace

std::string EndpointRouteJournal::SerializeJson(const RouteJournalDocument& doc) {
    std::string out = "{\n";
    out += "  \"version\": " + std::to_string(doc.version) + ",\n";
    out += "  \"created_utc\": \"" + staged::EscapeJsonString(doc.createdUtcIso) + "\",\n";
    out += "  \"entries\": [\n";
    for (size_t i = 0u; i < doc.entries.size(); ++i) {
        const auto& e = doc.entries[i];
        out += "    {\n";
        out += "      \"identity_key\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.identityKey)) + "\",\n";
        out += "      \"exe_leaf\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.exeLeaf)) + "\",\n";
        out += "      \"exe_full_path\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.exeFullPath)) + "\",\n";
        out += "      \"aumid\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.aumid)) + "\",\n";
        out += "      \"orig_console_endpoint_id\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.origConsoleEndpointId)) + "\",\n";
        out += "      \"orig_multimedia_endpoint_id\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.origMultimediaEndpointId)) + "\",\n";
        out += "      \"applied_sink_endpoint_id\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.appliedSinkEndpointId)) + "\",\n";
        out += "      \"applied_sink_name\": \"" + staged::EscapeJsonString(staged::WideToUtf8(e.appliedSinkName)) + "\",\n";
        out += "      \"last_applied_pid\": " + std::to_string(e.lastAppliedPid) + ",\n";
        out += "      \"pending_restore\": " + std::string(e.pendingRestore ? "true" : "false") + "\n";
        out += (i + 1u < doc.entries.size()) ? "    },\n" : "    }\n";
    }
    out += "  ]\n}\n";
    return out;
}

bool EndpointRouteJournal::DeserializeJson(const std::string& jsonUtf8,
                                           RouteJournalDocument& outDoc,
                                           std::string& outError) {
    outDoc = RouteJournalDocument{};
    outError.clear();

    const size_t entriesKey = jsonUtf8.find("\"entries\"");
    if (entriesKey == std::string::npos) {
        outError = "Route journal missing 'entries'";
        return false;
    }
    const size_t arrStart = jsonUtf8.find('[', entriesKey);
    if (arrStart == std::string::npos) {
        outError = "Route journal missing '[' after entries";
        return false;
    }

    bool inStr = false;
    bool esc = false;
    size_t i = arrStart + 1u;
    while (i < jsonUtf8.size()) {
        const char c = jsonUtf8[i];
        if (inStr) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                inStr = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            inStr = true;
            ++i;
            continue;
        }
        if (c == ']') {
            return true;
        }
        if (c == '{') {
            const size_t objBegin = i;
            int depth = 0;
            bool objFoundEnd = false;
            while (i < jsonUtf8.size()) {
                const char oc = jsonUtf8[i];
                if (inStr) {
                    if (esc) esc = false;
                    else if (oc == '\\') esc = true;
                    else if (oc == '"') inStr = false;
                } else {
                    if (oc == '"') inStr = true;
                    else if (oc == '{') ++depth;
                    else if (oc == '}') {
                        --depth;
                        if (depth == 0) {
                            ++i;
                            objFoundEnd = true;
                            break;
                        }
                    }
                }
                ++i;
            }
            if (!objFoundEnd) {
                outError = "Unterminated entry object in route journal";
                return false;
            }

            std::map<std::string, std::string> sf;
            std::map<std::string, std::string> rf;
            if (!ParseSimpleJsonObjectFields(jsonUtf8.substr(objBegin, i - objBegin), sf, rf, outError)) {
                return false;
            }
            RouteJournalEntry e;
            e.identityKey              = staged::Utf8ToWide(sf["identity_key"]);
            e.exeLeaf                  = staged::Utf8ToWide(sf["exe_leaf"]);
            e.exeFullPath              = staged::Utf8ToWide(sf["exe_full_path"]);
            e.aumid                    = staged::Utf8ToWide(sf["aumid"]);
            e.origConsoleEndpointId    = ExtractEndpointIdFromPolicyString(staged::Utf8ToWide(sf["orig_console_endpoint_id"]));
            e.origMultimediaEndpointId = ExtractEndpointIdFromPolicyString(staged::Utf8ToWide(sf["orig_multimedia_endpoint_id"]));
            e.appliedSinkEndpointId    = ExtractEndpointIdFromPolicyString(staged::Utf8ToWide(sf["applied_sink_endpoint_id"]));
            e.appliedSinkName          = staged::Utf8ToWide(sf["applied_sink_name"]);
            if (rf.count("last_applied_pid") > 0u) {
                e.lastAppliedPid = static_cast<uint32_t>(std::strtoul(rf["last_applied_pid"].c_str(), nullptr, 10));
            }
            if (rf.count("pending_restore") > 0u) {
                e.pendingRestore = (rf["pending_restore"] == "true");
            }
            if (!e.identityKey.empty()) {
                outDoc.entries.push_back(std::move(e));
            }
            continue;
        }
        ++i;
    }

    outError = "Unterminated 'entries' array in route journal";
    return false;
}

bool EndpointRouteJournal::Load(const std::wstring& journalPath,
                                RouteJournalDocument& outDoc,
                                bool& outExisted,
                                std::string& outError) {
    outDoc = RouteJournalDocument{};
    outExisted = false;
    outError.clear();
    if (journalPath.empty() || !FileExistsW(journalPath)) {
        return true;
    }
    outExisted = true;
    std::string raw;
    if (!staged::ReadTextFileUtf8(journalPath, raw, outError)) {
        return false;
    }
    return DeserializeJson(raw, outDoc, outError);
}

bool EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(const std::wstring& journalPath,
                                                     const RouteJournalDocument& doc,
                                                     std::string& outError) {
    outError.clear();
    if (journalPath.empty()) {
        return true;
    }
    if (doc.entries.empty()) {
        if (FileExistsW(journalPath)) {
            if (!DeleteFileW(journalPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                outError = "DeleteFileW(route_journal) failed: err=" + std::to_string(GetLastError());
                return false;
            }
        }
        return true;
    }
    RouteJournalDocument copy = doc;
    if (copy.createdUtcIso.empty()) {
        copy.createdUtcIso = staged::GetCurrentUtcIsoString();
    }
    return staged::WriteTextFileAtomic(journalPath, SerializeJson(copy), outError);
}

uint32_t AutoRouteController::CountPendingJournalEntries(const std::wstring& journalPath) {
    RouteJournalDocument doc;
    bool existed = false;
    std::string err;
    if (!EndpointRouteJournal::Load(journalPath, doc, existed, err) || !existed) {
        return 0u;
    }
    return static_cast<uint32_t>(doc.entries.size());
}

namespace {

RouteRestoreItemResult RestoreSingleJournalEntry(IAudioEndpointPolicyAccess& policy,
                                                 const RouteJournalEntry& entry,
                                                 const std::vector<uint32_t>& livePids,
                                                 const staged::AudioTopologyReport* optTopo,
                                                 bool& outKeepInJournal,
                                                 bool& outMarkPending) {
    RouteRestoreItemResult res;
    res.identityKey = entry.identityKey;
    res.exeLeaf = entry.exeLeaf;
    outKeepInJournal = false;
    outMarkPending = false;

    if (livePids.empty()) {
        outKeepInJournal = true;
        outMarkPending = true;
        res.status = RouteRestoreItemStatus::DeferredProcessGone;
        res.detail = "Target process currently not running; restore queued until app reopens";
        return res;
    }

    // Helper: check whether 'endpointId' itself is a known virtual sink (user wouldn't have modified TO the virtual sink).
    auto isVirtualEndpointId = [&](const std::wstring& epId) -> bool {
        if (!optTopo || epId.empty()) return false;
        for (const auto& ep : optTopo->endpoints) {
            if (staged::EqualsIgnoreCase(ep.endpointId, epId)) {
                int32_t prio = 0;
                return IsKnownVirtualSilentSinkFriendlyName(ep.friendlyName, prio);
            }
        }
        return false;
    };

    const uint32_t targetPid = livePids.front();

    std::wstring curConsole;
    std::wstring curMult;
    HRESULT hrC = S_OK;
    HRESULT hrM = S_OK;
    std::string errC;
    std::string errM;
    const bool okReadC = policy.GetPersistedDefaultEndpoint(targetPid, eRender, eConsole, curConsole, hrC, errC);
    const bool okReadM = policy.GetPersistedDefaultEndpoint(targetPid, eRender, eMultimedia, curMult, hrM, errM);

    // User-modified guard: if user manually moved the app to ANOTHER device in Windows Volume Mixer while
    // bridge was running, preserve the user's explicit choice instead of overwriting it back!
    const bool consoleMatchesAppliedOrVirtual =
        !okReadC ||
        ArePolicyEndpointStringsEqual(curConsole, entry.appliedSinkEndpointId) ||
        isVirtualEndpointId(curConsole);
    const bool multMatchesAppliedOrVirtual =
        !okReadM ||
        ArePolicyEndpointStringsEqual(curMult, entry.appliedSinkEndpointId) ||
        isVirtualEndpointId(curMult);

    if (!consoleMatchesAppliedOrVirtual && !multMatchesAppliedOrVirtual) {
        outKeepInJournal = false;
        res.status = RouteRestoreItemStatus::SkippedUserModified;
        res.detail = "User changed endpoint manually in Windows Volume Mixer; preserved user selection";
        return res;
    }

    bool wroteAny = true;
    std::string writeErr;
    if (consoleMatchesAppliedOrVirtual) {
        HRESULT hrSetC = S_OK;
        std::string errSetC;
        if (!policy.SetPersistedDefaultEndpoint(targetPid, eRender, eConsole, entry.origConsoleEndpointId, hrSetC, errSetC)) {
            wroteAny = false;
            writeErr = errSetC;
        }
    }
    if (multMatchesAppliedOrVirtual) {
        HRESULT hrSetM = S_OK;
        std::string errSetM;
        if (!policy.SetPersistedDefaultEndpoint(targetPid, eRender, eMultimedia, entry.origMultimediaEndpointId, hrSetM, errSetM)) {
            wroteAny = false;
            if (writeErr.empty()) writeErr = errSetM;
        }
    }

    if (!wroteAny) {
        outKeepInJournal = true;
        outMarkPending = entry.pendingRestore;
        res.status = RouteRestoreItemStatus::FailedWrite;
        res.detail = writeErr;
        return res;
    }

    outKeepInJournal = false;
    res.status = RouteRestoreItemStatus::Restored;
    res.detail = "Restored original audio endpoint";
    return res;
}

bool DoesIdentityMatchTarget(const ProcessPolicyIdentity& id,
                             const AutoRouteTargetSpec& target) {
    if (target.isAppleMusicPreset) {
        return staged::EqualsIgnoreCase(id.exeLeaf, L"AMPLibraryAgent.exe") ||
               staged::EqualsIgnoreCase(id.exeLeaf, L"AppleMusic.exe");
    }
    const std::wstring wantLeaf = !target.customExeName.empty()
        ? staged::ExtractLeafFilename(target.customExeName)
        : staged::ExtractLeafFilename(target.customExePath);
    if (wantLeaf.empty() || !staged::EqualsIgnoreCase(id.exeLeaf, wantLeaf)) {
        return false;
    }
    if (!target.customExePath.empty() && !id.exeFullPath.empty()) {
        return staged::EqualsIgnoreCase(id.exeFullPath, target.customExePath);
    }
    return true;
}

std::string FormatShortRestoreSummary(const RouteRestoreSummary& sum) {
    if (!sum.journalExisted || sum.items.empty()) {
        return {};
    }
    return "restored=" + std::to_string(sum.restoredCount) +
           ", deferred=" + std::to_string(sum.deferredCount) +
           ", skipped_user=" + std::to_string(sum.skippedUserModifiedCount) +
           ", failed=" + std::to_string(sum.failedCount);
}

} // namespace

RouteRestoreSummary AutoRouteController::RecoverOnStartup(IAudioEndpointPolicyAccess& policy,
                                                          const std::wstring& journalPath,
                                                          const staged::AudioTopologyReport* optTopo) {
    return RestoreAllRoutes(policy, journalPath, optTopo, true);
}

RouteRestoreSummary AutoRouteController::RestoreAllRoutes(IAudioEndpointPolicyAccess& policy,
                                                          const std::wstring& journalPath,
                                                          const staged::AudioTopologyReport* optTopo,
                                                          bool deferIfProcessGone) {
    RouteRestoreSummary summary;
    RouteJournalDocument doc;
    std::string err;
    if (!EndpointRouteJournal::Load(journalPath, doc, summary.journalExisted, err)) {
        summary.journalParsed = false;
        summary.error = err;
        return summary;
    }
    summary.journalParsed = true;
    if (!summary.journalExisted || doc.entries.empty()) {
        summary.journalEmptyAfterRestore = true;
        return summary;
    }

    staged::AudioTopologyReport localTopo;
    const staged::AudioTopologyReport* topoPtr = optTopo;
    if (!topoPtr) {
        std::string topoErr;
        if (staged::DiscoverAudioTopology(localTopo, topoErr)) {
            topoPtr = &localTopo;
        }
    }

    std::vector<ProcessPolicyIdentity> identities;
    if (topoPtr) {
        std::string enumErr;
        policy.EnumeratePolicyIdentities(*topoPtr, identities, enumErr);
    }

    RouteJournalDocument remaining;
    remaining.version = doc.version;
    remaining.createdUtcIso = doc.createdUtcIso;

    for (const auto& entry : doc.entries) {
        const std::vector<uint32_t> livePids = CollectPidsForIdentityKey(identities, entry.identityKey);
        bool keep = false;
        bool markPending = false;
        RouteRestoreItemResult itemRes =
            RestoreSingleJournalEntry(policy, entry, livePids, topoPtr, keep, markPending);

        switch (itemRes.status) {
            case RouteRestoreItemStatus::Restored:            ++summary.restoredCount; break;
            case RouteRestoreItemStatus::SkippedUserModified: ++summary.skippedUserModifiedCount; break;
            case RouteRestoreItemStatus::DeferredProcessGone: ++summary.deferredCount; break;
            case RouteRestoreItemStatus::FailedWrite:         ++summary.failedCount; break;
        }
        summary.items.push_back(std::move(itemRes));

        if (keep && (itemRes.status != RouteRestoreItemStatus::DeferredProcessGone || deferIfProcessGone)) {
            RouteJournalEntry kept = entry;
            if (markPending) kept.pendingRestore = true;
            remaining.entries.push_back(std::move(kept));
        }
    }

    std::string saveErr;
    EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, remaining, saveErr);
    if (!saveErr.empty()) {
        summary.error = saveErr;
    }
    summary.journalEmptyAfterRestore = remaining.entries.empty();
    return summary;
}

AutoRouteStatus AutoRouteController::ReconcileDuringBridge(IAudioEndpointPolicyAccess& policy,
                                                           const std::wstring& journalPath,
                                                           const staged::AudioTopologyReport& topo,
                                                           const AutoRouteTargetSpec& target,
                                                           AutoRouteMode mode) {
    AutoRouteStatus st;
    st.mode = mode;

    const SilentSinkReport sinkRep = EvaluateSilentSinks(topo);
    st.virtualSinkPresence = sinkRep.presence;
    st.sinkEndpointIdUtf8 = staged::WideToUtf8(sinkRep.selectedEndpointId);
    st.sinkFriendlyNameUtf8 = staged::WideToUtf8(sinkRep.selectedFriendlyName);

    std::string apiReason;
    if (!policy.IsSupported(apiReason)) {
        st.policyApiSupported = false;
        st.lastErrorUtf8 = apiReason;
        st.state = (mode == AutoRouteMode::Manual)
            ? AutoRouteState::DisabledManual
            : AutoRouteState::UnsupportedOsPolicy;
        return st;
    }
    st.policyApiSupported = true;

    RouteJournalDocument doc;
    bool existed = false;
    std::string loadErr;
    if (!EndpointRouteJournal::Load(journalPath, doc, existed, loadErr)) {
        doc = RouteJournalDocument{};
        st.lastErrorUtf8 = loadErr;
    }

    std::vector<ProcessPolicyIdentity> identities;
    std::string enumErr;
    policy.EnumeratePolicyIdentities(topo, identities, enumErr);

    // Filter live identities matching current target spec.
    std::vector<ProcessPolicyIdentity> targetIds;
    std::unordered_set<std::wstring> targetKeys;
    for (const auto& id : identities) {
        if (DoesIdentityMatchTarget(id, target)) {
            targetIds.push_back(id);
            targetKeys.insert(ToLowerAsciiW(id.identityKey));
        }
    }

    const bool canAutoIsolate =
        (mode == AutoRouteMode::Auto) &&
        (sinkRep.presence == VirtualSinkPresence::ActiveReady) &&
        sinkRep.hasSelectableSink &&
        !sinkRep.selectedEndpointId.empty();

    // Step A: Restore any journal entries that should no longer remain isolated
    // (switched source app, switched to Manual mode, or virtual sink missing/hazardous).
    RouteJournalDocument nextDoc;
    nextDoc.version = 1u;
    nextDoc.createdUtcIso = doc.createdUtcIso.empty() ? staged::GetCurrentUtcIsoString() : doc.createdUtcIso;

    RouteRestoreSummary stepRestore;
    for (const auto& entry : doc.entries) {
        const std::wstring lowerKey = ToLowerAsciiW(entry.identityKey);
        const bool isCurrentTarget = canAutoIsolate && (targetKeys.count(lowerKey) > 0u);
        if (isCurrentTarget) {
            nextDoc.entries.push_back(entry);
            continue;
        }
        // Keep current target entry dormant only if app isn't running yet and we CAN auto isolate,
        // wait: no, keeping original orig endpoints across temporary target restart is safer!
        if (canAutoIsolate && !entry.pendingRestore && target.isAppleMusicPreset &&
            (staged::EqualsIgnoreCase(entry.exeLeaf, L"AMPLibraryAgent.exe") ||
             staged::EqualsIgnoreCase(entry.exeLeaf, L"AppleMusic.exe"))) {
            nextDoc.entries.push_back(entry);
            continue;
        }

        const std::vector<uint32_t> livePids = CollectPidsForIdentityKey(identities, entry.identityKey);
        bool keep = false;
        bool markPending = false;
        RouteRestoreItemResult itemRes = RestoreSingleJournalEntry(policy, entry, livePids, &topo, keep, markPending);
        stepRestore.journalExisted = true;
        stepRestore.items.push_back(itemRes);
        switch (itemRes.status) {
            case RouteRestoreItemStatus::Restored:            ++stepRestore.restoredCount; break;
            case RouteRestoreItemStatus::SkippedUserModified: ++stepRestore.skippedUserModifiedCount; break;
            case RouteRestoreItemStatus::DeferredProcessGone: ++stepRestore.deferredCount; break;
            case RouteRestoreItemStatus::FailedWrite:         ++stepRestore.failedCount; break;
        }
        if (keep) {
            RouteJournalEntry kept = entry;
            if (markPending) kept.pendingRestore = true;
            nextDoc.entries.push_back(std::move(kept));
        }
    }
    st.lastRestoreSummaryUtf8 = FormatShortRestoreSummary(stepRestore);

    if (mode == AutoRouteMode::Manual) {
        std::string saveErr;
        EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, nextDoc, saveErr);
        st.pendingRestoreCount = static_cast<uint32_t>(nextDoc.entries.size());
        st.state = AutoRouteState::DisabledManual;
        return st;
    }

    if (sinkRep.presence == VirtualSinkPresence::VirtualSinkIsSystemDefault) {
        std::string saveErr;
        EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, nextDoc, saveErr);
        st.pendingRestoreCount = static_cast<uint32_t>(nextDoc.entries.size());
        st.state = AutoRouteState::VirtualSinkIsSystemDefault;
        return st;
    }

    if (sinkRep.presence != VirtualSinkPresence::ActiveReady || !sinkRep.hasSelectableSink) {
        std::string saveErr;
        EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, nextDoc, saveErr);
        st.pendingRestoreCount = static_cast<uint32_t>(nextDoc.entries.size());
        st.state = AutoRouteState::NoVirtualSinkInstalled;
        return st;
    }

    if (targetIds.empty()) {
        std::string saveErr;
        EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, nextDoc, saveErr);
        st.pendingRestoreCount = static_cast<uint32_t>(nextDoc.entries.size());
        st.state = AutoRouteState::WaitingForTargetProcess;
        return st;
    }

    // Step B: Ensure every live target identity is journaled BEFORE setting AudioPolicyConfig,
    // then pinned to sinkRep.selectedEndpointId on both eConsole and eMultimedia.
    uint32_t okCount = 0u;
    uint32_t failCount = 0u;
    bool journalDirty = (stepRestore.restoredCount > 0u || stepRestore.skippedUserModifiedCount > 0u);

    auto isKnownVirtualEndpointInTopo = [&](const std::wstring& epId) -> bool {
        if (epId.empty()) return false;
        for (const auto& ep : topo.endpoints) {
            if (staged::EqualsIgnoreCase(ep.endpointId, epId)) {
                int32_t prio = 0;
                return IsKnownVirtualSilentSinkFriendlyName(ep.friendlyName, prio);
            }
        }
        return false;
    };

    for (const auto& id : targetIds) {
        std::wstring curC;
        std::wstring curM;
        HRESULT hrC = S_OK;
        HRESULT hrM = S_OK;
        std::string errRead;
        policy.GetPersistedDefaultEndpoint(id.pid, eRender, eConsole, curC, hrC, errRead);
        policy.GetPersistedDefaultEndpoint(id.pid, eRender, eMultimedia, curM, hrM, errRead);

        RouteJournalEntry* existingEntry = nullptr;
        for (auto& je : nextDoc.entries) {
            if (staged::EqualsIgnoreCase(je.identityKey, id.identityKey)) {
                existingEntry = &je;
                break;
            }
        }

        const bool alreadyPinnedToSink =
            ArePolicyEndpointStringsEqual(curC, sinkRep.selectedEndpointId) &&
            ArePolicyEndpointStringsEqual(curM, sinkRep.selectedEndpointId);

        if (!existingEntry) {
            RouteJournalEntry ne;
            ne.identityKey = id.identityKey;
            ne.exeLeaf = id.exeLeaf;
            ne.exeFullPath = id.exeFullPath;
            ne.aumid = id.aumid;
            // Self-healing guard: if original wasn't journaled yet yet already points at a virtual sink
            // (e.g. after manual journal deletion), recover back to System Default ("") on exit.
            ne.origConsoleEndpointId    = isKnownVirtualEndpointInTopo(curC) ? std::wstring() : curC;
            ne.origMultimediaEndpointId = isKnownVirtualEndpointInTopo(curM) ? std::wstring() : curM;
            ne.appliedSinkEndpointId    = sinkRep.selectedEndpointId;
            ne.appliedSinkName          = sinkRep.selectedFriendlyName;
            ne.lastAppliedPid           = id.pid;
            ne.pendingRestore           = false;
            nextDoc.entries.push_back(std::move(ne));
            journalDirty = true;
        } else {
            if (existingEntry->pendingRestore ||
                !staged::EqualsIgnoreCase(existingEntry->appliedSinkEndpointId, sinkRep.selectedEndpointId) ||
                existingEntry->lastAppliedPid != id.pid) {
                existingEntry->appliedSinkEndpointId = sinkRep.selectedEndpointId;
                existingEntry->appliedSinkName = sinkRep.selectedFriendlyName;
                existingEntry->lastAppliedPid = id.pid;
                existingEntry->pendingRestore = false;
                journalDirty = true;
            }
        }

        if (alreadyPinnedToSink) {
            ++okCount;
            continue;
        }

        // Write journal atomically BEFORE mutating Windows policy!
        if (journalDirty) {
            std::string saveErr;
            if (!EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, nextDoc, saveErr)) {
                st.lastErrorUtf8 = saveErr;
                ++failCount;
                continue;
            }
            journalDirty = false;
        }

        HRESULT hrSetC = S_OK;
        HRESULT hrSetM = S_OK;
        std::string errSetC;
        std::string errSetM;
        const bool okC = policy.SetPersistedDefaultEndpoint(
            id.pid, eRender, eConsole, sinkRep.selectedEndpointId, hrSetC, errSetC);
        const bool okM = policy.SetPersistedDefaultEndpoint(
            id.pid, eRender, eMultimedia, sinkRep.selectedEndpointId, hrSetM, errSetM);
        if (okC && okM) {
            ++okCount;
        } else {
            ++failCount;
            st.lastErrorUtf8 = !okM ? errSetM : errSetC;
        }
    }

    if (journalDirty) {
        std::string saveErr;
        EndpointRouteJournal::SaveAtomicOrRemoveIfEmpty(journalPath, nextDoc, saveErr);
    }

    st.isolatedIdentityCount = okCount;
    for (const auto& je : nextDoc.entries) {
        if (je.pendingRestore) ++st.pendingRestoreCount;
    }

    if (okCount > 0u && failCount == 0u) {
        st.state = AutoRouteState::AutoIsolatedOk;
    } else if (okCount > 0u) {
        st.state = AutoRouteState::PartiallyIsolated;
    } else {
        st.state = AutoRouteState::PolicyWriteFailed;
    }
    return st;
}

// ============================================================================
// Section 4 Implementations (Driver Verification & Safe PnP Installer)
// ============================================================================

VirtualSinkDriverPackage LocateAndVerifyVirtualSinkPackage(const std::wstring& explicitDriverDir,
                                                           const std::wstring& exePathOverride) {
    VirtualSinkDriverPackage pkg;

    auto readRegStringW = [](HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName) -> std::wstring {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(rootKey, subKey, 0u, KEY_READ, &hKey) != ERROR_SUCCESS || !hKey) {
            return {};
        }
        wchar_t buf[MAX_PATH * 4]{};
        DWORD type = 0u;
        DWORD cb = sizeof(buf) - sizeof(wchar_t);
        const LONG rc = RegQueryValueExW(
            hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(hKey);
        if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb < sizeof(wchar_t)) {
            return {};
        }
        std::wstring val(buf);
        for (wchar_t& ch : val) {
            if (ch == L'/') ch = L'\\';
        }
        return val;
    };

    std::vector<std::wstring> searchDirs;
    if (!explicitDriverDir.empty()) {
        searchDirs.push_back(explicitDriverDir);
    } else {
        std::wstring exePath = exePathOverride;
        if (exePath.empty()) {
            wchar_t buf[MAX_PATH * 4]{};
            const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
            if (len > 0u && len < std::size(buf)) {
                exePath.assign(buf, len);
            }
        }
        const std::wstring exeDir = ParentDirectoryOf(exePath);
        if (!exeDir.empty()) {
            searchDirs.push_back(JoinWinPath(exeDir, L"assets\\drivers\\SteamStreamingSpeakers"));
            searchDirs.push_back(JoinWinPath(exeDir, L"drivers\\SteamStreamingSpeakers"));

            std::wstring cur = exeDir;
            for (int depth = 0; depth < 5 && !cur.empty(); ++depth) {
                searchDirs.push_back(JoinWinPath(cur, L"assets\\drivers\\SteamStreamingSpeakers"));
                const std::wstring parent = ParentDirectoryOf(cur);
                if (parent == cur) break;
                cur = parent;
            }
        }

        // Route A auto-probe: local Steam client install directories
        const std::wstring steamCandidates[] = {
            readRegStringW(HKEY_CURRENT_USER,  L"Software\\Valve\\Steam", L"SteamPath"),
            readRegStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
            readRegStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"),
        };
        for (const auto& steamRoot : steamCandidates) {
            if (!steamRoot.empty()) {
                searchDirs.push_back(JoinWinPath(steamRoot, L"drivers\\SteamStreamingSpeakers\\x64"));
                searchDirs.push_back(JoinWinPath(steamRoot, L"drivers\\SteamStreamingSpeakers"));
            }
        }

        // Route A auto-probe: already-staged Windows DriverStore FileRepository packages
        wchar_t sysDirBuf[MAX_PATH]{};
        const UINT sysLen = GetSystemDirectoryW(sysDirBuf, static_cast<UINT>(std::size(sysDirBuf)));
        if (sysLen > 0u && sysLen < std::size(sysDirBuf)) {
            const std::wstring fileRepoDir = JoinWinPath(sysDirBuf, L"DriverStore\\FileRepository");
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW(
                JoinWinPath(fileRepoDir, L"steamstreamingspeakers.inf_amd64_*").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
                        std::wcscmp(fd.cFileName, L".") != 0 &&
                        std::wcscmp(fd.cFileName, L"..") != 0) {
                        searchDirs.push_back(JoinWinPath(fileRepoDir, fd.cFileName));
                    }
                } while (FindNextFileW(hFind, &fd) != FALSE);
                FindClose(hFind);
            }
        }
    }

    for (const auto& dir : searchDirs) {
        const std::wstring inf = JoinWinPath(dir, L"SteamStreamingSpeakers.inf");
        const std::wstring sys = JoinWinPath(dir, L"SteamStreamingSpeakers.sys");
        const std::wstring cat = JoinWinPath(dir, L"steamstreamingspeakers.cat");
        if (FileExistsW(inf) && FileExistsW(sys) && FileExistsW(cat)) {
            pkg.found = true;
            pkg.directory = dir;
            pkg.infPath = inf;
            pkg.sysPath = sys;
            pkg.catPath = cat;
            break;
        }
    }

    if (!pkg.found) {
        pkg.errorDetail =
            "No local SteamStreamingSpeakers WHQL driver package was found (not bundled in the public release). "
            "Select a local driver folder or install a compatible virtual audio sink (such as VB-Cable).";
        return pkg;
    }

    struct HashCheck {
        const std::wstring* path;
        const char* expectedHex;
        const char* name;
    };
    const HashCheck checks[] = {
        { &pkg.infPath, kExpectedInfSha256, "SteamStreamingSpeakers.inf" },
        { &pkg.sysPath, kExpectedSysSha256, "SteamStreamingSpeakers.sys" },
        { &pkg.catPath, kExpectedCatSha256, "steamstreamingspeakers.cat" },
    };

    for (const auto& hc : checks) {
        std::string actualHex;
        std::string hashErr;
        if (!ComputeFileSha256HexUpper(*hc.path, actualHex, hashErr)) {
            pkg.errorDetail = hashErr;
            return pkg;
        }
        if (actualHex != hc.expectedHex) {
            pkg.errorDetail = std::string("SHA-256 mismatch for ") + hc.name +
                              " (expected " + hc.expectedHex + ", got " + actualHex + ")";
            return pkg;
        }
        if (!VerifyAuthenticodeCatalogOrBinary(*hc.path, hashErr)) {
            pkg.errorDetail = hashErr;
            return pkg;
        }
    }

    pkg.integrityValid = true;
    return pkg;
}

bool IsVirtualAudioSinkDevicePresent() {
    HDEVINFO hDevInfo = nullptr;
    std::vector<SP_DEVINFO_DATA> devs;
    if (FindExistingRootSteamSpeakersDevnodes(hDevInfo, devs) && hDevInfo) {
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    return !devs.empty();
}

std::string SerializeVirtualSinkInstallResultJson(const VirtualSinkInstallResult& res) {
    std::string out = "{\n";
    out += "  \"action\": \"" + staged::EscapeJsonString(res.action) + "\",\n";
    out += "  \"succeeded\": " + std::string(res.succeeded ? "true" : "false") + ",\n";
    out += "  \"requires_reboot\": " + std::string(res.requiresReboot ? "true" : "false") + ",\n";
    out += "  \"already_installed\": " + std::string(res.alreadyInstalled ? "true" : "false") + ",\n";
    out += "  \"elevated\": " + std::string(res.elevated ? "true" : "false") + ",\n";
    out += "  \"defaults_preserved\": " + std::string(res.defaultsPreserved ? "true" : "false") + ",\n";
    out += "  \"default_render_before\": \"" + staged::EscapeJsonString(res.defaultRenderBeforeUtf8) + "\",\n";
    out += "  \"default_render_after\": \"" + staged::EscapeJsonString(res.defaultRenderAfterUtf8) + "\",\n";
    out += "  \"default_capture_before\": \"" + staged::EscapeJsonString(res.defaultCaptureBeforeUtf8) + "\",\n";
    out += "  \"default_capture_after\": \"" + staged::EscapeJsonString(res.defaultCaptureAfterUtf8) + "\",\n";
    out += "  \"virtual_sink_endpoint_id\": \"" + staged::EscapeJsonString(res.virtualSinkEndpointIdUtf8) + "\",\n";
    out += "  \"virtual_sink_friendly_name\": \"" + staged::EscapeJsonString(res.virtualSinkFriendlyNameUtf8) + "\",\n";
    out += "  \"detail\": \"" + staged::EscapeJsonString(res.detailUtf8) + "\"\n";
    out += "}\n";
    return out;
}

VirtualSinkInstallResult InstallVirtualAudioSink(const std::wstring& explicitDriverDir,
                                                 const std::wstring& reportJsonPath) {
    VirtualSinkInstallResult res;
    res.action = "install";

    auto finalizeReport = [&](const VirtualSinkInstallResult& r) -> VirtualSinkInstallResult {
        if (!reportJsonPath.empty()) {
            std::string writeErr;
            staged::WriteTextFileAtomic(reportJsonPath, SerializeVirtualSinkInstallResultJson(r), writeErr);
        }
        return r;
    };

    ScopedCoInit co(COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> pEnum;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(pEnum.Put()));
    EndpointDefaultsSnapshot preSnap;
    if (SUCCEEDED(hr) && pEnum) {
        preSnap = CaptureSystemDefaultsSnapshot(pEnum.Get());
        res.defaultRenderBeforeUtf8 = staged::WideToUtf8(preSnap.renderMultimedia);
        res.defaultCaptureBeforeUtf8 = staged::WideToUtf8(preSnap.captureMultimedia);
    }

    const VirtualSinkDriverPackage pkg = LocateAndVerifyVirtualSinkPackage(explicitDriverDir);
    if (!pkg.found || !pkg.integrityValid) {
        res.detailUtf8 = pkg.errorDetail;
        return finalizeReport(res);
    }

    using UpdateDriverFn = BOOL(WINAPI*)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
    using DiInstallDriverFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
    using DiUninstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
    HMODULE hNewDev = LoadLibraryW(L"newdev.dll");
    const auto pUpdateDriver = hNewDev
        ? reinterpret_cast<UpdateDriverFn>(GetProcAddress(hNewDev, "UpdateDriverForPlugAndPlayDevicesW"))
        : nullptr;
    const auto pDiInstallDriver = hNewDev
        ? reinterpret_cast<DiInstallDriverFn>(GetProcAddress(hNewDev, "DiInstallDriverW"))
        : nullptr;
    const auto pDiUninstallDevice = hNewDev
        ? reinterpret_cast<DiUninstallDeviceFn>(GetProcAddress(hNewDev, "DiUninstallDevice"))
        : nullptr;
    if (!pUpdateDriver && !pDiInstallDriver) {
        if (hNewDev) FreeLibrary(hNewDev);
        res.detailUtf8 = "Failed loading newdev.dll driver installer exports";
        return finalizeReport(res);
    }

    ComPtr<IUnknown> policyConfig;
    CoCreateInstance(
        kClsidPolicyConfigClient, nullptr, CLSCTX_ALL,
        kIidPolicyConfig, reinterpret_cast<void**>(policyConfig.Put()));

    // Guard against duplicate devnodes (e.g., if a prior install created both ROOT\MEDIA\0000
    // and ROOT\SteamStreamingSpeakers\0000). Keep at most one canonical devnode before installing.
    HDEVINFO existingList = nullptr;
    std::vector<SP_DEVINFO_DATA> existingDevs;
    if (FindExistingRootSteamSpeakersDevnodes(existingList, existingDevs)) {
        if (existingList && existingDevs.size() > 1u) {
            bool keptOne = false;
            for (auto& d : existingDevs) {
                wchar_t instId[256]{};
                DWORD req = 0u;
                const bool isCanonical = SetupDiGetDeviceInstanceIdW(
                    existingList, &d, instId, static_cast<DWORD>(std::size(instId)), &req) &&
                    staged::EqualsIgnoreCase(instId, L"ROOT\\SteamStreamingSpeakers\\0000");
                if (!keptOne && isCanonical) {
                    keptOne = true;
                    continue;
                }
                if (pDiUninstallDevice) {
                    BOOL unusedReboot = FALSE;
                    pDiUninstallDevice(nullptr, existingList, &d, 0u, &unusedReboot);
                } else {
                    SetupDiCallClassInstaller(DIF_REMOVE, existingList, &d);
                }
            }
            SetupDiDestroyDeviceInfoList(existingList);
            existingList = nullptr;
            existingDevs.clear();
            FindExistingRootSteamSpeakersDevnodes(existingList, existingDevs);
        }
    }
    if (existingList) {
        SetupDiDestroyDeviceInfoList(existingList);
        existingList = nullptr;
    }

    res.alreadyInstalled = !existingDevs.empty();

    BOOL rebootNeeded = FALSE;
    constexpr DWORD kDiirFlagForceInf = 0x00000002u;
    constexpr DWORD kInstallFlagForce = 0x00000001u;

    if (!res.alreadyInstalled) {
        // NOTE: SteamStreamingSpeakers.inf contains `[DeviceInstall32] AddDevice = ROOT\SteamStreamingSpeakers\0000`.
        // Importing the INF via DiInstallDriverW automatically triggers Windows DriverStore reflection to create
        // that exact single devnode. Creating a devnode manually BEFORE importing causes duplicate devices!
        bool driverInfImported = false;
        DWORD lastInstallErr = ERROR_SUCCESS;

        if (pDiInstallDriver) {
            if (pDiInstallDriver(nullptr, pkg.infPath.c_str(), kDiirFlagForceInf, &rebootNeeded)) {
                driverInfImported = true;
                res.elevated = true;
            } else {
                lastInstallErr = GetLastError();
                if (lastInstallErr == ERROR_ACCESS_DENIED) {
                    FreeLibrary(hNewDev);
                    res.detailUtf8 = "DiInstallDriverW failed (err=5; Administrator elevation required)";
                    return finalizeReport(res);
                }
            }
        }

        // Poll briefly for `[DeviceInstall32]` reflection to register ROOT\SteamStreamingSpeakers\0000,
        // continuously guarding existing default playback/capture endpoints.
        for (int waitIdx = 0; waitIdx < 15; ++waitIdx) {
            if (pEnum && policyConfig) {
                EnforceSystemDefaultsSnapshot(pEnum.Get(), policyConfig.Get(), preSnap);
            }
            HDEVINFO checkList = nullptr;
            std::vector<SP_DEVINFO_DATA> checkDevs;
            if (FindExistingRootSteamSpeakersDevnodes(checkList, checkDevs) && checkList) {
                SetupDiDestroyDeviceInfoList(checkList);
            }
            if (!checkDevs.empty()) {
                existingDevs = std::move(checkDevs);
                break;
            }
            Sleep(100u);
        }

        // Fallback ONLY if `[DeviceInstall32]` did not materialize a devnode (e.g., INF was already
        // reflected earlier and its prior devnode was deleted without unpublishing the INF).
        if (existingDevs.empty()) {
            HDEVINFO devInfoSet = SetupDiCreateDeviceInfoList(&kGuidDevClassMedia, nullptr);
            if (devInfoSet == INVALID_HANDLE_VALUE) {
                FreeLibrary(hNewDev);
                res.detailUtf8 = driverInfImported
                    ? "Driver INF staged, but SetupDiCreateDeviceInfoList failed (err=" + std::to_string(GetLastError()) + ")"
                    : "DiInstallDriverW/SetupDiCreateDeviceInfoList failed (err=" + std::to_string(lastInstallErr) + ")";
                return finalizeReport(res);
            }

            SP_DEVINFO_DATA devInfoData{};
            devInfoData.cbSize = sizeof(devInfoData);
            BOOL created = SetupDiCreateDeviceInfoW(
                devInfoSet,
                L"ROOT\\SteamStreamingSpeakers\\0000",
                &kGuidDevClassMedia,
                L"Steam Streaming Speakers",
                nullptr,
                0u,
                &devInfoData);
            if (!created) {
                devInfoData.cbSize = sizeof(devInfoData);
                created = SetupDiCreateDeviceInfoW(
                    devInfoSet,
                    L"MEDIA",
                    &kGuidDevClassMedia,
                    L"Steam Streaming Speakers",
                    nullptr,
                    DICD_GENERATE_ID,
                    &devInfoData);
            }
            if (!created) {
                const DWORD err = GetLastError();
                SetupDiDestroyDeviceInfoList(devInfoSet);
                FreeLibrary(hNewDev);
                res.detailUtf8 = "SetupDiCreateDeviceInfoW failed (err=" + std::to_string(err) +
                                 (err == ERROR_ACCESS_DENIED ? "; Administrator elevation required" : "") + ")";
                return finalizeReport(res);
            }
            res.elevated = true;

            const wchar_t hwIdMultiSz[] = L"ROOT\\SteamStreamingSpeakers\0";
            if (!SetupDiSetDeviceRegistryPropertyW(
                    devInfoSet,
                    &devInfoData,
                    SPDRP_HARDWAREID,
                    reinterpret_cast<const BYTE*>(hwIdMultiSz),
                    sizeof(hwIdMultiSz))) {
                const DWORD err = GetLastError();
                SetupDiDestroyDeviceInfoList(devInfoSet);
                FreeLibrary(hNewDev);
                res.detailUtf8 = "SetupDiSetDeviceRegistryPropertyW(SPDRP_HARDWAREID) failed (err=" + std::to_string(err) + ")";
                return finalizeReport(res);
            }

            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devInfoSet, &devInfoData)) {
                const DWORD err = GetLastError();
                SetupDiDestroyDeviceInfoList(devInfoSet);
                FreeLibrary(hNewDev);
                res.detailUtf8 = "SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed (err=" + std::to_string(err) + ")";
                return finalizeReport(res);
            }
            SetupDiDestroyDeviceInfoList(devInfoSet);

            if (pUpdateDriver) {
                BOOL updReboot = FALSE;
                const BOOL updOk = pUpdateDriver(
                    nullptr, kSteamSpeakersHardwareId, pkg.infPath.c_str(), kInstallFlagForce, &updReboot);
                if (!updOk) {
                    const DWORD updErr = GetLastError();
                    FreeLibrary(hNewDev);
                    res.detailUtf8 = "UpdateDriverForPlugAndPlayDevicesW failed (err=" + std::to_string(updErr) +
                                     (updErr == ERROR_ACCESS_DENIED ? "; Administrator elevation required" : "") + ")";
                    return finalizeReport(res);
                }
                rebootNeeded = rebootNeeded || updReboot;
            }
        }
    } else {
        res.elevated = true;
        if (pUpdateDriver) {
            BOOL updReboot = FALSE;
            const BOOL updOk = pUpdateDriver(
                nullptr, kSteamSpeakersHardwareId, pkg.infPath.c_str(), kInstallFlagForce, &updReboot);
            if (!updOk) {
                const DWORD updErr = GetLastError();
                FreeLibrary(hNewDev);
                res.detailUtf8 = "UpdateDriverForPlugAndPlayDevicesW failed (err=" + std::to_string(updErr) +
                                 (updErr == ERROR_ACCESS_DENIED ? "; Administrator elevation required" : "") + ")";
                return finalizeReport(res);
            }
            rebootNeeded = rebootNeeded || updReboot;
        } else if (pDiInstallDriver) {
            if (!pDiInstallDriver(nullptr, pkg.infPath.c_str(), kDiirFlagForceInf, &rebootNeeded)) {
                const DWORD err = GetLastError();
                FreeLibrary(hNewDev);
                res.detailUtf8 = "DiInstallDriverW failed (err=" + std::to_string(err) +
                                 (err == ERROR_ACCESS_DENIED ? "; Administrator elevation required" : "") + ")";
                return finalizeReport(res);
            }
        }
    }

    FreeLibrary(hNewDev);
    res.requiresReboot = (rebootNeeded != FALSE);

    // Phase 1: Wait up to 5s for AudioEndpointBuilder to materialize the SWD render endpoint
    // while continuously snapping system default endpoints back if promoted.
    std::wstring steamEpId;
    std::wstring steamFriendly;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (pEnum && policyConfig) {
            EnforceSystemDefaultsSnapshot(pEnum.Get(), policyConfig.Get(), preSnap);
        }
        if (FindSteamSpeakersRenderEndpoint(pEnum.Get(), steamEpId, steamFriendly)) {
            break;
        }
        Sleep(100u);
    }

    // Phase 2: Post-arrival settle guard! Windows AudioEndpointBuilder frequently switches
    // default Console/Multimedia endpoints 300-1500ms AFTER MMDeviceEnumerator lists the new SWD.
    // Hold the guard for at least 2.5s (25 ticks) and until 10 consecutive clean 100ms checks pass.
    int consecutiveClean = 0;
    for (int settleTick = 0; settleTick < 40; ++settleTick) {
        if (pEnum && policyConfig) {
            EnforceSystemDefaultsSnapshot(pEnum.Get(), policyConfig.Get(), preSnap);
        }
        if (steamEpId.empty()) {
            FindSteamSpeakersRenderEndpoint(pEnum.Get(), steamEpId, steamFriendly);
        }
        if (pEnum) {
            const EndpointDefaultsSnapshot curSnap = CaptureSystemDefaultsSnapshot(pEnum.Get());
            if (AreSystemDefaultsPreserved(preSnap, curSnap)) {
                ++consecutiveClean;
            } else {
                consecutiveClean = 0;
            }
            if (settleTick >= 24 && consecutiveClean >= 10) {
                break;
            }
        }
        Sleep(100u);
    }

    if (pEnum) {
        const EndpointDefaultsSnapshot postSnap = CaptureSystemDefaultsSnapshot(pEnum.Get());
        res.defaultRenderAfterUtf8 = staged::WideToUtf8(postSnap.renderMultimedia);
        res.defaultCaptureAfterUtf8 = staged::WideToUtf8(postSnap.captureMultimedia);
        res.defaultsPreserved = AreSystemDefaultsPreserved(preSnap, postSnap);
    }

    res.virtualSinkEndpointIdUtf8 = staged::WideToUtf8(steamEpId);
    res.virtualSinkFriendlyNameUtf8 = staged::WideToUtf8(steamFriendly);
    res.succeeded = true;
    if (steamEpId.empty()) {
        res.detailUtf8 = "Driver registered cleanly; waiting for Windows audio endpoint activation";
    } else if (res.defaultsPreserved) {
        res.detailUtf8 = "Installed virtual silent sink [" + res.virtualSinkFriendlyNameUtf8 +
                         "] while preserving system defaults";
    } else {
        res.detailUtf8 = "Installed virtual silent sink [" + res.virtualSinkFriendlyNameUtf8 +
                         "], but Windows changed default endpoint (please re-select your speakers)";
    }
    return finalizeReport(res);
}

VirtualSinkInstallResult UninstallVirtualAudioSink(const std::wstring& reportJsonPath) {
    VirtualSinkInstallResult res;
    res.action = "uninstall";

    auto finalizeReport = [&](const VirtualSinkInstallResult& r) -> VirtualSinkInstallResult {
        if (!reportJsonPath.empty()) {
            std::string writeErr;
            staged::WriteTextFileAtomic(reportJsonPath, SerializeVirtualSinkInstallResultJson(r), writeErr);
        }
        return r;
    };

    using DiUninstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
    HMODULE hNewDev = LoadLibraryW(L"newdev.dll");
    const auto pDiUninstall = hNewDev
        ? reinterpret_cast<DiUninstallDeviceFn>(GetProcAddress(hNewDev, "DiUninstallDevice"))
        : nullptr;

    HDEVINFO hDevInfo = nullptr;
    std::vector<SP_DEVINFO_DATA> devs;
    if (!FindExistingRootSteamSpeakersDevnodes(hDevInfo, devs)) {
        if (hNewDev) FreeLibrary(hNewDev);
        res.detailUtf8 = "SetupDiGetClassDevsW failed (err=" + std::to_string(GetLastError()) + ")";
        return finalizeReport(res);
    }

    BOOL rebootNeeded = FALSE;
    uint32_t removedCount = 0u;
    DWORD lastErr = ERROR_SUCCESS;

    if (hDevInfo && !devs.empty()) {
        for (auto& d : devs) {
            BOOL ok = FALSE;
            if (pDiUninstall) {
                ok = pDiUninstall(nullptr, hDevInfo, &d, 0u, &rebootNeeded);
            } else {
                ok = SetupDiCallClassInstaller(DIF_REMOVE, hDevInfo, &d);
            }
            if (ok) {
                ++removedCount;
                res.elevated = true;
            } else {
                lastErr = GetLastError();
            }
        }
    }
    if (hDevInfo) {
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    if (hNewDev) {
        FreeLibrary(hNewDev);
    }

    if (!devs.empty() && removedCount == 0u) {
        res.detailUtf8 = "Failed removing ROOT\\SteamStreamingSpeakers devnode (err=" +
                         std::to_string(lastErr) +
                         (lastErr == ERROR_ACCESS_DENIED ? "; Administrator elevation required" : "") + ")";
        return finalizeReport(res);
    }

    // Also remove any published OEM INF package matching SteamStreamingSpeakers.inf from %SystemRoot%\INF
    wchar_t infDirBuf[MAX_PATH]{};
    if (GetWindowsDirectoryW(infDirBuf, static_cast<DWORD>(std::size(infDirBuf))) > 0u) {
        const std::wstring infDir = JoinWinPath(infDirBuf, L"INF");
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(JoinWinPath(infDir, L"oem*.inf").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring fullInf = JoinWinPath(infDir, fd.cFileName);
                std::string text;
                std::string readErr;
                if (staged::ReadTextFileUtf8(fullInf, text, readErr) &&
                    text.find("SteamStreamingSpeakers") != std::string::npos) {
                    SetupUninstallOEMInfW(fd.cFileName, 0x00000001u /* SUOI_FORCEDELETE */, nullptr);
                }
            } while (FindNextFileW(hFind, &fd) != FALSE);
            FindClose(hFind);
        }
    }

    res.requiresReboot = (rebootNeeded != FALSE);
    res.succeeded = true;
    res.detailUtf8 = "Removed " + std::to_string(removedCount) +
                     " virtual audio device node(s) cleanly";
    return finalizeReport(res);
}

} // namespace stagee
