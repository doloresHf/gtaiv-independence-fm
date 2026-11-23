#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <setupapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <stdexcept>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>

namespace fs = std::filesystem;
namespace {
constexpr wchar_t title[] = L"Independence FM Setup";
constexpr wchar_t loaderUrl[] = L"https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v9.7.4";
bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
const wchar_t* T(const wchar_t* en, const wchar_t* zh) { return chinese ? zh : en; }
void Require(bool ok, const char* detail) { if (!ok) throw std::runtime_error(detail); }
int Dialog(const std::wstring& message, UINT type = MB_OK) {
    return MessageBoxW(nullptr, message.c_str(), title, type);
}
struct CloseWin { void operator()(void* h) const { CloseHandle(h); } };
using Handle = std::unique_ptr<void, CloseWin>;

fs::path Known(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    Require(SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &value)), "Windows folder lookup failed");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> holder(value, CoTaskMemFree);
    return value;
}
fs::path PickGame(const fs::path& start = {}) {
    wchar_t filename[32768]{};
    const auto folder = start.wstring();
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.lpstrTitle = T(L"Select GTAIV.exe (Steam > Manage > Browse local files)", L"选择 GTAIV.exe（Steam > 管理 > 浏览本地文件）");
    open.lpstrFilter = L"GTAIV.exe\0GTAIV.exe\0";
    open.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    open.lpstrFile = filename;
    open.nMaxFile = static_cast<DWORD>(std::size(filename));
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&open) ? fs::path(filename) : fs::path();
}
fs::path PickFolder() {
    BROWSEINFOW browse{};
    browse.lpszTitle = T(L"Select the User Music folder used by GTA IV", L"选择 GTA IV 实际使用的 User Music 文件夹");
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return {};
    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    return ok ? fs::path(path) : fs::path();
}
bool X86(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    in.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!in || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos)) return false;
    in.seekg(dos.e_lfanew);
    DWORD sig{};
    IMAGE_FILE_HEADER pe{};
    in.read(reinterpret_cast<char*>(&sig), sizeof(sig));
    in.read(reinterpret_cast<char*>(&pe), sizeof(pe));
    return in && sig == IMAGE_NT_SIGNATURE && pe.Machine == IMAGE_FILE_MACHINE_I386;
}
bool Compatible(const fs::path& game) {
    if (_wcsicmp(game.filename().c_str(), L"GTAIV.exe") || !X86(game)) return false;
    DWORD ignored{};
    DWORD size = GetFileVersionInfoSizeW(game.c_str(), &ignored);
    if (!size) return false;
    std::vector<unsigned char> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length{};
    return GetFileVersionInfoW(game.c_str(), 0, size, data.data())
        && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length)
        && length >= sizeof(*info) && info->dwFileVersionMS == MAKELONG(2, 1)
        && info->dwFileVersionLS == MAKELONG(59, 0);
}
bool Running() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    Require(snapshot != INVALID_HANDLE_VALUE, "Could not check running processes");
    Handle holder(snapshot);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    Require(Process32FirstW(snapshot, &entry) != FALSE, "Could not enumerate running processes");
    do {
        for (auto name : {L"GTAIV.exe", L"iv_apple_music_radio.exe"})
            if (!_wcsicmp(entry.szExeFile, name)) return true;
    } while (Process32NextW(snapshot, &entry));
    return false;
}
std::wstring Digest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Require(in.good(), "Could not read a file for verification");
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "SHA-256 unavailable");
    struct Guard {
        BCRYPT_ALG_HANDLE algorithm;
        BCRYPT_HASH_HANDLE& hash;
        ~Guard() { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); }
    } guard{algorithm, hash};
    Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "SHA-256 setup failed");
    std::array<unsigned char, 65536> data{};
    while (in.read(reinterpret_cast<char*>(data.data()), data.size()) || in.gcount()) {
        Require(BCryptHashData(hash, data.data(), static_cast<ULONG>(in.gcount()), 0) >= 0, "SHA-256 read failed");
    }
    Require(in.eof() && !in.bad(), "File read failed");
    std::array<unsigned char, 32> digest{};
    Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA-256 failed");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
struct Layout {
    fs::path plugins, music, app;
    std::array<fs::path, 6> Targets(bool usesBridgeAsi = true) const {
        return {plugins / (usesBridgeAsi ? L"IVRadioBridge.asi" : L"IVRadioProbe.asi"),
                plugins / (usesBridgeAsi ? L"IVRadioBridge.ini" : L"IVRadioProbe.ini"),
                music / L"IV_Radio_Stream_01.mp3", music / L"IV_Radio_Stream_02.mp3",
                music / L"IV_Radio_Stream_03.mp3", app / L"iv_apple_music_radio.exe"};
    }
};
struct Receipt {
    Layout layout;
    bool usesBridgeAsi = true;
    std::array<std::wstring, 6> hashes;
};
// Endpoints may be deliberately redirected (OneDrive). Resolve the selected directory
// before recording it and reject a managed path that is itself a reparse point.
void CheckFile(const fs::path& file) {
    const DWORD attrs = GetFileAttributesW(file.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        Require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "Could not inspect destination");
        return;
    }
    Require(!(attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "A managed path is not a regular file");
}
bool IsHash(const std::wstring& string) {
    return string.size() == 64 && string.find_first_not_of(L"0123456789abcdef") == std::wstring::npos;
}
void Store(const fs::path& path, const Receipt& receipt) {
    const fs::path temp = path.wstring() + L".pending";
    DeleteFileW(temp.c_str());
    const auto payload = [&] {
        auto value = std::wstring(receipt.usesBridgeAsi ? L"IndependenceFM-v3\n" : L"IndependenceFM-v2\n") +
            receipt.layout.plugins.wstring() + L"\n" +
            receipt.layout.music.wstring() + L"\n" + std::to_wstring(receipt.hashes.size()) + L"\n";
        for (const auto& hash : receipt.hashes) value += hash + L"\n";
        return value;
    }();
    Handle out(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (out.get() == INVALID_HANDLE_VALUE) { out.release(); throw std::runtime_error("Could not start installation receipt"); }
    const auto length = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    DWORD written{};
    bool ok = WriteFile(out.get(), payload.data(), length, &written, nullptr) && written == length && FlushFileBuffers(out.get());
    out.reset();
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Could not write installation receipt");
    }
}
Receipt Load(const fs::path& path, const fs::path& app) {
    CheckFile(path);
    auto length = fs::file_size(path);
    Require(length <= 140000 && length % sizeof(wchar_t) == 0, "Invalid installation receipt");
    std::ifstream in(path, std::ios::binary);
    std::wstring data(static_cast<size_t>(length / sizeof(wchar_t)), L'\0');
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(length));
    Require(in.good() && data.find(L'\0') == std::wstring::npos, "Invalid installation receipt");
    size_t cursor{};
    auto line = [&]() {
        size_t next = data.find(L'\n', cursor);
        Require(next != std::wstring::npos, "Truncated installation receipt");
        auto result = data.substr(cursor, next - cursor);
        cursor = next + 1;
        return result;
    };
    const auto header = line();
    const bool isV1 = (header == L"IndependenceFM-v1");
    const bool isV2 = (header == L"IndependenceFM-v2");
    const bool isV3 = (header == L"IndependenceFM-v3");
    Require(isV1 || isV2 || isV3, "Unrecognized installation receipt");
    Receipt result;
    result.usesBridgeAsi = isV3;
    result.layout = {fs::path(line()), fs::path(line()), app};
    Require(result.layout.plugins.is_absolute() && result.layout.music.is_absolute() &&
            result.layout.plugins.filename() == L"plugins", "Invalid installation paths");
    if (isV2 || isV3) {
        Require(line() == std::to_wstring(result.hashes.size()), "Unexpected installation receipt file count");
    }
    for (auto& hash : result.hashes) {
        hash = line();
        Require(IsHash(hash), "Invalid installation hash");
    }
    return result;
}
void VerifyInstalled(const Receipt& receipt, bool fresh) {
    for (const auto& directory : {receipt.layout.plugins, receipt.layout.music, receipt.layout.app})
        Require(fs::weakly_canonical(directory) == directory, "An installation directory has moved or redirects elsewhere");
    const auto targets = receipt.layout.Targets(receipt.usesBridgeAsi);
    for (size_t i = 0; i < targets.size(); ++i) {
        CheckFile(targets[i]);
        if (fs::exists(targets[i])) {
            Require(!fresh, "An existing file has no installation receipt. Move it aside manually first.");
            Require(Digest(targets[i]) == receipt.hashes[i], "A managed file was edited. Back it up and move it aside before continuing.");
        }
    }
    if (fresh && receipt.usesBridgeAsi) {
        const fs::path legacyProbe = receipt.layout.plugins / L"IVRadioProbe.asi";
        CheckFile(legacyProbe);
        Require(!fs::exists(legacyProbe), "An existing IVRadioProbe.asi has no installation receipt. Move it aside manually first.");
    }
}
void Install(const Receipt& receipt, const fs::path& payload) {
    const auto targets = receipt.layout.Targets(receipt.usesBridgeAsi);
    std::array<fs::path, 6> sources{};
    for (size_t i = 0; i < targets.size(); ++i) {
        sources[i] = payload / targets[i].filename();
        CheckFile(sources[i]);
        Require(Digest(sources[i]) == receipt.hashes[i], "The package differs from the original installation");
    }
    std::vector<size_t> created;
    created.reserve(targets.size());
    try {
        for (size_t i = 0; i < targets.size(); ++i) {
            if (fs::exists(targets[i])) continue;
            fs::create_directories(targets[i].parent_path());
            const auto temp = targets[i].wstring() + L".independence-fm-installing";
            if (fs::exists(temp)) {
                CheckFile(temp);
                Require(Digest(temp) == receipt.hashes[i], "A previous temporary file needs manual recovery");
            } else {
                Require(CopyFileW(sources[i].c_str(), temp.c_str(), TRUE) != FALSE, "Copy failed. Check permissions and free disk space.");
            }
            if (Digest(temp) != receipt.hashes[i] ||
                !MoveFileExW(temp.c_str(), targets[i].c_str(), MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temp.c_str());
                throw std::runtime_error("Could not finish writing an installation file");
            }
            created.push_back(i);
        }
    } catch (...) {
        for (size_t i : created) {
            try { if (Digest(targets[i]) == receipt.hashes[i]) DeleteFileW(targets[i].c_str()); } catch (...) {}
        }
        throw;
    }
}
void Upgrade(const Receipt& oldReceipt, const Receipt& newReceipt, const fs::path& payload, const fs::path& record) {
    VerifyInstalled(oldReceipt, false);
    const auto oldTargets = oldReceipt.layout.Targets(oldReceipt.usesBridgeAsi);
    const auto newTargets = newReceipt.layout.Targets(newReceipt.usesBridgeAsi);
    std::array<fs::path, 6> sources{};
    for (size_t i = 0; i < newTargets.size(); ++i) {
        sources[i] = payload / newTargets[i].filename();
        CheckFile(sources[i]);
        Require(Digest(sources[i]) == newReceipt.hashes[i], "The package differs from the upgrade manifest");
    }
    auto inList = [](const std::array<fs::path, 6>& list, const fs::path& p) {
        return std::find(list.begin(), list.end(), p) != list.end();
    };
    if (oldReceipt.usesBridgeAsi != newReceipt.usesBridgeAsi) {
        for (const auto& nt : newTargets) {
            if (!inList(oldTargets, nt)) {
                CheckFile(nt);
                Require(!fs::exists(nt), "A destination plugin file already exists outside the installation receipt. Move it aside manually first.");
            }
        }
    }
    struct UndoStep {
        fs::path target;
        fs::path backup;
        bool hadExisting = false;
    };
    std::vector<UndoStep> steps;
    steps.reserve(oldTargets.size() + newTargets.size());
    try {
        for (const auto& ot : oldTargets) {
            if (!inList(newTargets, ot) && fs::exists(ot)) {
                CheckFile(ot);
                const fs::path retired = ot.wstring() + L".independence-fm-retired";
                DeleteFileW(retired.c_str());
                Require(MoveFileExW(ot.c_str(), retired.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
                        "Could not retire legacy plugin file during upgrade");
                steps.push_back(UndoStep{ot, retired, true});
            }
        }
        for (size_t i = 0; i < newTargets.size(); ++i) {
            CheckFile(newTargets[i]);
            if (fs::exists(newTargets[i]) && Digest(newTargets[i]) == newReceipt.hashes[i]) continue;
            fs::create_directories(newTargets[i].parent_path());
            const fs::path temp = newTargets[i].wstring() + L".independence-fm-installing";
            const fs::path backup = newTargets[i].wstring() + L".independence-fm-backup";
            DeleteFileW(temp.c_str());
            Require(CopyFileW(sources[i].c_str(), temp.c_str(), FALSE) != FALSE,
                    "Copy failed during upgrade. Check permissions and free disk space.");
            if (Digest(temp) != newReceipt.hashes[i]) {
                DeleteFileW(temp.c_str());
                throw std::runtime_error("Upgrade staging verification failed");
            }
            UndoStep step{newTargets[i], backup, fs::exists(newTargets[i])};
            if (step.hadExisting) {
                DeleteFileW(backup.c_str());
                if (!MoveFileExW(newTargets[i].c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    DeleteFileW(temp.c_str());
                    throw std::runtime_error("Could not back up existing file during upgrade");
                }
            }
            if (!MoveFileExW(temp.c_str(), newTargets[i].c_str(), MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temp.c_str());
                if (step.hadExisting) MoveFileExW(backup.c_str(), newTargets[i].c_str(), MOVEFILE_REPLACE_EXISTING);
                throw std::runtime_error("Could not replace file during upgrade");
            }
            steps.push_back(std::move(step));
        }
        Store(record, newReceipt);
        for (const auto& step : steps) {
            if (step.hadExisting) DeleteFileW(step.backup.c_str());
        }
    } catch (...) {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            try {
                if (it->hadExisting && fs::exists(it->backup)) {
                    MoveFileExW(it->backup.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                } else {
                    DeleteFileW(it->target.c_str());
                }
            } catch (...) {}
        }
        throw;
    }
}
bool HasSteamVirtualSinkDevice() {
    constexpr GUID kMediaClass = {0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO hDev = SetupDiGetClassDevsW(&kMediaClass, L"ROOT", nullptr, 0u);
    if (hDev == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDev, i, &data); ++i) {
        wchar_t hwIds[1024]{};
        DWORD req = 0;
        if (SetupDiGetDeviceRegistryPropertyW(hDev, &data, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds), &req)) {
            const wchar_t* p = hwIds;
            while (*p) {
                if (!_wcsicmp(p, L"ROOT\\SteamStreamingSpeakers")) { found = true; break; }
                p += wcslen(p) + 1;
            }
        }
        if (found) break;
        data.cbSize = sizeof(data);
    }
    SetupDiDestroyDeviceInfoList(hDev);
    return found;
}
void RunHiddenWait(const fs::path& exe, const std::wstring& args) {
    if (!fs::exists(exe)) return;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000u);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
void RunElevatedWait(const fs::path& exe, const std::wstring& args) {
    if (!fs::exists(exe)) return;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000u);
        CloseHandle(sei.hProcess);
    }
}
std::wstring List(const Receipt& receipt) {
    std::wstring list;
    for (const auto& path : receipt.layout.Targets(receipt.usesBridgeAsi)) list += L"\n" + path.wstring();
    return list;
}
void Uninstall(const Receipt& receipt, const fs::path& record) {
    VerifyInstalled(receipt, false);
    for (const auto& path : receipt.layout.Targets(receipt.usesBridgeAsi))
        if (fs::exists(path))
            Require(DeleteFileW(path.c_str()) != FALSE, "Removal interrupted. Run Setup again to finish.");
    Require(DeleteFileW(record.c_str()) != FALSE, "Could not remove the installation receipt");
}

int SelfTest(const fs::path& parent) {
    const auto unique = std::wstring(L"setup-test-") + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const auto root = fs::canonical(parent) / unique;
    Require(fs::create_directory(root), "Could not create test sandbox");
    Receipt fixture;
    fixture.usesBridgeAsi = true;
    fixture.layout = {root / L"plugins", root / L"用户音乐", root / L"app"};
    const auto payloadV3 = root / L"payload_v3";
    const auto payloadLegacy = root / L"payload_legacy";
    const auto payloadV3Alt = root / L"payload_v3_alt";
    fs::create_directories(payloadV3);
    fs::create_directories(payloadLegacy);
    fs::create_directories(payloadV3Alt);
    fs::create_directories(fixture.layout.app);
    fs::create_directories(fixture.layout.plugins);
    fs::create_directories(fixture.layout.music);
    const auto targetsV3 = fixture.layout.Targets(true);
    for (size_t i = 0; i < targetsV3.size(); ++i) {
        std::ofstream(payloadV3 / targetsV3[i].filename()) << "v3 fixture " << i;
        fixture.hashes[i] = Digest(payloadV3 / targetsV3[i].filename());
    }
    const auto playerSong = fixture.layout.music / L"existing-song.mp3";
    std::ofstream(playerSong) << "user file";

    // Guard 1: Unmanaged IVRadioBridge.asi blocks fresh v3 install
    std::ofstream(targetsV3.front()) << "unmanaged bridge";
    bool rejected = false;
    try { VerifyInstalled(fixture, true); } catch (const std::exception&) { rejected = true; }
    Require(rejected && fs::exists(targetsV3.front()), "Unmanaged IVRadioBridge.asi collision was not protected");
    fs::remove(targetsV3.front());

    // Guard 2: Unmanaged legacy IVRadioProbe.asi blocks fresh v3 install
    const fs::path unmanagedLegacyProbe = fixture.layout.plugins / L"IVRadioProbe.asi";
    std::ofstream(unmanagedLegacyProbe) << "unmanaged probe";
    rejected = false;
    try { VerifyInstalled(fixture, true); } catch (const std::exception&) { rejected = true; }
    Require(rejected && fs::exists(unmanagedLegacyProbe), "Unmanaged legacy IVRadioProbe.asi was not protected");
    fs::remove(unmanagedLegacyProbe);

    // Fresh v3 install, repair, modified-file guard, and clean uninstall
    VerifyInstalled(fixture, true);
    const auto record = fixture.layout.app / L"install.receipt";
    Store(record, fixture);
    auto loaded = Load(record, fixture.layout.app);
    Require(loaded.usesBridgeAsi, "Expected v3 receipt flag on load");
    Install(loaded, payloadV3);
    VerifyInstalled(loaded, false);
    Require(std::all_of(targetsV3.begin(), targetsV3.end(), [](const auto& path) { return fs::exists(path); }),
            "Install did not place all v3 files");
    fs::remove(targetsV3[2]);
    Install(loaded, payloadV3);
    Require(fs::exists(targetsV3[2]), "Repair did not restore the missing file");
    std::ofstream(targetsV3.front(), std::ios::app) << "modified";
    rejected = false;
    try { Uninstall(loaded, record); } catch (const std::exception&) { rejected = true; }
    Require(rejected && fs::exists(targetsV3.front()) && fs::exists(record), "Changed file was not protected");
    fs::remove(targetsV3.front());
    Uninstall(loaded, record);
    Require(std::none_of(targetsV3.begin(), targetsV3.end(), [](const auto& path) { return fs::exists(path); })
            && fs::exists(playerSong) && !fs::exists(record), "Uninstall changed unrelated or left managed files");

    // Legacy v1 -> v3 and v2 -> v3 upgrades (retire IVRadioProbe.{asi,ini} and install IVRadioBridge.{asi,ini})
    Receipt legacyReceipt;
    legacyReceipt.usesBridgeAsi = false;
    legacyReceipt.layout = fixture.layout;
    const auto targetsLegacy = legacyReceipt.layout.Targets(false);
    for (size_t i = 0; i < targetsLegacy.size(); ++i) {
        std::ofstream(payloadLegacy / targetsLegacy[i].filename()) << "legacy fixture " << i;
        legacyReceipt.hashes[i] = Digest(payloadLegacy / targetsLegacy[i].filename());
    }
    const std::wstring v1Payload = [&] {
        std::wstring s = L"IndependenceFM-v1\n" + legacyReceipt.layout.plugins.wstring() + L"\n" + legacyReceipt.layout.music.wstring() + L"\n";
        for (const auto& h : legacyReceipt.hashes) s += h + L"\n";
        return s;
    }();
    {
        std::ofstream recOut(record, std::ios::binary);
        recOut.write(reinterpret_cast<const char*>(v1Payload.data()),
                     static_cast<std::streamsize>(v1Payload.size() * sizeof(wchar_t)));
    }
    auto loadedV1 = Load(record, fixture.layout.app);
    Require(!loadedV1.usesBridgeAsi, "Legacy v1 receipt must set usesBridgeAsi=false");
    Install(loadedV1, payloadLegacy);
    Upgrade(loadedV1, fixture, payloadV3, record);
    auto reloadedV3FromV1 = Load(record, fixture.layout.app);
    Require(reloadedV3FromV1.usesBridgeAsi && reloadedV3FromV1.hashes == fixture.hashes,
            "v1 -> v3 upgrade did not save IndependenceFM-v3 receipt");
    Require(!fs::exists(targetsLegacy[0]) && !fs::exists(targetsLegacy[1]),
            "v1 -> v3 upgrade did not retire legacy IVRadioProbe files");
    Require(fs::exists(targetsV3[0]) && fs::exists(targetsV3[1]) && fs::exists(playerSong),
            "v1 -> v3 upgrade missing IVRadioBridge files or affected unrelated song");
    Uninstall(reloadedV3FromV1, record);

    // Legacy v2 -> v3 upgrade + v3 modified-file protection during upgrade
    Store(record, legacyReceipt);
    auto loadedV2 = Load(record, fixture.layout.app);
    Require(!loadedV2.usesBridgeAsi, "Legacy v2 receipt must set usesBridgeAsi=false");
    Install(loadedV2, payloadLegacy);
    Upgrade(loadedV2, fixture, payloadV3, record);
    auto reloadedV3 = Load(record, fixture.layout.app);
    Require(reloadedV3.usesBridgeAsi && !fs::exists(targetsLegacy[0]) && !fs::exists(targetsLegacy[1]),
            "v2 -> v3 upgrade did not replace IVRadioProbe with IVRadioBridge");

    Receipt altV3Receipt = fixture;
    for (size_t i = 0; i < targetsV3.size(); ++i) {
        if (i < 2u || i == 5u) {
            std::ofstream(payloadV3Alt / targetsV3[i].filename()) << "v3 alt " << i;
        } else {
            std::ofstream(payloadV3Alt / targetsV3[i].filename()) << "v3 fixture " << i;
        }
        altV3Receipt.hashes[i] = Digest(payloadV3Alt / targetsV3[i].filename());
    }
    std::ofstream(targetsV3[1], std::ios::app) << "user-edit";
    const auto editedDigest = Digest(targetsV3[1]);
    rejected = false;
    try { Upgrade(reloadedV3, altV3Receipt, payloadV3Alt, record); } catch (const std::exception&) { rejected = true; }
    Require(rejected && Digest(targetsV3[1]) == editedDigest && Load(record, fixture.layout.app).hashes == fixture.hashes,
            "Modified file was overwritten or receipt mutated during blocked v3 upgrade");
    fs::remove(targetsV3[1]);
    Upgrade(reloadedV3, altV3Receipt, payloadV3Alt, record);
    Uninstall(altV3Receipt, record);
    Require(!fs::exists(record) && fs::exists(playerSong), "Clean uninstall after v3 upgrade failed");

    std::ofstream(root / L"PASS.txt") << "v3 install, collision guards, repair, v1/v2-to-v3 upgrade, modified-file protection and uninstall passed";
    return 0;
}

int Run() {
    Handle lock(CreateMutexW(nullptr, TRUE, L"Local\\IndependenceFM_Setup"));
    Require(lock && GetLastError() != ERROR_ALREADY_EXISTS, "Setup is already running");
    Require(!Running(), "Close GTA IV and exit Independence FM from the tray before continuing.");
    wchar_t module[32768]{};
    DWORD len = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    Require(len && len < std::size(module), "Cannot locate the extracted setup");
    const fs::path payload = fs::path(module).parent_path() / L"payload";
    const fs::path app = fs::weakly_canonical(Known(FOLDERID_LocalAppData) / L"GTAIV-Independence-FM");
    const fs::path record = app / L"install.receipt";
    CheckFile(record);
    const bool existing = fs::exists(record);
    Receipt receipt;
    Receipt targetReceipt;
    bool upgradeMode = false;
    if (existing) {
        receipt = Load(record, app);
        targetReceipt.layout = receipt.layout;
        targetReceipt.usesBridgeAsi = true;
        const auto targets = targetReceipt.layout.Targets(targetReceipt.usesBridgeAsi);
        for (size_t i = 0; i < targets.size(); ++i) targetReceipt.hashes[i] = Digest(payload / targets[i].filename());
        upgradeMode = (!receipt.usesBridgeAsi) || (targetReceipt.hashes != receipt.hashes);
        const int action = upgradeMode
            ? Dialog(T(L"Existing installation detected.\n\nYes: Upgrade to new version (keep paths)\nNo: Uninstall\nCancel: Close",
                       L"检测到已安装旧版本。\n\n是：原地升级到新版本（保留当前路径）\n否：卸载\n取消：关闭"), MB_YESNOCANCEL | MB_ICONQUESTION)
            : Dialog(T(L"Yes: Repair installation\nNo: Uninstall\nCancel: Close", L"是：修复安装\n否：卸载\n取消：关闭"), MB_YESNOCANCEL);
        if (action == IDCANCEL) return 0;
        VerifyInstalled(receipt, false);
        if (action == IDNO) {
            if (Dialog(T(L"Remove these mod files?", L"移除以下 Mod 文件？") + List(receipt), MB_OKCANCEL) != IDOK) return 0;
            Require(!Running(), "Close GTA IV and the tray application before uninstalling.");
            const fs::path installedLauncher = receipt.layout.Targets(receipt.usesBridgeAsi).back();
            const fs::path payloadLauncher = payload / L"iv_apple_music_radio.exe";
            const fs::path helperLauncher = fs::exists(payloadLauncher) ? payloadLauncher : installedLauncher;
            RunHiddenWait(helperLauncher, L"--restore-routes --quiet --config-path \"" + (app / L"iv_apple_music_radio.settings.json").wstring() + L"\"");
            if (HasSteamVirtualSinkDevice()) {
                if (Dialog(T(L"Also uninstall the Steam Streaming Speakers virtual audio driver?\n\nChoose No if you use Steam Remote Play or want to keep this virtual speaker.",
                             L"是否一并卸载 Steam Streaming Speakers 虚拟静音驱动？\n\n如果您平时使用 Steam 远程畅玩 (Remote Play) 或想保留该虚拟声卡，请选“否”。"),
                           MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    RunElevatedWait(helperLauncher, L"--uninstall-virtual-sink --quiet");
                }
            }
            Uninstall(receipt, record);
            Dialog(T(L"Uninstalled (app audio routes restored). Re-scan music in GTA IV to refresh the track list.",
                     L"已卸载（并自动恢复受管应用的音频路由）。请在 GTA IV 重新扫描音乐以刷新列表。"));
            return 0;
        }
    } else {
        const auto game = PickGame();
        if (game.empty()) return 0;
        Require(Compatible(game), "Select the 32-bit GTA IV Complete Edition 1.2.0.59 executable.");
        auto root = fs::canonical(game).parent_path();
        if (!X86(root / L"dinput8.dll")) {
            if (Dialog(T(L"Install the official Win32 ASI Loader (dinput8.dll) beside GTAIV.exe, then reopen Setup. Open the official download page?",
                         L"请先将官方 Win32 ASI Loader 的 dinput8.dll 安装到 GTAIV.exe 同目录，再重试。打开官方下载页？"),
                       MB_YESNO | MB_ICONINFORMATION) == IDYES)
                ShellExecuteW(nullptr, L"open", loaderUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        auto music = Known(FOLDERID_Documents) / L"Rockstar Games" / L"GTA IV" / L"User Music";
        int choice = Dialog(T(L"Use this User Music folder?\n\n", L"使用此 User Music 目录？\n\n") + music.wstring() +
            T(L"\n\nChoose No to browse.", L"\n\n选“否”浏览其他位置。"), MB_YESNOCANCEL);
        if (choice == IDCANCEL) return 0;
        if (choice == IDNO) music = PickFolder();
        if (music.empty()) return 0;
        receipt.usesBridgeAsi = true;
        receipt.layout = {fs::weakly_canonical(root / L"plugins"), fs::weakly_canonical(music), app};
        Require(receipt.layout.plugins.parent_path() == root, "The plugins directory redirects outside the game folder");
        VerifyInstalled(receipt, true);
        const auto targets = receipt.layout.Targets(receipt.usesBridgeAsi);
        for (size_t i = 0; i < targets.size(); ++i) receipt.hashes[i] = Digest(payload / targets[i].filename());
        targetReceipt = receipt;
    }
    const std::wstring confirmPrompt = upgradeMode
        ? T(L"Upgrade these files?", L"升级以下文件？")
        : T(L"Install or repair these files?", L"安装或修复以下文件？");
    if (Dialog(confirmPrompt + List(targetReceipt), MB_OKCANCEL) != IDOK) return 0;
    Require(!Running(), "Close GTA IV and the tray application before continuing.");
    if (!existing) {
        VerifyInstalled(receipt, true);
        fs::create_directories(app);
        Store(record, targetReceipt);
        Install(targetReceipt, payload);
    } else if (upgradeMode) {
        Upgrade(receipt, targetReceipt, payload, record);
    } else {
        VerifyInstalled(receipt, false);
        Install(receipt, payload);
    }
    const auto launcher = targetReceipt.layout.Targets(targetReceipt.usesBridgeAsi).back();
    Dialog(T(L"Ready!\n\nStart your audio source, check 'Options -> Settings -> Zero-Hardware Silent Isolation' in the tray app, then run a Complete Scan in GTA IV and tune to Independence FM.\n\nSet GTA IV Audio -> Independence FM Mode to Radio for Apple Music DJ/ads, ESC pause, and B/N hotkey sync (or Sequential for nonstop streaming).",
             L"部署完成！\n\n播放音源后启动托盘程序，可在『选项 -> 设置』查看「无硬件静音隔离」与「电台模式联动」，进 GTA IV 执行「完整扫描音乐」并切到 Independence FM。\n\n将游戏声音设置中的 Independence FM 设为 Radio（电台）即可联动原版 DJ/广告、ESC 暂停与 B/N 切歌（顺序模式亦可无缝收听）。"));
    ShellExecuteW(nullptr, L"open", launcher.c_str(), nullptr, app.c_str(), SW_SHOWNORMAL);
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result{};
    int argc{};
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> args(CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
    const bool test = args && argc == 3 && std::wstring(args.get()[1]) == L"--selftest";
    try { result = test ? SelfTest(args.get()[2]) : Run(); }
    catch (const std::exception& error) {
        const std::string detail = error.what();
        if (!test) Dialog(T(L"Setup stopped.\n\n", L"安装程序已停止。\n\n") +
            std::wstring(detail.begin(), detail.end()), MB_OK | MB_ICONERROR);
        result = 1;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
