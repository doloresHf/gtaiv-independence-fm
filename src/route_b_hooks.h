#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

#include "ipc_ring_protocol.h"
#include "route_b_ctrl_protocol.h"

namespace routeb {

struct RouteBIniConfig {
    bool enabled = true;
    bool observeOnly = true;
    std::wstring mappingName = stagec::kDefaultMappingName;
    std::wstring ctrlMappingName = kDefaultCtrlMappingName;
    std::vector<std::wstring> targetFiles;
    uint32_t steadyLeadMs = 5500u;
    uint32_t heartbeatTimeoutMs = 500u;
    uint32_t fadeMs = 10u;
};

// Loads [RouteB] settings from IVRadioBridge.ini (returns defaults if missing).
RouteBIniConfig LoadIniConfig(const std::wstring& iniPath);

// Initializes logger, INI config, ASLR-safe signature verification, Control SHM, and MinHook detours.
// Note: Called lazily from a worker thread or post-attach entrypoint outside DllMain loader lock where possible.
bool InitializeRouteBPlugin(HMODULE selfModule);

// Unhooks (if !processTerminating) and cleanly flushes Control SHM / Ring consumer / log files.
void ShutdownRouteBPlugin(bool processTerminating);

} // namespace routeb
