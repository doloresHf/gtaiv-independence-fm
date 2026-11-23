#include <windows.h>

#include "route_b_hooks.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        routeb::InitializeRouteBPlugin(hModule);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        routeb::ShutdownRouteBPlugin(lpReserved != nullptr);
    }
    return TRUE;
}
