#include "stage_e_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <map>
#include <set>

namespace stagee {

namespace {

double ComputePeakDbfs(int32_t peakAbs) noexcept {
    if (peakAbs <= 0) {
        return -144.0;
    }
    const double norm = std::min(1.0, static_cast<double>(peakAbs) / 32768.0);
    const double db = 20.0 * std::log10(norm);
    if (!std::isfinite(db)) {
        return -144.0;
    }
    return std::clamp(db, -144.0, 0.0);
}

double ComputeRmsDbfs(uint64_t sumSquares, uint64_t stereoFrames) noexcept {
    if (stereoFrames == 0ULL || sumSquares == 0ULL) {
        return -144.0;
    }
    const double denom = static_cast<double>(stereoFrames) * 2.0 * 32768.0 * 32768.0;
    const double meanSq = std::min(1.0, static_cast<double>(sumSquares) / denom);
    if (meanSq <= 0.0) {
        return -144.0;
    }
    const double db = 10.0 * std::log10(meanSq);
    if (!std::isfinite(db)) {
        return -144.0;
    }
    return std::clamp(db, -144.0, 0.0);
}

double ClampFiniteJsonDb(double db) noexcept {
    if (!std::isfinite(db)) {
        return -144.0;
    }
    return std::clamp(db, -144.0, 0.0);
}

bool ContainsAsciiCaseInsensitive(const std::wstring& haystack, const wchar_t* needle) {
    if (!needle || !*needle) {
        return true;
    }
    std::wstring hLower = haystack;
    std::wstring nLower = needle;
    auto toLowerCh = [](wchar_t ch) -> wchar_t {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    };
    std::transform(hLower.begin(), hLower.end(), hLower.begin(), toLowerCh);
    std::transform(nLower.begin(), nLower.end(), nLower.begin(), toLowerCh);
    return hLower.find(nLower) != std::wstring::npos;
}

bool IsAppleMusicStorePathHint(const std::wstring& fullPath) {
    return ContainsAsciiCaseInsensitive(fullPath, L"AppleInc.AppleMusicWin");
}

std::wstring GetSessionExeLeaf(const staged::AudioSessionRecord& sess) {
    if (!sess.exeName.empty()) {
        return sess.exeName;
    }
    return staged::ExtractLeafFilename(sess.exeFullPath);
}

std::wstring GetProcessExeLeaf(const staged::ProcessEntry& proc) {
    if (!proc.exeName.empty()) {
        return proc.exeName;
    }
    return staged::ExtractLeafFilename(proc.exeFullPath);
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

bool FileExistsAt(const std::wstring& path) noexcept {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0u);
}

bool DirectoryExistsAt(const std::wstring& path) noexcept {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0u);
}

std::wstring StripTrailingSlashes(const std::wstring& path) {
    std::wstring res = path;
    while (res.size() > 3u && (res.back() == L'\\' || res.back() == L'/')) {
        res.pop_back();
    }
    return res;
}

std::wstring ParentDirectoryOf(const std::wstring& path) {
    const std::wstring clean = StripTrailingSlashes(path);
    const size_t pos = clean.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L"";
    }
    if (pos == 2u && clean.size() >= 3u && clean[1] == L':') {
        return clean.substr(0u, 3u); // Preserve drive root e.g. "E:\"
    }
    return clean.substr(0u, pos);
}

std::wstring JoinWinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) {
        return leaf;
    }
    if (dir.back() == L'\\' || dir.back() == L'/') {
        return dir + leaf;
    }
    return dir + L"\\" + leaf;
}

bool EnsureDirectoryRecursive(const std::wstring& dirPath) {
    if (dirPath.empty()) {
        return false;
    }
    const std::wstring clean = StripTrailingSlashes(dirPath);
    if (DirectoryExistsAt(clean)) {
        return true;
    }
    const std::wstring parent = ParentDirectoryOf(clean);
    if (!parent.empty() && parent != clean && !DirectoryExistsAt(parent)) {
        if (!EnsureDirectoryRecursive(parent)) {
            return false;
        }
    }
    if (CreateDirectoryW(clean.c_str(), nullptr) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS && DirectoryExistsAt(clean);
}

bool IsAsciiDigitW(wchar_t ch) noexcept {
    return ch >= L'0' && ch <= L'9';
}

bool EndsWithIgnoreCaseW(const std::wstring& s, const std::wstring& suffix) noexcept {
    if (s.size() < suffix.size()) {
        return false;
    }
    return staged::EqualsIgnoreCase(s.substr(s.size() - suffix.size()), suffix);
}

std::string TruncateUtf8Safe(const std::string& utf8, size_t maxBytes) {
    if (utf8.size() <= maxBytes) {
        return utf8;
    }
    size_t cut = maxBytes;
    while (cut > 0u && (static_cast<unsigned char>(utf8[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return utf8.substr(0u, cut);
}

} // namespace

// ============================================================================
// Section A0: UI Language & Persistent Launcher Settings
// ============================================================================

const char* UiLanguageToCode(UiLanguage lang) noexcept {
    switch (lang) {
        case UiLanguage::Auto: return "auto";
        case UiLanguage::ZhCn: return "zh";
        case UiLanguage::EnUs: return "en";
    }
    return "auto";
}

bool ParseUiLanguageCode(const std::string& codeUtf8, UiLanguage& outLang) noexcept {
    std::string lower;
    lower.reserve(codeUtf8.size());
    for (unsigned char ch : codeUtf8) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            lower.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lower.push_back(static_cast<char>(ch));
        }
    }
    if (lower == "auto" || lower == "system" || lower == "default") {
        outLang = UiLanguage::Auto;
        return true;
    }
    if (lower == "zh" || lower == "zh-cn" || lower == "zh_cn" || lower == "chinese" || lower == "cn") {
        outLang = UiLanguage::ZhCn;
        return true;
    }
    if (lower == "en" || lower == "en-us" || lower == "en_us" || lower == "english") {
        outLang = UiLanguage::EnUs;
        return true;
    }
    return false;
}

UiLanguage ResolveEffectiveLanguage(UiLanguage preference) noexcept {
    if (preference == UiLanguage::ZhCn || preference == UiLanguage::EnUs) {
        return preference;
    }
    const LANGID langId = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(langId) == LANG_CHINESE) ? UiLanguage::ZhCn : UiLanguage::EnUs;
}

const char* AudioSourceModeToCode(AudioSourceMode mode) noexcept {
    switch (mode) {
        case AudioSourceMode::AppleMusicPreset: return "apple_music";
        case AudioSourceMode::CustomProcess:    return "custom_exe";
    }
    return "apple_music";
}

bool ParseAudioSourceModeCode(const std::string& codeUtf8, AudioSourceMode& outMode) noexcept {
    std::string lower;
    lower.reserve(codeUtf8.size());
    for (unsigned char ch : codeUtf8) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            lower.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lower.push_back(static_cast<char>(ch));
        }
    }
    if (lower == "apple_music" || lower == "applemusic" || lower == "default" || lower == "preset") {
        outMode = AudioSourceMode::AppleMusicPreset;
        return true;
    }
    if (lower == "custom_exe" || lower == "custom" || lower == "exe" || lower == "stream_anything") {
        outMode = AudioSourceMode::CustomProcess;
        return true;
    }
    return false;
}

std::string FormatAudioSourceDisplayNameUtf8(const AudioSourceConfig& src) {
    if (src.mode == AudioSourceMode::AppleMusicPreset) {
        return "Apple Music";
    }
    const std::wstring leaf = !src.customExeName.empty()
        ? staged::ExtractLeafFilename(src.customExeName)
        : staged::ExtractLeafFilename(src.customExePath);
    if (leaf.empty()) {
        return "Custom App";
    }
    return staged::WideToUtf8(leaf);
}

bool IsForbiddenCaptureTargetExe(const std::wstring& exeNameOrPath) noexcept {
    const std::wstring leaf = staged::ExtractLeafFilename(exeNameOrPath);
    if (leaf.empty()) {
        return true;
    }
    static const wchar_t* const kForbiddenLeaves[] = {
        // Game binaries (capturing GTA IV feeds radio output back into radio -> 8s howling loop!)
        L"GTAIV.exe",
        L"EFLC.exe",
        L"LaunchGTAIV.exe",
        L"PlayGTAIV.exe",
        // Bridge / producer / test binaries
        L"iv_apple_music_radio.exe",
        L"iv_stage_c_producer.exe",
        L"iv_stage_d_producer.exe",
        L"iv_stage_d_probe.exe",
        L"stage_e_offline_selftest.exe",
        L"stage_d_offline_selftest.exe",
        L"stage_d_bridge_selftest.exe",
        L"ipc_protocol_selftest.exe",
        L"wmf_selftest.exe",
        // System host / shell trees (INCLUDE_TARGET_PROCESS_TREE on explorer.exe/svchost.exe
        // would capture child desktop apps including GTAIV.exe or system audio daemon)
        L"audiodg.exe",
        L"svchost.exe",
        L"explorer.exe",
        L"System",
        L"Registry",
        L"Idle",
        L"csrss.exe",
        L"wininit.exe",
        L"services.exe",
        L"lsass.exe",
        L"dwm.exe",
    };
    for (const wchar_t* item : kForbiddenLeaves) {
        if (staged::EqualsIgnoreCase(leaf, item)) {
            return true;
        }
    }
    return false;
}

bool IsAppleMusicShellOrAgentExe(const std::wstring& exeNameOrPath) noexcept {
    const std::wstring leaf = staged::ExtractLeafFilename(exeNameOrPath);
    return staged::EqualsIgnoreCase(leaf, L"AppleMusic.exe") ||
           staged::EqualsIgnoreCase(leaf, L"AMPLibraryAgent.exe");
}

uint32_t ResolveSameImageRootPid(const std::vector<staged::ProcessEntry>& processes,
                                 uint32_t candidatePid) {
    if (candidatePid == 0u || processes.empty()) {
        return candidatePid;
    }

    std::map<uint32_t, const staged::ProcessEntry*> byPid;
    for (const auto& p : processes) {
        if (p.pid != 0u) {
            byPid[p.pid] = &p;
        }
    }

    auto it = byPid.find(candidatePid);
    if (it == byPid.end()) {
        return candidatePid;
    }

    const std::wstring targetLeaf = GetProcessExeLeaf(*it->second);
    if (targetLeaf.empty()) {
        return candidatePid;
    }

    uint32_t curPid = candidatePid;
    std::set<uint32_t> visited;
    visited.insert(curPid);

    for (size_t depth = 0u; depth < 64u; ++depth) {
        auto curIt = byPid.find(curPid);
        if (curIt == byPid.end()) {
            break;
        }
        const uint32_t parentPid = curIt->second->parentPid;
        if (parentPid == 0u || parentPid == curPid || visited.count(parentPid) > 0u) {
            break;
        }
        auto parentIt = byPid.find(parentPid);
        if (parentIt == byPid.end()) {
            break;
        }
        const std::wstring parentLeaf = GetProcessExeLeaf(*parentIt->second);
        if (!staged::EqualsIgnoreCase(parentLeaf, targetLeaf)) {
            break;
        }
        visited.insert(parentPid);
        curPid = parentPid;
    }

    return curPid;
}

std::vector<SelectableAudioApp> EnumerateSelectableAudioApps(const staged::AudioTopologyReport& topo) {
    std::map<uint32_t, const staged::ProcessEntry*> procByPid;
    for (const auto& p : topo.processes) {
        if (p.pid != 0u) {
            procByPid[p.pid] = &p;
        }
    }

    struct AggKey {
        std::wstring lowerLeaf;
        uint32_t rootPid = 0u;
        bool operator<(const AggKey& o) const {
            if (lowerLeaf != o.lowerLeaf) return lowerLeaf < o.lowerLeaf;
            return rootPid < o.rootPid;
        }
    };

    std::map<AggKey, SelectableAudioApp> aggregated;

    for (const auto& ep : topo.endpoints) {
        for (const auto& sess : ep.sessions) {
            if (sess.isSystemSoundsSession || sess.isMultiProcessSession || sess.processId == 0u) {
                continue;
            }
            if (sess.sessionState != AudioSessionStateActive &&
                sess.sessionState != AudioSessionStateInactive) {
                continue;
            }

            std::wstring leaf = GetSessionExeLeaf(sess);
            if (leaf.empty()) {
                auto pIt = procByPid.find(sess.processId);
                if (pIt != procByPid.end()) {
                    leaf = GetProcessExeLeaf(*pIt->second);
                }
            }
            if (leaf.empty() || IsForbiddenCaptureTargetExe(leaf)) {
                continue;
            }

            const uint32_t rootPid = ResolveSameImageRootPid(topo.processes, sess.processId);
            std::wstring fullPath = sess.exeFullPath;
            auto rootIt = procByPid.find(rootPid);
            if (rootIt != procByPid.end()) {
                const std::wstring rootLeaf = GetProcessExeLeaf(*rootIt->second);
                if (!rootLeaf.empty()) {
                    leaf = rootLeaf;
                }
                if (!rootIt->second->exeFullPath.empty()) {
                    fullPath = rootIt->second->exeFullPath;
                }
            }

            const bool isActive = (sess.sessionState == AudioSessionStateActive);
            std::wstring cleanDisp = sess.displayName;
            if (!cleanDisp.empty() &&
                (cleanDisp[0] == L'@' || staged::EqualsIgnoreCase(cleanDisp, leaf))) {
                cleanDisp.clear();
            }

            const AggKey key{ ToLowerAsciiW(leaf), rootPid };
            auto [it, inserted] = aggregated.emplace(key, SelectableAudioApp{});
            SelectableAudioApp& entry = it->second;
            if (inserted) {
                entry.rootPid = rootPid;
                entry.exeName = leaf;
                entry.exeFullPath = fullPath;
                entry.sessionDisplayName = cleanDisp;
                entry.endpointName = ep.friendlyName;
                entry.hasActiveSession = isActive;
                entry.isAppleMusicFamily = IsAppleMusicShellOrAgentExe(leaf);
            } else {
                if (entry.exeFullPath.empty() && !fullPath.empty()) {
                    entry.exeFullPath = fullPath;
                }
                if (entry.sessionDisplayName.empty() && !cleanDisp.empty()) {
                    entry.sessionDisplayName = cleanDisp;
                }
                if (isActive && !entry.hasActiveSession) {
                    entry.hasActiveSession = true;
                    entry.endpointName = ep.friendlyName;
                    if (!cleanDisp.empty()) {
                        entry.sessionDisplayName = cleanDisp;
                    }
                }
            }
        }
    }

    std::vector<SelectableAudioApp> result;
    result.reserve(aggregated.size());
    for (auto& kv : aggregated) {
        result.push_back(std::move(kv.second));
    }

    std::sort(result.begin(), result.end(), [](const SelectableAudioApp& a, const SelectableAudioApp& b) {
        if (a.hasActiveSession != b.hasActiveSession) {
            return a.hasActiveSession > b.hasActiveSession; // Playing apps first
        }
        const std::wstring la = ToLowerAsciiW(a.exeName);
        const std::wstring lb = ToLowerAsciiW(b.exeName);
        if (la != lb) {
            return la < lb;
        }
        return a.rootPid < b.rootPid;
    });

    return result;
}

std::wstring ResolveDefaultSettingsFilePath(const std::wstring& exePathOverride) {
    std::wstring exePath = exePathOverride;
    if (exePath.empty()) {
        wchar_t modBuf[MAX_PATH * 4]{};
        const DWORD len = GetModuleFileNameW(nullptr, modBuf, static_cast<DWORD>(std::size(modBuf)));
        if (len > 0u && len < std::size(modBuf)) {
            exePath.assign(modBuf, len);
        }
    }
    const std::wstring exeDir = ParentDirectoryOf(exePath);
    if (exeDir.empty()) {
        return L"iv_apple_music_radio.settings.json";
    }
    return JoinWinPath(exeDir, L"iv_apple_music_radio.settings.json");
}

namespace {

bool ExtractJsonStringFieldSimple(const std::string& json,
                                  const char* fieldName,
                                  std::string& outValue,
                                  std::string& outError) {
    const std::string keyToken = std::string("\"") + fieldName + "\"";
    const size_t keyPos = json.find(keyToken);
    if (keyPos == std::string::npos) {
        return false; // Field optional / absent
    }
    size_t pos = keyPos + keyToken.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') {
        outError = std::string("Malformed JSON colon after ") + keyToken;
        return false;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        outError = std::string("Expected JSON string value for ") + keyToken;
        return false;
    }
    ++pos;
    std::string rawEscaped;
    bool closed = false;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '\\') {
            if (pos >= json.size()) {
                break;
            }
            rawEscaped.push_back('\\');
            rawEscaped.push_back(json[pos++]);
        } else if (c == '"') {
            closed = true;
            break;
        } else {
            rawEscaped.push_back(c);
        }
    }
    if (!closed) {
        outError = std::string("Unterminated JSON string for ") + keyToken;
        return false;
    }
    return staged::UnescapeJsonString(rawEscaped, outValue, outError);
}

} // namespace

bool LoadLauncherSettings(const std::wstring& filePath,
                          LauncherSettings& outSettings,
                          std::string& outError) {
    outSettings = LauncherSettings{};
    outError.clear();

    if (filePath.empty() || !FileExistsAt(filePath)) {
        return true; // Default settings on first run
    }

    std::string content;
    if (!staged::ReadTextFileUtf8(filePath, content, outError)) {
        return false;
    }
    if (content.find('{') == std::string::npos || content.find('}') == std::string::npos) {
        outError = "Settings file is not a valid JSON object";
        return false;
    }

    std::string langVal;
    std::string parseErr;
    if (ExtractJsonStringFieldSimple(content, "language", langVal, parseErr)) {
        UiLanguage parsedLang = UiLanguage::Auto;
        if (!ParseUiLanguageCode(langVal, parsedLang)) {
            outError = "Unsupported 'language' value in settings: " + langVal;
            return false;
        }
        outSettings.language = parsedLang;
    } else if (!parseErr.empty()) {
        outError = parseErr;
        return false;
    }

    std::string modeVal;
    parseErr.clear();
    if (ExtractJsonStringFieldSimple(content, "source_mode", modeVal, parseErr)) {
        AudioSourceMode parsedMode = AudioSourceMode::AppleMusicPreset;
        if (!ParseAudioSourceModeCode(modeVal, parsedMode)) {
            outError = "Unsupported 'source_mode' value in settings: " + modeVal;
            return false;
        }
        outSettings.source.mode = parsedMode;
    } else if (!parseErr.empty()) {
        outError = parseErr;
        return false;
    }

    std::string exeNameUtf8;
    parseErr.clear();
    if (ExtractJsonStringFieldSimple(content, "custom_exe_name", exeNameUtf8, parseErr)) {
        outSettings.source.customExeName = staged::ExtractLeafFilename(staged::Utf8ToWide(exeNameUtf8));
    } else if (!parseErr.empty()) {
        outError = parseErr;
        return false;
    }

    std::string exePathUtf8;
    parseErr.clear();
    if (ExtractJsonStringFieldSimple(content, "custom_exe_path", exePathUtf8, parseErr)) {
        outSettings.source.customExePath = staged::Utf8ToWide(exePathUtf8);
    } else if (!parseErr.empty()) {
        outError = parseErr;
        return false;
    }

    if (outSettings.source.mode == AudioSourceMode::CustomProcess) {
        if (outSettings.source.customExeName.empty() && !outSettings.source.customExePath.empty()) {
            outSettings.source.customExeName = staged::ExtractLeafFilename(outSettings.source.customExePath);
        }
        if (outSettings.source.customExeName.empty()) {
            outError = "Settings 'custom_exe' mode requires a non-empty 'custom_exe_name'";
            return false;
        }
        if (IsForbiddenCaptureTargetExe(outSettings.source.customExeName)) {
            outError = "Forbidden capture target in settings: " +
                       staged::WideToUtf8(outSettings.source.customExeName);
            return false;
        }
        if (IsAppleMusicShellOrAgentExe(outSettings.source.customExeName)) {
            outSettings.source.mode = AudioSourceMode::AppleMusicPreset;
            outSettings.source.customExeName.clear();
            outSettings.source.customExePath.clear();
        }
    }

    return true;
}

bool SaveLauncherSettings(const std::wstring& filePath,
                          const LauncherSettings& settings,
                          std::string& outError) {
    outError.clear();
    if (filePath.empty()) {
        outError = "Settings file path is empty";
        return false;
    }
    const std::wstring dir = ParentDirectoryOf(filePath);
    if (!dir.empty() && !EnsureDirectoryRecursive(dir)) {
        outError = "Failed creating parent directory for settings file";
        return false;
    }

    const std::wstring leafExe = settings.source.customExeName.empty()
        ? staged::ExtractLeafFilename(settings.source.customExePath)
        : staged::ExtractLeafFilename(settings.source.customExeName);

    std::string json = "{\n";
    json += "  \"version\": " + std::to_string(settings.version) + ",\n";
    json += "  \"language\": \"" + staged::EscapeJsonString(UiLanguageToCode(settings.language)) + "\",\n";
    json += "  \"source_mode\": \"" + staged::EscapeJsonString(AudioSourceModeToCode(settings.source.mode)) + "\",\n";
    json += "  \"custom_exe_name\": \"" + staged::EscapeJsonString(staged::WideToUtf8(leafExe)) + "\",\n";
    json += "  \"custom_exe_path\": \"" + staged::EscapeJsonString(staged::WideToUtf8(settings.source.customExePath)) + "\"\n";
    json += "}\n";

    return staged::WriteTextFileAtomic(filePath, json, outError);
}

// ============================================================================
// Section A: RollingLevelMeter Implementation
// ============================================================================

RollingLevelMeter::RollingLevelMeter() noexcept {
    Reset(0ULL);
}

void RollingLevelMeter::Reset(uint64_t originQpc100ns) noexcept {
    initialized_ = true;
    originQpc100ns_ = originQpc100ns;
    currentWallQpc100ns_ = originQpc100ns;
    currentSlot_ = 0ULL;
    hasAnyPacket_ = false;
    lastPacketWallQpc100ns_ = 0ULL;
    hasAnyAudible_ = false;
    lastAudibleWallQpc100ns_ = 0ULL;
    for (auto& b : buckets_) {
        b = Bucket{};
    }
}

void RollingLevelMeter::AdvanceTo(uint64_t nowQpc100ns) noexcept {
    if (!initialized_) {
        Reset(nowQpc100ns);
        return;
    }
    if (nowQpc100ns < currentWallQpc100ns_) {
        nowQpc100ns = currentWallQpc100ns_;
    }
    currentWallQpc100ns_ = nowQpc100ns;

    const uint64_t targetSlot = (nowQpc100ns >= originQpc100ns_)
        ? ((nowQpc100ns - originQpc100ns_) / kLevelBucketDuration100ns)
        : 0ULL;
    if (targetSlot <= currentSlot_) {
        return;
    }

    const uint64_t deltaSlots = targetSlot - currentSlot_;
    if (deltaSlots >= kLevelBuckets5s) {
        for (auto& b : buckets_) {
            b = Bucket{};
        }
    } else {
        for (uint64_t s = currentSlot_ + 1ULL; s <= targetSlot; ++s) {
            buckets_[static_cast<size_t>(s % kLevelBuckets5s)] = Bucket{};
        }
    }
    currentSlot_ = targetSlot;
}

void RollingLevelMeter::IngestStereoInt16(uint64_t nowQpc100ns,
                                          const int16_t* stereo48kLR,
                                          uint32_t frameCount,
                                          bool silentFlag) noexcept {
    AdvanceTo(nowQpc100ns);
    hasAnyPacket_ = true;
    lastPacketWallQpc100ns_ = currentWallQpc100ns_;

    if (frameCount == 0u) {
        return;
    }

    Bucket& cur = buckets_[static_cast<size_t>(currentSlot_ % kLevelBuckets5s)];
    cur.frames += frameCount;

    if (silentFlag || !stereo48kLR) {
        cur.silentFlagFrames += frameCount;
        return;
    }

    uint64_t chunkNonZero = 0ULL;
    uint64_t chunkAudible = 0ULL;
    int32_t chunkPeak = 0;
    uint64_t chunkSq = 0ULL;

    for (uint32_t i = 0u; i < frameCount; ++i) {
        const int32_t sL = static_cast<int32_t>(stereo48kLR[i * 2u]);
        const int32_t sR = static_cast<int32_t>(stereo48kLR[i * 2u + 1u]);
        const int32_t absL = (sL < 0) ? -sL : sL;
        const int32_t absR = (sR < 0) ? -sR : sR;
        const int32_t fPeak = (absL >= absR) ? absL : absR;

        if (fPeak > chunkPeak) {
            chunkPeak = fPeak;
        }
        if (fPeak > 0) {
            ++chunkNonZero;
        }
        if (fPeak > kNearZeroPeakThreshold) {
            ++chunkAudible;
        }

        const int64_t sqL = static_cast<int64_t>(sL) * static_cast<int64_t>(sL);
        const int64_t sqR = static_cast<int64_t>(sR) * static_cast<int64_t>(sR);
        chunkSq += static_cast<uint64_t>(sqL + sqR);
    }

    cur.nonZeroFrames += chunkNonZero;
    cur.audibleFrames += chunkAudible;
    if (chunkPeak > cur.peakAbs) {
        cur.peakAbs = chunkPeak;
    }
    cur.sumSquares += chunkSq;

    if (chunkAudible > 0ULL) {
        hasAnyAudible_ = true;
        lastAudibleWallQpc100ns_ = currentWallQpc100ns_;
    }
}

PcmLevelSnapshot RollingLevelMeter::Snapshot(uint64_t nowQpc100ns) noexcept {
    AdvanceTo(nowQpc100ns);

    PcmLevelSnapshot out;
    out.nowQpc100ns = currentWallQpc100ns_;
    out.hasAnyPacket = hasAnyPacket_;
    if (hasAnyPacket_ && currentWallQpc100ns_ >= lastPacketWallQpc100ns_) {
        const uint64_t deltaPacket100ns = currentWallQpc100ns_ - lastPacketWallQpc100ns_;
        out.hasRecentPackets = (deltaPacket100ns <= kRecentPacketWindow100ns);
        out.msSinceLastPacket = deltaPacket100ns / 10000ULL;
    }
    if (hasAnyAudible_ && currentWallQpc100ns_ >= lastAudibleWallQpc100ns_) {
        out.msSinceLastAudible = (currentWallQpc100ns_ - lastAudibleWallQpc100ns_) / 10000ULL;
    }

    uint64_t sumSq1s = 0ULL;
    uint64_t sumSq5s = 0ULL;

    for (size_t age = 0u; age < kLevelBuckets5s; ++age) {
        if (age > currentSlot_) {
            break;
        }
        const uint64_t slot = currentSlot_ - age;
        const Bucket& b = buckets_[static_cast<size_t>(slot % kLevelBuckets5s)];

        out.frames5s += b.frames;
        out.nonZeroFrames5s += b.nonZeroFrames;
        out.audibleFrames5s += b.audibleFrames;
        if (b.peakAbs > out.peakAbs5s) {
            out.peakAbs5s = b.peakAbs;
        }
        sumSq5s += b.sumSquares;

        if (age < kLevelBuckets1s) {
            out.frames1s += b.frames;
            out.nonZeroFrames1s += b.nonZeroFrames;
            out.audibleFrames1s += b.audibleFrames;
            out.silentFlagFrames1s += b.silentFlagFrames;
            if (b.peakAbs > out.peakAbs1s) {
                out.peakAbs1s = b.peakAbs;
            }
            sumSq1s += b.sumSquares;
        }

        if (age < 2u) {
            out.frames100ms += b.frames;
            out.audibleFrames100ms += b.audibleFrames;
            if (b.peakAbs > out.peakAbs100ms) {
                out.peakAbs100ms = b.peakAbs;
            }
        }
    }

    out.peakDbfs1s = ComputePeakDbfs(out.peakAbs1s);
    out.rmsDbfs1s = ComputeRmsDbfs(sumSq1s, out.frames1s);
    out.peakDbfs5s = ComputePeakDbfs(out.peakAbs5s);
    out.rmsDbfs5s = ComputeRmsDbfs(sumSq5s, out.frames5s);
    return out;
}

// ============================================================================
// Section B: Split-Endpoint Audio Routing & Session Isolation Classifier
// ============================================================================

const char* RoutingIsolationStateName(RoutingIsolationState state) noexcept {
    switch (state) {
        case RoutingIsolationState::Unknown: return "unknown";
        case RoutingIsolationState::SingleEndpointOnly: return "single_endpoint_only";
        case RoutingIsolationState::WaitingForTargetSession: return "waiting_for_target_session";
        case RoutingIsolationState::IsolatedOk: return "isolated_ok";
        case RoutingIsolationState::ConflictOnDefaultOutput: return "conflict_on_default_output";
        case RoutingIsolationState::SecondaryEndpointLostFallback: return "secondary_endpoint_lost_fallback";
    }
    return "unknown";
}

void FormatRoutingDiagnosticText(RoutingDiagnostic& out, UiLanguage lang) {
    const bool en = (ResolveEffectiveLanguage(lang) == UiLanguage::EnUs);
    const std::string appDisp = out.targetDisplayNameUtf8.empty()
        ? std::string("Apple Music")
        : out.targetDisplayNameUtf8;
    const std::string defEpDisp = out.defaultEndpointNameUtf8.empty()
        ? (en ? std::string("<No Default Output Device>") : std::string("<未检测到主输出设备>"))
        : out.defaultEndpointNameUtf8;

    switch (out.state) {
        case RoutingIsolationState::IsolatedOk:
            if (out.targetSessionActive) {
                out.bannerTitleUtf8 = en
                    ? "Split-Endpoint Isolation Active (No Desktop Echo)"
                    : "路由隔离正常（无桌面回音）";
            } else {
                out.bannerTitleUtf8 = en
                    ? "Split-Endpoint Isolation Ready (Paused)"
                    : "路由隔离已就绪（当前暂停）";
            }
            out.bannerDetailUtf8 = en
                ? (appDisp + " -> [" + out.targetEndpointNameUtf8 + "]  |  Main Output -> [" + defEpDisp + "]")
                : (appDisp + " -> [" + out.targetEndpointNameUtf8 + "]  |  主输出 -> [" + defEpDisp + "]");
            return;

        case RoutingIsolationState::SecondaryEndpointLostFallback:
            if (out.targetHasSession && out.targetSessionActive) {
                out.bannerTitleUtf8 = en
                    ? ("Secondary Audio Device Disconnected — " + appDisp + " Fell Back to Main Output!")
                    : ("副音频设备已断开，" + appDisp + " 已回落至主输出！");
                out.bannerDetailUtf8 = en
                    ? ("Secondary device [" + out.lostEndpointNameUtf8 + "] disconnected; audio is now playing directly on [" +
                       out.targetEndpointNameUtf8 + "] (~8s echo). Stopping this bridge cannot mute direct desktop output — "
                       "reconnect the secondary device and click 'Volume Mixer' to route it back.")
                    : ("原副设备 [" + out.lostEndpointNameUtf8 + "] 已掉线，当前直出到 [" +
                       out.targetEndpointNameUtf8 + "]（会产生约 8s 延迟双声）。"
                       "暂停本工具无法阻止主输出直出；请重插副设备并点击『打开音量合成器』切回。");
            } else if (out.targetHasSession) {
                out.bannerTitleUtf8 = en
                    ? ("Secondary Audio Device Disconnected — " + appDisp + " Session on Main Output!")
                    : ("副音频设备已断开，" + appDisp + " 会话位于主输出！");
                out.bannerDetailUtf8 = en
                    ? ("Secondary device [" + out.lostEndpointNameUtf8 + "] disconnected; resuming playback on [" +
                       out.targetEndpointNameUtf8 + "] will cause double audio. Reconnect the secondary device and check Volume Mixer.")
                    : ("原副设备 [" + out.lostEndpointNameUtf8 + "] 已掉线，继续播放将在 [" +
                       out.targetEndpointNameUtf8 + "] 产生双声回音；请重插副设备并在音量合成器确认路由。");
            } else {
                out.bannerTitleUtf8 = en
                    ? "Secondary Audio Device Disconnected (Main Output Only)"
                    : "副音频设备已断开（当前仅剩主输出设备）";
                out.bannerDetailUtf8 = en
                    ? ("Previous secondary device [" + out.lostEndpointNameUtf8 + "] disconnected; only [" +
                       defEpDisp + "] remains. Please reconnect your secondary output device.")
                    : ("原副设备 [" + out.lostEndpointNameUtf8 + "] 已断开，当前仅剩 [" +
                       defEpDisp + "]；建议重新接入副音频设备。");
            }
            return;

        case RoutingIsolationState::ConflictOnDefaultOutput:
            if (out.targetSessionActive) {
                out.bannerTitleUtf8 = en
                    ? ("Routing Overlap Warning: " + appDisp + " Is Playing on Main Output!")
                    : ("路由重叠警告：" + appDisp + " 正在主输出设备发声！");
                out.bannerDetailUtf8 = en
                    ? (appDisp + " is outputting directly to [" + out.targetEndpointNameUtf8 +
                       "], causing ~8s delayed double audio with the game radio. Stopping the bridge only mutes the game radio — "
                       "click 'Volume Mixer' and route " + appDisp + " to a secondary device.")
                    : (appDisp + " 当前位于主设备 [" + out.targetEndpointNameUtf8 +
                       "]，会与游戏电台产生双声重叠。暂停本桥接器仅停止向游戏供音，"
                       "无法阻止主设备直出；请点击『打开音量合成器』将输出改到副设备。");
            } else {
                out.bannerTitleUtf8 = en
                    ? ("Routing Overlap Notice: " + appDisp + " Session Is on Main Output")
                    : ("路由重叠提醒：" + appDisp + " 会话位于主输出设备");
                out.bannerDetailUtf8 = en
                    ? (appDisp + " is bound to main device [" + out.targetEndpointNameUtf8 +
                       "], which will cause double audio once playback starts. Click 'Volume Mixer' to route it to a secondary device.")
                    : (appDisp + " 当前绑定在主设备 [" + out.targetEndpointNameUtf8 +
                       "]，开始播放后会产生双声回音；请点击『打开音量合成器』将其输出改到副设备。");
            }
            return;

        case RoutingIsolationState::SingleEndpointOnly:
            out.bannerTitleUtf8 = en
                ? "Single Output Notice: No Secondary Audio Endpoint Detected"
                : "单输出设备提醒：未检测到副音频分流端点";
            if (out.activeRenderEndpointCount == 0u) {
                out.bannerDetailUtf8 = en
                    ? "No active audio output devices detected. Please connect your headphones/speakers and a secondary output device."
                    : "当前未检测到可用音频输出设备；请连接耳机/音箱与副音频设备。";
            } else {
                out.bannerDetailUtf8 = en
                    ? ("Only 1 playback device [" + defEpDisp +
                       "] is active. Connect a secondary device and route " + appDisp + " to it in Volume Mixer to prevent double audio.")
                    : ("系统当前仅有 1 个播放设备 [" + defEpDisp +
                       "]；建议接入副设备并在音量合成器将 " + appDisp + " 路由至副设备以避免双声。");
            }
            return;

        case RoutingIsolationState::WaitingForTargetSession:
            out.bannerTitleUtf8 = en
                ? ("Multiple Outputs Ready (Waiting for " + appDisp + " Audio Session)")
                : ("多输出设备已就绪（等待 " + appDisp + " 音频会话）");
            out.bannerDetailUtf8 = en
                ? ("Main output [" + defEpDisp + "] (" + std::to_string(out.activeRenderEndpointCount) +
                   " playback endpoints active); split-endpoint isolation will be verified automatically once playback starts.")
                : ("主输出 [" + defEpDisp + "]（共检测到 " +
                   std::to_string(out.activeRenderEndpointCount) +
                   " 个播放端点）；首次播放后将自动核查副端点分流状态。");
            return;

        case RoutingIsolationState::Unknown:
        default:
            out.bannerTitleUtf8 = en
                ? "Inspecting Windows Audio Output Topology..."
                : "正在检测音频输出拓扑...";
            out.bannerDetailUtf8 = en
                ? "Waiting for audio endpoints and session status update."
                : "等待音频端点与会话状态更新。";
            return;
    }
}

RoutingDiagnostic EvaluateRoutingIsolation(const staged::AudioTopologyReport& topo,
                                           uint32_t currentTargetPid,
                                           const std::wstring& targetExeLeaf,
                                           RoutingIsolatorTracker& tracker,
                                           UiLanguage lang) {
    RoutingDiagnostic out;
    out.activeRenderEndpointCount = static_cast<uint32_t>(topo.endpoints.size());

    const std::wstring effectiveLeaf = targetExeLeaf.empty()
        ? std::wstring(L"AMPLibraryAgent.exe")
        : staged::ExtractLeafFilename(targetExeLeaf);
    const bool defaultAppleMusicMode = staged::EqualsIgnoreCase(effectiveLeaf, L"AMPLibraryAgent.exe");
    out.isAppleMusicMode = defaultAppleMusicMode;
    out.targetDisplayNameUtf8 = defaultAppleMusicMode
        ? std::string("Apple Music")
        : staged::WideToUtf8(effectiveLeaf);

    if (currentTargetPid != 0u && currentTargetPid != tracker.lastObservedTargetPid) {
        tracker.lastObservedTargetPid = currentTargetPid;
        tracker.lastActiveEndpointId.clear();
        tracker.lastActiveEndpointName.clear();
    }

    // Build entire process-tree PID set when bound to a root PID (supports Chromium/Electron child audio processes)
    std::set<uint32_t> targetTreePids;
    if (currentTargetPid != 0u) {
        targetTreePids.insert(currentTargetPid);
        const std::vector<uint32_t> descendants = staged::CollectProcessTreePids(topo.processes, currentTargetPid);
        targetTreePids.insert(descendants.begin(), descendants.end());
    }

    auto isTargetSession = [&](const staged::AudioSessionRecord& sess) -> bool {
        if (sess.isSystemSoundsSession || sess.isMultiProcessSession || sess.processId == 0u) {
            return false;
        }
        if (currentTargetPid != 0u) {
            return targetTreePids.count(sess.processId) > 0u;
        }
        return staged::EqualsIgnoreCase(GetSessionExeLeaf(sess), effectiveLeaf);
    };

    // Check running processes in topology snapshot
    for (const auto& proc : topo.processes) {
        const std::wstring leaf = GetProcessExeLeaf(proc);
        if (staged::EqualsIgnoreCase(leaf, L"AppleMusic.exe")) {
            out.appleMusicUiProcessAlive = true;
        }
        if (staged::EqualsIgnoreCase(leaf, effectiveLeaf)) {
            out.ampAgentProcessAlive = true;
        }
        if (staged::EqualsIgnoreCase(leaf, L"GTAIV.exe")) {
            out.gtaProcessAlive = true;
        }
    }

    // Identify default render endpoint name and GTAIV.exe active audio endpoint (if any)
    std::wstring defaultEndpointId = !topo.defaultMultimediaEndpointId.empty()
        ? topo.defaultMultimediaEndpointId
        : topo.defaultConsoleEndpointId;
    std::wstring defaultEndpointName;
    std::wstring gtaActiveEndpointId;
    std::wstring gtaActiveEndpointName;
    bool lastIsolatedStillPresent = false;

    for (const auto& ep : topo.endpoints) {
        if (!tracker.lastIsolatedEndpointId.empty() &&
            staged::EqualsIgnoreCase(ep.endpointId, tracker.lastIsolatedEndpointId)) {
            lastIsolatedStillPresent = true;
        }
        if ((!defaultEndpointId.empty() && staged::EqualsIgnoreCase(ep.endpointId, defaultEndpointId)) ||
            (defaultEndpointId.empty() && (ep.isDefaultMultimedia || ep.isDefaultConsole))) {
            defaultEndpointId = ep.endpointId;
            defaultEndpointName = ep.friendlyName;
        }
    }
    if (defaultEndpointName.empty() && !topo.endpoints.empty()) {
        defaultEndpointId = topo.endpoints.front().endpointId;
        defaultEndpointName = topo.endpoints.front().friendlyName;
    }

    out.defaultEndpointIdUtf8 = staged::WideToUtf8(defaultEndpointId);
    out.defaultEndpointNameUtf8 = staged::WideToUtf8(defaultEndpointName);

    auto isConflictEndpoint = [&](const std::wstring& epId) -> bool {
        if (!topo.defaultMultimediaEndpointId.empty() &&
            staged::EqualsIgnoreCase(epId, topo.defaultMultimediaEndpointId)) {
            return true;
        }
        if (!topo.defaultConsoleEndpointId.empty() &&
            staged::EqualsIgnoreCase(epId, topo.defaultConsoleEndpointId)) {
            return true;
        }
        if (!defaultEndpointId.empty() && staged::EqualsIgnoreCase(epId, defaultEndpointId)) {
            return true;
        }
        if (!gtaActiveEndpointId.empty() && staged::EqualsIgnoreCase(epId, gtaActiveEndpointId)) {
            return true;
        }
        return false;
    };

    // Pass 1: Locate GTAIV.exe active session & co-existing non-target active media sessions
    std::set<std::string> otherAppsSet;
    for (const auto& ep : topo.endpoints) {
        for (const auto& sess : ep.sessions) {
            if (sess.isSystemSoundsSession || sess.processId == 0u) {
                continue;
            }
            const std::wstring leaf = GetSessionExeLeaf(sess);
            if (staged::EqualsIgnoreCase(leaf, L"GTAIV.exe")) {
                if (sess.sessionState == AudioSessionStateActive || gtaActiveEndpointId.empty()) {
                    gtaActiveEndpointId = ep.endpointId;
                    gtaActiveEndpointName = ep.friendlyName;
                }
                continue;
            }
            if (isTargetSession(sess) ||
                (defaultAppleMusicMode && staged::EqualsIgnoreCase(leaf, L"AppleMusic.exe"))) {
                continue;
            }
            if (sess.sessionState == AudioSessionStateActive && !leaf.empty()) {
                otherAppsSet.insert(staged::WideToUtf8(leaf));
            }
        }
    }
    out.gtaEndpointNameUtf8 = staged::WideToUtf8(gtaActiveEndpointName);
    out.otherActiveMediaAppsUtf8.assign(otherAppsSet.begin(), otherAppsSet.end());

    // Pass 2: Collect target sessions (preferring Apple Store path when in default mode and PID==0)
    struct MatchedSession {
        const staged::AudioEndpointRecord* ep = nullptr;
        const staged::AudioSessionRecord* sess = nullptr;
    };
    std::vector<MatchedSession> activeTargetSessions;
    std::vector<MatchedSession> inactiveTargetSessions;

    for (int storePass = 0; storePass < 2; ++storePass) {
        const bool requireStore = (storePass == 0) && (currentTargetPid == 0u) && defaultAppleMusicMode;
        for (const auto& ep : topo.endpoints) {
            for (const auto& sess : ep.sessions) {
                if (!isTargetSession(sess)) {
                    continue;
                }
                if (requireStore && !IsAppleMusicStorePathHint(sess.exeFullPath)) {
                    continue;
                }
                if (sess.sessionState == AudioSessionStateActive) {
                    activeTargetSessions.push_back({ &ep, &sess });
                } else if (sess.sessionState == AudioSessionStateInactive) {
                    inactiveTargetSessions.push_back({ &ep, &sess });
                }
            }
        }
        if (!activeTargetSessions.empty() || !inactiveTargetSessions.empty()) {
            break;
        }
    }

    auto populateSessionVolume = [&](const staged::AudioSessionRecord& s) {
        out.targetHasSession = true;
        out.targetSessionActive = (s.sessionState == AudioSessionStateActive);
        if (s.volumeValid) {
            out.targetSessionVolume = s.masterVolume;
            out.targetSessionMuted = s.muted || (s.masterVolume < 0.005f);
        }
    };

    // Case A: At least one Active target session exists (authoritative, ignores stale Inactive tombstones)
    if (!activeTargetSessions.empty()) {
        const MatchedSession* conflictMatch = nullptr;
        for (const auto& m : activeTargetSessions) {
            if (isConflictEndpoint(m.ep->endpointId)) {
                conflictMatch = &m;
                break;
            }
        }
        const MatchedSession& chosen = conflictMatch ? *conflictMatch : activeTargetSessions.front();
        populateSessionVolume(*chosen.sess);
        out.targetEndpointIdUtf8 = staged::WideToUtf8(chosen.ep->endpointId);
        out.targetEndpointNameUtf8 = staged::WideToUtf8(chosen.ep->friendlyName);

        tracker.lastActiveEndpointId = chosen.ep->endpointId;
        tracker.lastActiveEndpointName = chosen.ep->friendlyName;

        if (!conflictMatch) {
            out.state = RoutingIsolationState::IsolatedOk;
            tracker.lastIsolatedEndpointId = chosen.ep->endpointId;
            tracker.lastIsolatedEndpointName = chosen.ep->friendlyName;
        } else if (!tracker.lastIsolatedEndpointId.empty() && !lastIsolatedStillPresent) {
            out.state = RoutingIsolationState::SecondaryEndpointLostFallback;
            out.lostEndpointNameUtf8 = staged::WideToUtf8(tracker.lastIsolatedEndpointName);
        } else {
            out.state = RoutingIsolationState::ConflictOnDefaultOutput;
        }
        FormatRoutingDiagnosticText(out, lang);
        return out;
    }

    // Case B: Only Inactive target session(s) exist (paused / idle)
    if (!inactiveTargetSessions.empty()) {
        const MatchedSession* chosen = nullptr;

        // 1. Prefer last known Active endpoint for this PID to ignore stale tombstones
        if (!tracker.lastActiveEndpointId.empty()) {
            for (const auto& m : inactiveTargetSessions) {
                if (staged::EqualsIgnoreCase(m.ep->endpointId, tracker.lastActiveEndpointId)) {
                    chosen = &m;
                    break;
                }
            }
        }
        // 2. Otherwise prefer a non-conflict isolated endpoint if one already has a session
        if (!chosen) {
            for (const auto& m : inactiveTargetSessions) {
                if (!isConflictEndpoint(m.ep->endpointId)) {
                    chosen = &m;
                    break;
                }
            }
        }
        // 3. Fallback to first inactive session
        if (!chosen) {
            chosen = &inactiveTargetSessions.front();
        }

        populateSessionVolume(*chosen->sess);
        out.targetEndpointIdUtf8 = staged::WideToUtf8(chosen->ep->endpointId);
        out.targetEndpointNameUtf8 = staged::WideToUtf8(chosen->ep->friendlyName);

        if (!isConflictEndpoint(chosen->ep->endpointId)) {
            out.state = RoutingIsolationState::IsolatedOk;
            tracker.lastIsolatedEndpointId = chosen->ep->endpointId;
            tracker.lastIsolatedEndpointName = chosen->ep->friendlyName;
        } else if (!tracker.lastIsolatedEndpointId.empty() && !lastIsolatedStillPresent) {
            out.state = RoutingIsolationState::SecondaryEndpointLostFallback;
            out.lostEndpointNameUtf8 = staged::WideToUtf8(tracker.lastIsolatedEndpointName);
        } else {
            out.state = RoutingIsolationState::ConflictOnDefaultOutput;
        }
        FormatRoutingDiagnosticText(out, lang);
        return out;
    }

    // Case C: Target has not opened any WASAPI render session yet
    if (!tracker.lastIsolatedEndpointId.empty() && !lastIsolatedStillPresent && topo.endpoints.size() <= 1u) {
        out.state = RoutingIsolationState::SecondaryEndpointLostFallback;
        out.lostEndpointNameUtf8 = staged::WideToUtf8(tracker.lastIsolatedEndpointName);
    } else if (topo.endpoints.size() <= 1u) {
        out.state = RoutingIsolationState::SingleEndpointOnly;
    } else {
        out.state = RoutingIsolationState::WaitingForTargetSession;
    }
    FormatRoutingDiagnosticText(out, lang);
    return out;
}

// ============================================================================
// Section C: Unified Human-Readable Pipeline & Silence State Machine
// ============================================================================

const char* PipelineHeadlineStateName(PipelineHeadlineState state) noexcept {
    switch (state) {
        case PipelineHeadlineState::BridgeStopped: return "bridge_stopped";
        case PipelineHeadlineState::ProducerMutexBusy: return "producer_mutex_busy";
        case PipelineHeadlineState::WaitingForAppleMusic: return "waiting_for_apple_music";
        case PipelineHeadlineState::ReconnectingTarget: return "reconnecting_target";
        case PipelineHeadlineState::TargetMutedInMixerWarning: return "target_muted_in_mixer_warning";
        case PipelineHeadlineState::AppleMusicPausedOrIdle: return "apple_music_paused_or_idle";
        case PipelineHeadlineState::BriefQuietPassage: return "brief_quiet_passage";
        case PipelineHeadlineState::SustainedZeroAudioWarning: return "sustained_zero_audio_warning";
        case PipelineHeadlineState::StreamingWaitingForGame: return "streaming_waiting_for_game";
        case PipelineHeadlineState::StreamingInGame: return "streaming_in_game";
    }
    return "bridge_stopped";
}

BridgeDiagnosticReport EvaluateBridgeDiagnostics(const BridgeDiagnosticInput& input,
                                                 BridgeStatusTracker& tracker,
                                                 UiLanguage lang) {
    const bool en = (ResolveEffectiveLanguage(lang) == UiLanguage::EnUs);
    const bool isAppleMusic =
        (input.sourceMode == AudioSourceMode::AppleMusicPreset) &&
        (input.targetDisplayNameUtf8.empty() || input.targetDisplayNameUtf8 == "Apple Music") &&
        input.routing.isAppleMusicMode;
    const std::string appDisp = !input.targetDisplayNameUtf8.empty()
        ? input.targetDisplayNameUtf8
        : (!input.routing.targetDisplayNameUtf8.empty()
               ? input.routing.targetDisplayNameUtf8
               : std::string("Apple Music"));

    BridgeDiagnosticReport out;
    out.hasRoutingConflict =
        (input.routing.state == RoutingIsolationState::ConflictOnDefaultOutput) ||
        (input.routing.state == RoutingIsolationState::SecondaryEndpointLostFallback);

    if (input.producerMutexBusy) {
        tracker.Reset();
        out.headline = PipelineHeadlineState::ProducerMutexBusy;
        out.badgeColor = TrayBadgeColor::Red;
        out.headlineTitleUtf8 = en
            ? "Startup Failed: Shared Memory Already in Use"
            : "启动失败：共享内存已被另一生产者占用";
        out.headlineDetailUtf8 = !input.mutexBusyDetailUtf8.empty()
            ? input.mutexBusyDetailUtf8
            : (en ? std::string("Please close any other running iv_stage_d_producer.exe or test producer process and try again.")
                  : std::string("请检查并关闭其他正在运行的 iv_stage_d_producer.exe 或测试进程后再试。"));
        out.trayTooltipUtf8 = en
            ? "GTA IV Audio Bridge: Shared Memory Conflict"
            : "GTA IV 电台桥接：共享内存冲突";
        return out;
    }

    if (!input.engineRunning) {
        tracker.Reset();
        out.headline = PipelineHeadlineState::BridgeStopped;
        out.badgeColor = TrayBadgeColor::Gray;
        out.headlineTitleUtf8 = en ? "Bridge Stopped" : "桥接已停止";
        if (input.lastShutdownVerified) {
            char buf[320]{};
            if (en) {
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "Last shutdown drain: %s (holdback=%u, committed=%llu, queued=%llu). Click 'Start Bridge' to resume.",
                    input.lastShutdownClean ? "Flushed cleanly H=0, C=Q" : "Verification mismatch",
                    static_cast<unsigned>(input.lastShutdownHoldbackFrames),
                    static_cast<unsigned long long>(input.lastShutdownCommittedFrames),
                    static_cast<unsigned long long>(input.lastShutdownQueuedFrames));
            } else {
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "上次退出收尾：%s（holdback=%u, committed=%llu, queued=%llu）。点击『启动桥接』恢复运行。",
                    input.lastShutdownClean ? "已完整刷空 H=0, C=Q" : "校验异常",
                    static_cast<unsigned>(input.lastShutdownHoldbackFrames),
                    static_cast<unsigned long long>(input.lastShutdownCommittedFrames),
                    static_cast<unsigned long long>(input.lastShutdownQueuedFrames));
            }
            out.headlineDetailUtf8 = buf;
        } else {
            out.headlineDetailUtf8 = en
                ? ("Not streaming to GTA IV. Click 'Start Bridge' to auto-discover " + appDisp + ".")
                : ("当前未向 GTA IV 供音；点击『启动桥接』即可自动发现 " + appDisp + "。");
        }
        out.trayTooltipUtf8 = en ? "GTA IV Audio Bridge: Stopped" : "GTA IV 电台桥接：已停止";
        return out;
    }

    if (input.captureTargetPid != tracker.trackedPid) {
        tracker.trackedPid = input.captureTargetPid;
        tracker.activeSilentTracking = false;
        tracker.activeSilentStartQpc100ns = 0ULL;
        tracker.lastNonActiveQpc100ns = input.nowQpc100ns;
    }

    // Target not currently bound
    if (!input.captureStreamActive || input.captureTargetPid == 0u) {
        tracker.activeSilentTracking = false;
        tracker.activeSilentStartQpc100ns = 0ULL;
        tracker.lastNonActiveQpc100ns = input.nowQpc100ns;

        if (input.streamRestartCount > 0ULL && input.msSinceLastStreamEnd < 3000ULL) {
            out.headline = PipelineHeadlineState::ReconnectingTarget;
            out.badgeColor = TrayBadgeColor::Amber;
            out.headlineTitleUtf8 = en
                ? "Reconnecting to Audio Session..."
                : "目标音频会话重连中...";
            out.headlineDetailUtf8 = en
                ? ("Previous capture session exited or switched; auto-scanning every 350 ms to rebind " + appDisp + ".")
                : ("检测到原捕获会话退出或切换，正在每 350ms 自动扫描重新绑定 " + appDisp + "。");
            out.trayTooltipUtf8 = en
                ? TruncateUtf8Safe("GTA IV Audio Bridge: Reconnecting " + appDisp + "...", 120u)
                : TruncateUtf8Safe("GTA IV 电台桥接：自动重连 " + appDisp + "...", 120u);
            return out;
        }

        out.headline = PipelineHeadlineState::WaitingForAppleMusic;
        out.badgeColor = out.hasRoutingConflict ? TrayBadgeColor::Red : TrayBadgeColor::Blue;
        if (isAppleMusic && input.routing.appleMusicUiProcessAlive && !input.routing.ampAgentProcessAlive) {
            out.headlineTitleUtf8 = en
                ? "Apple Music Window Detected — Waiting for Audio Engine"
                : "已检测到 Apple Music 窗口，等待音频引擎启动";
            out.headlineDetailUtf8 = en
                ? "Play any track in Apple Music to start its audio agent (auto-attaches every 350 ms, no PID needed)."
                : "请在 Apple Music 中点击任意歌曲开始播放（后台每 350ms 自动接入，无需手填 PID）。";
        } else {
            out.headlineTitleUtf8 = en ? ("Waiting for " + appDisp) : ("等待启动 " + appDisp);
            out.headlineDetailUtf8 = en
                ? (appDisp + " is not running yet. Launch the app and it will be discovered and captured automatically every 350 ms.")
                : ("未检测到 " + appDisp + " 运行；启动该程序后将每 350ms 自动发现并捕获。");
        }
        out.trayTooltipUtf8 = en
            ? TruncateUtf8Safe("GTA IV Audio Bridge: Waiting for " + appDisp, 120u)
            : TruncateUtf8Safe("GTA IV 电台桥接：等待 " + appDisp, 120u);
        return out;
    }

    // Target session muted or 0 volume in Windows Volume Mixer
    if (input.routing.targetHasSession && input.routing.targetSessionMuted) {
        tracker.activeSilentTracking = false;
        tracker.activeSilentStartQpc100ns = 0ULL;
        tracker.lastNonActiveQpc100ns = input.nowQpc100ns;

        out.headline = PipelineHeadlineState::TargetMutedInMixerWarning;
        out.badgeColor = TrayBadgeColor::Red;
        out.headlineTitleUtf8 = en
            ? (appDisp + " Is Muted in Windows Volume Mixer")
            : ("系统音量合成器中 " + appDisp + " 已被静音");
        out.headlineDetailUtf8 = en
            ? "Windows zeroes process-loopback audio when an app session is muted or at 0% in Volume Mixer. Click 'Volume Mixer' to unmute it."
            : "Windows 会将静音或 0% 音量会话的进程环回音频一并置零；请点击『打开音量合成器』恢复其会话音量。";
        out.trayTooltipUtf8 = en
            ? "GTA IV Audio Bridge: Warning - Muted in Volume Mixer"
            : "GTA IV 电台桥接：警告 - 合成器静音导致无声";
        return out;
    }

    // Check instantaneous audible signal vs active-silent tracking.
    const bool hasAudible100ms = (input.level.audibleFrames100ms > 0ULL);
    const bool hasAudible1s = (input.level.audibleFrames1s > 0ULL);
    const bool sessionKnownInactive =
        (input.routing.state != RoutingIsolationState::Unknown) && !input.routing.targetSessionActive;
    const bool activelyPlayingButSilent =
        !hasAudible100ms &&
        (input.routing.targetSessionActive ||
         (!sessionKnownInactive && input.level.hasRecentPackets && input.level.frames1s >= 12000ULL));

    if (hasAudible100ms) {
        tracker.activeSilentTracking = false;
        tracker.activeSilentStartQpc100ns = 0ULL;
        tracker.lastNonActiveQpc100ns = input.nowQpc100ns;
        out.continuousActiveSilentMs = 0ULL;
    } else if (activelyPlayingButSilent) {
        if (!tracker.activeSilentTracking) {
            tracker.activeSilentTracking = true;
            uint64_t anchor100ns = input.nowQpc100ns;
            if (input.level.msSinceLastAudible != UINT64_MAX) {
                const uint64_t sinceAudible100ns = input.level.msSinceLastAudible * 10000ULL;
                const uint64_t audibleEnd100ns = (input.nowQpc100ns >= sinceAudible100ns)
                    ? (input.nowQpc100ns - sinceAudible100ns)
                    : 0ULL;
                anchor100ns = std::max(audibleEnd100ns, tracker.lastNonActiveQpc100ns);
            }
            tracker.activeSilentStartQpc100ns = anchor100ns;
        }
        if (input.nowQpc100ns >= tracker.activeSilentStartQpc100ns) {
            out.continuousActiveSilentMs = (input.nowQpc100ns - tracker.activeSilentStartQpc100ns) / 10000ULL;
        }
    } else {
        tracker.activeSilentTracking = false;
        tracker.activeSilentStartQpc100ns = 0ULL;
        tracker.lastNonActiveQpc100ns = input.nowQpc100ns;
        out.continuousActiveSilentMs = 0ULL;
    }

    const bool withinStreamingHoldover =
        !sessionKnownInactive &&
        (hasAudible1s || (input.level.msSinceLastAudible < kStreamingHoldoverMs));

    if (hasAudible100ms || (withinStreamingHoldover && (input.routing.targetSessionActive || input.level.hasRecentPackets))) {
        if (input.ipc.consumerActiveReaders > 0u) {
            out.headline = PipelineHeadlineState::StreamingInGame;
            out.badgeColor = out.hasRoutingConflict ? TrayBadgeColor::Red : TrayBadgeColor::Green;
            out.headlineTitleUtf8 = en
                ? "Streaming Live to GTA IV Independence FM"
                : "正在向 GTA IV 独立电台供音";
            char buf[360]{};
            if (en) {
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "In-game radio connected (active slots: %u) | Source: %s (PID %lu) | Peak %.1f dBFS / RMS %.1f dBFS",
                    static_cast<unsigned>(input.ipc.consumerActiveReaders),
                    input.captureTargetExeUtf8.c_str(),
                    static_cast<unsigned long>(input.captureTargetPid),
                    input.level.peakDbfs1s,
                    input.level.rmsDbfs1s);
            } else {
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "游戏电台已连接（活跃读取槽：%u） | 捕获进程：%s (PID %lu) | Peak %.1f dBFS / RMS %.1f dBFS",
                    static_cast<unsigned>(input.ipc.consumerActiveReaders),
                    input.captureTargetExeUtf8.c_str(),
                    static_cast<unsigned long>(input.captureTargetPid),
                    input.level.peakDbfs1s,
                    input.level.rmsDbfs1s);
            }
            out.headlineDetailUtf8 = buf;
            if (en) {
                out.trayTooltipUtf8 = out.hasRoutingConflict
                    ? "GTA IV Audio Bridge: Streaming (Warning: Main Output Overlap!)"
                    : TruncateUtf8Safe("GTA IV Audio Bridge: Streaming (" + appDisp + ")", 120u);
            } else {
                out.trayTooltipUtf8 = out.hasRoutingConflict
                    ? "GTA IV 电台桥接：供音中（警告：主输出路由重叠！）"
                    : TruncateUtf8Safe("GTA IV 电台桥接：供音中 (" + appDisp + ")", 120u);
            }
        } else {
            out.headline = PipelineHeadlineState::StreamingWaitingForGame;
            out.badgeColor = out.hasRoutingConflict ? TrayBadgeColor::Red : TrayBadgeColor::Green;
            out.headlineTitleUtf8 = en
                ? ("Capturing " + appDisp + " Audio (Waiting for In-Game Radio)")
                : ("已捕获 " + appDisp + " 音频（等待进入游戏电台）");
            char buf[320]{};
            if (en) {
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "Audio stream healthy (Peak %.1f dBFS / RMS %.1f dBFS) | %s; tune to Independence FM in a vehicle.",
                    input.level.peakDbfs1s,
                    input.level.rmsDbfs1s,
                    input.routing.gtaProcessAlive ? "GTAIV.exe running" : "Waiting for GTA IV");
            } else {
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "音频流正常（Peak %.1f dBFS / RMS %.1f dBFS） | %s；切入车载 Independence FM 即自动播放。",
                    input.level.peakDbfs1s,
                    input.level.rmsDbfs1s,
                    input.routing.gtaProcessAlive ? "已检测到 GTAIV.exe 运行" : "等待启动 GTA IV");
            }
            out.headlineDetailUtf8 = buf;
            if (en) {
                out.trayTooltipUtf8 = out.hasRoutingConflict
                    ? "GTA IV Audio Bridge: Audio Captured (Warning: Output Overlap!)"
                    : "GTA IV Audio Bridge: Audio Captured (Waiting for Game Radio)";
            } else {
                out.trayTooltipUtf8 = out.hasRoutingConflict
                    ? "GTA IV 电台桥接：已捕获音频（警告：主输出路由重叠！）"
                    : "GTA IV 电台桥接：已捕获音频（等待进入游戏电台）";
            }
        }
        return out;
    }

    if (activelyPlayingButSilent) {
        if (out.continuousActiveSilentMs >= kSustainedSilenceWarnMs) {
            out.headline = PipelineHeadlineState::SustainedZeroAudioWarning;
            out.badgeColor = TrayBadgeColor::Red;
            char titleBuf[200]{};
            if (en) {
                std::snprintf(
                    titleBuf,
                    sizeof(titleBuf),
                    "Sustained Silence Warning: Session Active with %.1f s of Zero Audio",
                    static_cast<double>(out.continuousActiveSilentMs) / 1000.0);
            } else {
                std::snprintf(
                    titleBuf,
                    sizeof(titleBuf),
                    "持续无声告警：会话播放中但已连续 %.1f 秒捕获全零电平",
                    static_cast<double>(out.continuousActiveSilentMs) / 1000.0);
            }
            out.headlineTitleUtf8 = titleBuf;
            if (isAppleMusic) {
                out.headlineDetailUtf8 = en
                    ? "Please check: (1) Set Apple Music 'Settings -> Playback -> Dolby Atmos' to 'Off'; "
                      "(2) Ensure Apple Music's in-app volume slider is not set to 0."
                    : "请依次排查：① Apple Music『设置 -> 播放 -> 杜比全景声 (Dolby Atmos)』是否已设为『关闭』；"
                      "② Apple Music 应用内音量滑块是否被调至 0。";
                out.trayTooltipUtf8 = en
                    ? "GTA IV Audio Bridge: Silence Warning (Check Dolby Atmos Setting)"
                    : "GTA IV 电台桥接：持续无声告警（请检查杜比全景声设置）";
            } else {
                out.headlineDetailUtf8 = en
                    ? ("Please check: (1) Ensure " + appDisp + "'s in-app volume or browser tab is not muted; "
                       "(2) Disable Exclusive Mode (WASAPI Exclusive / ASIO) in the app's audio output settings.")
                    : ("请依次排查：① 检查 " + appDisp + " 应用内音量滑块或网页标签页是否被静音；"
                       "② 若软件开启了独占音频输出（WASAPI 独占 / ASIO），请切回标准共享模式。");
                out.trayTooltipUtf8 = en
                    ? "GTA IV Audio Bridge: Silence Warning (Check App Volume / Exclusive Mode)"
                    : "GTA IV 电台桥接：持续无声告警（请检查应用内静音或独占模式）";
            }
            return out;
        }

        out.headline = PipelineHeadlineState::BriefQuietPassage;
        out.badgeColor = out.hasRoutingConflict ? TrayBadgeColor::Red : TrayBadgeColor::Green;
        out.headlineTitleUtf8 = en ? "Track Gap / Quiet Musical Passage" : "曲间过渡 / 歌曲弱音片段";
        char buf[300]{};
        if (en) {
            std::snprintf(
                buf,
                sizeof(buf),
                "%s is active; current quiet span %.1f s (< 5.0 s is normal between tracks or in quiet sections).",
                appDisp.c_str(),
                static_cast<double>(out.continuousActiveSilentMs) / 1000.0);
        } else {
            std::snprintf(
                buf,
                sizeof(buf),
                "%s 处于播放态，当前静音持续 %.1f 秒（< 5.0 秒属正常曲间空隙或弱音段，无需处理）。",
                appDisp.c_str(),
                static_cast<double>(out.continuousActiveSilentMs) / 1000.0);
        }
        out.headlineDetailUtf8 = buf;
        out.trayTooltipUtf8 = en
            ? "GTA IV Audio Bridge: Track Gap / Quiet Passage"
            : "GTA IV 电台桥接：曲间过渡 / 弱音段";
        return out;
    }

    out.headline = PipelineHeadlineState::AppleMusicPausedOrIdle;
    out.badgeColor = out.hasRoutingConflict ? TrayBadgeColor::Red : TrayBadgeColor::Blue;
    out.headlineTitleUtf8 = en ? (appDisp + " Paused or Idle") : (appDisp + " 已暂停或空闲");
    char buf[320]{};
    const char* exeDisp = input.captureTargetExeUtf8.empty() ? appDisp.c_str() : input.captureTargetExeUtf8.c_str();
    if (en) {
        std::snprintf(
            buf,
            sizeof(buf),
            "Attached to %s (PID %lu); playback is currently paused or idle (normal). Press Play to resume radio audio.",
            exeDisp,
            static_cast<unsigned long>(input.captureTargetPid));
    } else {
        std::snprintf(
            buf,
            sizeof(buf),
            "已绑定 %s (PID %lu)，当前未处于播放态（正常暂停，非故障）；点击播放后自动恢复供音。",
            exeDisp,
            static_cast<unsigned long>(input.captureTargetPid));
    }
    out.headlineDetailUtf8 = buf;
    out.trayTooltipUtf8 = en
        ? TruncateUtf8Safe("GTA IV Audio Bridge: " + appDisp + " Paused", 120u)
        : TruncateUtf8Safe("GTA IV 电台桥接：" + appDisp + " 已暂停", 120u);
    return out;
}

// ============================================================================
// Section D: Repo-Aware Default Log Directory & Rotating Session Logger
// ============================================================================

std::wstring ResolveDefaultLogDirectory(const std::wstring& exePathOverride) {
    std::wstring exePath = exePathOverride;
    if (exePath.empty()) {
        wchar_t modBuf[MAX_PATH * 4]{};
        const DWORD len = GetModuleFileNameW(nullptr, modBuf, static_cast<DWORD>(std::size(modBuf)));
        if (len > 0u && len < std::size(modBuf)) {
            exePath.assign(modBuf, len);
        }
    }

    const std::wstring exeDir = ParentDirectoryOf(exePath);
    if (exeDir.empty()) {
        return L"logs";
    }

    std::wstring probeDir = exeDir;
    for (int depth = 0; depth <= 4 && !probeDir.empty(); ++depth) {
        const std::wstring presetsFile = JoinWinPath(probeDir, L"CMakePresets.json");
        const std::wstring agentsFile = JoinWinPath(probeDir, L"AGENTS.md");
        if (FileExistsAt(presetsFile) && FileExistsAt(agentsFile)) {
            return JoinWinPath(probeDir, L"logs");
        }
        const std::wstring nextParent = ParentDirectoryOf(probeDir);
        if (nextParent == probeDir) {
            break;
        }
        probeDir = nextParent;
    }

    return JoinWinPath(exeDir, L"logs");
}

bool ParseStageESessionLogFilename(const std::wstring& leafFilename,
                                   std::wstring& outSessionKey) {
    outSessionKey.clear();
    const std::wstring kPrefix = L"stage_e_session_";
    const std::wstring kSuffix = L".jsonl";

    if (leafFilename.size() <= kPrefix.size() + kSuffix.size()) {
        return false;
    }
    if (!staged::EqualsIgnoreCase(leafFilename.substr(0u, kPrefix.size()), kPrefix)) {
        return false;
    }
    if (!EndsWithIgnoreCaseW(leafFilename, kSuffix)) {
        return false;
    }

    const std::wstring stem = leafFilename.substr(0u, leafFilename.size() - kSuffix.size());
    size_t pos = kPrefix.size();

    // Expect YYYYMMDD_HHMMSS_pid<digits>
    if (pos + 19u >= stem.size()) {
        return false;
    }
    for (size_t i = 0u; i < 8u; ++i) {
        if (!IsAsciiDigitW(stem[pos++])) return false;
    }
    if (stem[pos++] != L'_') return false;
    for (size_t i = 0u; i < 6u; ++i) {
        if (!IsAsciiDigitW(stem[pos++])) return false;
    }
    if (stem.compare(pos, 4u, L"_pid") != 0) {
        return false;
    }
    pos += 4u;
    const size_t pidStart = pos;
    while (pos < stem.size() && IsAsciiDigitW(stem[pos])) {
        ++pos;
    }
    if (pos == pidStart) {
        return false;
    }

    // Optional _s<digits>
    if (pos + 2u <= stem.size() && stem[pos] == L'_' && (stem[pos + 1u] == L's' || stem[pos + 1u] == L'S')) {
        size_t sPos = pos + 2u;
        const size_t sDigitsStart = sPos;
        while (sPos < stem.size() && IsAsciiDigitW(stem[sPos])) {
            ++sPos;
        }
        if (sPos == sDigitsStart) {
            return false;
        }
        pos = sPos;
    }

    const std::wstring canonicalKey = L"stage_e_session_" + stem.substr(kPrefix.size(), pos - kPrefix.size());

    // Remainder must be either empty or _part<2+ digits>
    if (pos < stem.size()) {
        if (stem.size() - pos < 7u) {
            return false;
        }
        const std::wstring remPrefix = stem.substr(pos, 5u);
        if (!staged::EqualsIgnoreCase(remPrefix, L"_part")) {
            return false;
        }
        pos += 5u;
        const size_t partStart = pos;
        while (pos < stem.size() && IsAsciiDigitW(stem[pos])) {
            ++pos;
        }
        if (pos - partStart < 2u || pos != stem.size()) {
            return false;
        }
    }

    outSessionKey = canonicalKey;
    return true;
}

RotatingTelemetryLogger::~RotatingTelemetryLogger() {
    Close();
}

bool RotatingTelemetryLogger::Open(const RotatingLoggerOptions& opts, std::string& outError) {
    Close();
    opts_ = opts;
    if (opts_.maxRetainedSessions == 0u) {
        opts_.maxRetainedSessions = 1u;
    }
    if (opts_.maxBytesPerPart < 512ULL) {
        opts_.maxBytesPerPart = 512ULL;
    }

    if (!opts_.explicitFilePath.empty()) {
        isExplicitFile_ = true;
        activeDirectory_ = ParentDirectoryOf(opts_.explicitFilePath);
        if (!activeDirectory_.empty() && !EnsureDirectoryRecursive(activeDirectory_)) {
            outError = "Failed to create telemetry parent directory: " + staged::WideToUtf8(activeDirectory_);
            return false;
        }
        fp_ = _wfopen(opts_.explicitFilePath.c_str(), L"wb");
        if (!fp_) {
            outError = "Failed to open explicit telemetry JSONL file: " + staged::WideToUtf8(opts_.explicitFilePath);
            return false;
        }
        activeFilePath_ = opts_.explicitFilePath;
        sessionKey_ = staged::ExtractLeafFilename(opts_.explicitFilePath);
        partIndex_ = 1u;
        currentPartBytes_ = 0ULL;
        retainedSessionsCount_ = 1u;
        return true;
    }

    isExplicitFile_ = false;
    activeDirectory_ = opts_.logDirectory.empty()
        ? ResolveDefaultLogDirectory()
        : StripTrailingSlashes(opts_.logDirectory);

    if (!EnsureDirectoryRecursive(activeDirectory_)) {
        outError = "Failed to create session log directory: " + staged::WideToUtf8(activeDirectory_);
        return false;
    }

    if (!opts_.sessionKeyOverride.empty()) {
        std::wstring parsedKey;
        if (!ParseStageESessionLogFilename(opts_.sessionKeyOverride + L".jsonl", parsedKey)) {
            outError = "Invalid sessionKeyOverride format: " + staged::WideToUtf8(opts_.sessionKeyOverride);
            return false;
        }
        sessionKey_ = parsedKey;
    } else {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const DWORD pid = GetCurrentProcessId();
        for (uint32_t seq = 1u; seq <= 999u; ++seq) {
            wchar_t keyBuf[128]{};
            std::swprintf(
                keyBuf,
                std::size(keyBuf),
                L"stage_e_session_%04u%02u%02u_%02u%02u%02u_pid%lu_s%02u",
                static_cast<unsigned>(st.wYear),
                static_cast<unsigned>(st.wMonth),
                static_cast<unsigned>(st.wDay),
                static_cast<unsigned>(st.wHour),
                static_cast<unsigned>(st.wMinute),
                static_cast<unsigned>(st.wSecond),
                static_cast<unsigned long>(pid),
                static_cast<unsigned>(seq));
            const std::wstring candidate = JoinWinPath(activeDirectory_, std::wstring(keyBuf) + L".jsonl");
            if (!FileExistsAt(candidate)) {
                sessionKey_ = keyBuf;
                break;
            }
        }
    }

    if (!OpenPartFile(1u, outError)) {
        return false;
    }

    PruneOldSessionLogs();
    return true;
}

bool RotatingTelemetryLogger::OpenPartFile(uint32_t partIndex, std::string& outError) {
    if (fp_) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }

    wchar_t suffixBuf[32]{};
    if (partIndex <= 1u) {
        std::swprintf(suffixBuf, std::size(suffixBuf), L".jsonl");
    } else {
        std::swprintf(suffixBuf, std::size(suffixBuf), L"_part%02u.jsonl", static_cast<unsigned>(partIndex));
    }

    const std::wstring fullPath = JoinWinPath(activeDirectory_, sessionKey_ + suffixBuf);
    fp_ = _wfopen(fullPath.c_str(), L"wb");
    if (!fp_) {
        outError = "Failed to open session telemetry file: " + staged::WideToUtf8(fullPath);
        return false;
    }

    activeFilePath_ = fullPath;
    partIndex_ = partIndex;
    currentPartBytes_ = 0ULL;
    return true;
}

void RotatingTelemetryLogger::PruneOldSessionLogs() noexcept {
    if (activeDirectory_.empty() || isExplicitFile_) {
        return;
    }

    const std::wstring pattern = JoinWinPath(activeDirectory_, L"stage_e_session_*.jsonl");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    std::map<std::wstring, std::vector<std::wstring>> filesBySession;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            continue;
        }
        const std::wstring leaf(fd.cFileName);
        std::wstring key;
        if (!ParseStageESessionLogFilename(leaf, key)) {
            continue;
        }
        filesBySession[key].push_back(JoinWinPath(activeDirectory_, leaf));
    } while (FindNextFileW(hFind, &fd) != FALSE);
    FindClose(hFind);

    std::vector<std::wstring> sortedKeys;
    sortedKeys.reserve(filesBySession.size());
    for (const auto& kv : filesBySession) {
        sortedKeys.push_back(kv.first);
    }
    // Descending lexicographical order puts newest YYYYMMDD_HHMMSS (and highest _sNN) first.
    std::sort(sortedKeys.begin(), sortedKeys.end(), std::greater<std::wstring>());

    // Guarantee the active session is never pruned even if system clock stepped backward.
    auto activeIt = std::find(sortedKeys.begin(), sortedKeys.end(), sessionKey_);
    if (activeIt != sortedKeys.end() && activeIt != sortedKeys.begin()) {
        const std::wstring activeCopy = *activeIt;
        sortedKeys.erase(activeIt);
        sortedKeys.insert(sortedKeys.begin(), activeCopy);
    }

    const size_t keepCount = std::min<size_t>(sortedKeys.size(), static_cast<size_t>(opts_.maxRetainedSessions));
    for (size_t idx = keepCount; idx < sortedKeys.size(); ++idx) {
        for (const auto& oldPath : filesBySession[sortedKeys[idx]]) {
            DeleteFileW(oldPath.c_str());
        }
    }
    retainedSessionsCount_ = static_cast<uint32_t>(keepCount);
}

bool RotatingTelemetryLogger::WriteLine(const std::string& jsonLine, bool allowPartSplit) {
    if (!fp_) {
        return false;
    }

    const uint64_t lineBytes = static_cast<uint64_t>(jsonLine.size()) + 1ULL;
    if (!isExplicitFile_ &&
        allowPartSplit &&
        currentPartBytes_ > 0ULL &&
        (currentPartBytes_ + lineBytes > opts_.maxBytesPerPart)) {
        std::string rotErr;
        if (!OpenPartFile(partIndex_ + 1u, rotErr)) {
            return false;
        }
    }

    const size_t written = std::fwrite(jsonLine.data(), 1u, jsonLine.size(), fp_);
    const int nl = std::fputc('\n', fp_);
    std::fflush(fp_);
    if (written != jsonLine.size() || nl == EOF) {
        return false;
    }
    currentPartBytes_ += lineBytes;
    return true;
}

void RotatingTelemetryLogger::Close() noexcept {
    if (fp_) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

std::string FormatStageETelemetryJsonLine(const staged::TimelineStatsSnapshot& snap,
                                          const staged::TelemetryLineContext& ctx,
                                          const PcmLevelSnapshot& level,
                                          const RoutingDiagnostic& routing,
                                          const BridgeDiagnosticReport& report) {
    std::string base = staged::FormatTelemetryJsonLine(snap, ctx);
    while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' ')) {
        base.pop_back();
    }
    if (base.empty() || base.back() != '}') {
        return base;
    }
    base.pop_back(); // Remove closing '}' to append E1 diagnostic fields cleanly

    char extBuf[1024]{};
    std::snprintf(
        extBuf,
        sizeof(extBuf),
        ",\"e1_utc\":\"%s\",\"e1_headline\":\"%s\",\"e1_routing\":\"%s\","
        "\"e1_routing_conflict\":%s,\"e1_peak_dbfs_1s\":%.2f,\"e1_rms_dbfs_1s\":%.2f,"
        "\"e1_nonzero_frames_1s\":%llu,\"e1_audible_frames_1s\":%llu,"
        "\"e1_continuous_silent_ms\":%llu,\"e1_session_active\":%s,"
        "\"e1_session_muted\":%s,\"e1_target_endpoint\":\"%s\","
        "\"e1_default_endpoint\":\"%s\"}",
        staged::EscapeJsonString(staged::GetCurrentUtcIsoString()).c_str(),
        staged::EscapeJsonString(PipelineHeadlineStateName(report.headline)).c_str(),
        staged::EscapeJsonString(RoutingIsolationStateName(routing.state)).c_str(),
        report.hasRoutingConflict ? "true" : "false",
        ClampFiniteJsonDb(level.peakDbfs1s),
        ClampFiniteJsonDb(level.rmsDbfs1s),
        static_cast<unsigned long long>(level.nonZeroFrames1s),
        static_cast<unsigned long long>(level.audibleFrames1s),
        static_cast<unsigned long long>(report.continuousActiveSilentMs),
        routing.targetSessionActive ? "true" : "false",
        routing.targetSessionMuted ? "true" : "false",
        staged::EscapeJsonString(TruncateUtf8Safe(routing.targetEndpointNameUtf8, 160u)).c_str(),
        staged::EscapeJsonString(TruncateUtf8Safe(routing.defaultEndpointNameUtf8, 160u)).c_str());

    base += extBuf;
    return base;
}

} // namespace stagee
