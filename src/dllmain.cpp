#include <windows.h>
#include <wmsdk.h>
#include <MinHook.h>

#include "async_logger.h"
#include "stage_b_source.h"
#include "wmf_wrappers.h"

namespace {

using PFN_WMCreateSyncReader = HRESULT(STDMETHODCALLTYPE*)(IUnknown* pUnkCert, DWORD dwRights, IWMSyncReader** ppSyncReader);
using PFN_WMCreateReader     = HRESULT(STDMETHODCALLTYPE*)(IUnknown* pUnkCert, DWORD dwRights, IWMReader** ppReader);
using PFN_WMCreateEditor     = HRESULT(STDMETHODCALLTYPE*)(IWMMetadataEditor** ppEditor);

PFN_WMCreateSyncReader g_origCreateSyncReader = nullptr;
PFN_WMCreateReader     g_origCreateReader     = nullptr;
PFN_WMCreateEditor     g_origCreateEditor     = nullptr;

HRESULT STDMETHODCALLTYPE Detour_WMCreateSyncReader(IUnknown* pUnkCert, DWORD dwRights, IWMSyncReader** ppSyncReader) {
    HRESULT hr = g_origCreateSyncReader(pUnkCert, dwRights, ppSyncReader);
    probe::AsyncLogger::Instance().LogText("WMCreateSyncReader(cert=%p, rights=0x%lX) -> hr=0x%08lX",
                                           pUnkCert, static_cast<unsigned long>(dwRights), static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && ppSyncReader && *ppSyncReader) {
        IWMSyncReader* realReader = *ppSyncReader;
        *ppSyncReader = new probe::WrappedSyncReader(realReader);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Detour_WMCreateReader(IUnknown* pUnkCert, DWORD dwRights, IWMReader** ppReader) {
    HRESULT hr = g_origCreateReader(pUnkCert, dwRights, ppReader);
    probe::AsyncLogger::Instance().LogText("WMCreateReader(cert=%p, rights=0x%lX) -> hr=0x%08lX",
                                           pUnkCert, static_cast<unsigned long>(dwRights), static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && ppReader && *ppReader) {
        IWMReader* realReader = *ppReader;
        *ppReader = new probe::WrappedAsyncReader(realReader);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Detour_WMCreateEditorImpl(IWMMetadataEditor** ppEditor) {
    HRESULT hr = g_origCreateEditor(ppEditor);
    probe::AsyncLogger::Instance().LogText("WMCreateEditor() -> hr=0x%08lX", static_cast<unsigned long>(hr));
    probe::AsyncLogger::Instance().LogJsonEvent("WMCreateEditor", 0, "editor", "\"hr\":\"0x%08lX\"", static_cast<unsigned long>(hr));
    return hr;
}

bool InstallWmfHooks() {
    if (MH_Initialize() != MH_OK) {
        probe::AsyncLogger::Instance().LogText("[ERROR] MH_Initialize failed");
        return false;
    }

    // WMVCore.dll is already loaded because both GTAIV.exe and IVRadioProbe.asi link against wmvcore.lib
    HMODULE hWmvCore = GetModuleHandleW(L"wmvcore.dll");
    if (!hWmvCore) {
        probe::AsyncLogger::Instance().LogText("[ERROR] GetModuleHandleW(wmvcore.dll) returned NULL");
        return false;
    }

    auto* pSync   = GetProcAddress(hWmvCore, "WMCreateSyncReader");
    auto* pAsync  = GetProcAddress(hWmvCore, "WMCreateReader");
    auto* pEditor = GetProcAddress(hWmvCore, "WMCreateEditor");

    probe::AsyncLogger::Instance().LogText("wmvcore.dll=%p | WMCreateSyncReader=%p | WMCreateReader=%p | WMCreateEditor=%p",
                                           hWmvCore, pSync, pAsync, pEditor);

    if (pSync) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(pSync),
                                     reinterpret_cast<LPVOID>(&Detour_WMCreateSyncReader),
                                     reinterpret_cast<LPVOID*>(&g_origCreateSyncReader));
        probe::AsyncLogger::Instance().LogText("Hook WMCreateSyncReader status=%d", static_cast<int>(st));
    }
    if (pAsync) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(pAsync),
                                     reinterpret_cast<LPVOID>(&Detour_WMCreateReader),
                                     reinterpret_cast<LPVOID*>(&g_origCreateReader));
        probe::AsyncLogger::Instance().LogText("Hook WMCreateReader status=%d", static_cast<int>(st));
    }
    if (pEditor) {
        MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(pEditor),
                                     reinterpret_cast<LPVOID>(&Detour_WMCreateEditorImpl),
                                     reinterpret_cast<LPVOID*>(&g_origCreateEditor));
        probe::AsyncLogger::Instance().LogText("Hook WMCreateEditor status=%d", static_cast<int>(st));
    }

    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    probe::AsyncLogger::Instance().LogText("MH_EnableHook(MH_ALL_HOOKS) status=%d", static_cast<int>(en));
    probe::AsyncLogger::Instance().LogJsonEvent("Probe_Initialized", 0, "global",
                                                "\"mh_enable\":%d", static_cast<int>(en));
    return en == MH_OK;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        probe::StageBManager::Instance().SetModuleHandle(hModule);
        probe::AsyncLogger::Instance().Init(hModule);

        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::string exeUtf8 = probe::AsyncLogger::WideToUtf8Escaped(exePath);
        probe::AsyncLogger::Instance().LogText("=== IVRadioProbe Stage A attached to %s (PID=%lu) ===",
                                               exeUtf8.c_str(), static_cast<unsigned long>(GetCurrentProcessId()));
        probe::AsyncLogger::Instance().LogJsonEvent("Process_Attach", 0, "global",
                                                    "\"exe\":\"%s\",\"pid\":%lu",
                                                    exeUtf8.c_str(), static_cast<unsigned long>(GetCurrentProcessId()));
        InstallWmfHooks();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        probe::AsyncLogger::Instance().LogText("=== IVRadioProbe Stage A detaching (lpReserved=%p) ===", lpReserved);
        probe::AsyncLogger::Instance().LogJsonEvent("Process_Detach", 0, "global",
                                                    "\"process_terminating\":%s", lpReserved ? "true" : "false");
        if (lpReserved == nullptr) {
            // Dynamic FreeLibrary (e.g. wmf_selftest): safe to unhook cleanly
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            probe::StageBManager::Instance().Shutdown();
        }
        probe::AsyncLogger::Instance().Shutdown();
    }
    return TRUE;
}
