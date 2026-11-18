#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
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
    std::array<fs::path, 6> Targets() const {
        return {plugins / L"IVRadioProbe.asi", plugins / L"IVRadioProbe.ini",
                music / L"IV_Radio_Stream_01.mp3", music / L"IV_Radio_Stream_02.mp3",
                music / L"IV_Radio_Stream_03.mp3", app / L"iv_apple_music_radio.exe"};
    }
};
struct Receipt {
    Layout layout;
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
    const auto payload = [&] {
        auto value = std::wstring(L"IndependenceFM-v1\n") + receipt.layout.plugins.wstring() + L"\n" +
            receipt.layout.music.wstring() + L"\n";
        for (const auto& hash : receipt.hashes) value += hash + L"\n";
        return value;
    }();
    Handle out(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (out.get() == INVALID_HANDLE_VALUE) { out.release(); throw std::runtime_error("Could not start installation receipt"); }
    const auto length = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    DWORD written{};
    bool ok = WriteFile(out.get(), payload.data(), length, &written, nullptr) && written == length && FlushFileBuffers(out.get());
    out.reset();
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
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
    Require(line() == L"IndependenceFM-v1", "Unrecognized installation receipt");
    Receipt result;
    result.layout = {fs::path(line()), fs::path(line()), app};
    Require(result.layout.plugins.is_absolute() && result.layout.music.is_absolute() &&
            result.layout.plugins.filename() == L"plugins", "Invalid installation paths");
    for (auto& hash : result.hashes) {
        hash = line();
        Require(IsHash(hash), "Invalid installation hash");
    }
    return result;
}
void VerifyInstalled(const Receipt& receipt, bool fresh) {
    for (const auto& directory : {receipt.layout.plugins, receipt.layout.music, receipt.layout.app})
        Require(fs::weakly_canonical(directory) == directory, "An installation directory has moved or redirects elsewhere");
    const auto targets = receipt.layout.Targets();
    for (size_t i = 0; i < targets.size(); ++i) {
        CheckFile(targets[i]);
        if (fs::exists(targets[i])) {
            Require(!fresh, "An existing file has no installation receipt. Move it aside manually first.");
            Require(Digest(targets[i]) == receipt.hashes[i], "A managed file was edited. Back it up and move it aside before continuing.");
        }
    }
}
void Install(const Receipt& receipt, const fs::path& payload) {
    const auto targets = receipt.layout.Targets();
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
std::wstring List(const Receipt& receipt) {
    std::wstring list;
    for (const auto& path : receipt.layout.Targets()) list += L"\n" + path.wstring();
    return list;
}
void Uninstall(const Receipt& receipt, const fs::path& record) {
    VerifyInstalled(receipt, false);
    for (const auto& path : receipt.layout.Targets())
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
    fixture.layout = {root / L"plugins", root / L"用户音乐", root / L"app"};
    const auto payload = root / L"payload";
    fs::create_directories(payload);
    fs::create_directories(fixture.layout.app);
    fs::create_directories(fixture.layout.plugins);
    fs::create_directories(fixture.layout.music);
    const auto targets = fixture.layout.Targets();
    for (size_t i = 0; i < targets.size(); ++i) {
        std::ofstream(payload / targets[i].filename()) << "fixture " << i;
        fixture.hashes[i] = Digest(payload / targets[i].filename());
    }
    const auto playerSong = fixture.layout.music / L"existing-song.mp3";
    std::ofstream(playerSong) << "user file";
    std::ofstream(targets.front()) << "unmanaged";
    bool rejected = false;
    try { VerifyInstalled(fixture, true); } catch (const std::exception&) { rejected = true; }
    Require(rejected && fs::exists(targets.front()), "Unmanaged collision was not protected");
    fs::remove(targets.front());
    VerifyInstalled(fixture, true);
    const auto record = fixture.layout.app / L"install.receipt";
    Store(record, fixture);
    auto loaded = Load(record, fixture.layout.app);
    Install(loaded, payload);
    VerifyInstalled(loaded, false);
    Require(std::all_of(targets.begin(), targets.end(), [](const auto& path) { return fs::exists(path); }),
            "Install did not place all files");
    fs::remove(targets[2]);
    Install(loaded, payload);
    Require(fs::exists(targets[2]), "Repair did not restore the missing file");
    std::ofstream(targets.front(), std::ios::app) << "modified";
    rejected = false;
    try { Uninstall(loaded, record); } catch (const std::exception&) { rejected = true; }
    Require(rejected && fs::exists(targets.front()) && fs::exists(record), "Changed file was not protected");
    fs::remove(targets.front());
    Uninstall(loaded, record);
    Require(std::none_of(targets.begin(), targets.end(), [](const auto& path) { return fs::exists(path); })
            && fs::exists(playerSong) && !fs::exists(record), "Uninstall changed unrelated or left managed files");
    std::ofstream(root / L"PASS.txt") << "install, conflict, repair, modified-file protection and uninstall passed";
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
    if (existing) {
        receipt = Load(record, app);
        int action = Dialog(T(L"Yes: Repair installation\nNo: Uninstall\nCancel: Close", L"是：修复安装\n否：卸载\n取消：关闭"), MB_YESNOCANCEL);
        if (action == IDCANCEL) return 0;
        VerifyInstalled(receipt, false);
        if (action == IDNO) {
            if (Dialog(T(L"Remove these mod files?", L"移除以下 Mod 文件？") + List(receipt), MB_OKCANCEL) != IDOK) return 0;
            Require(!Running(), "Close GTA IV and the tray application before uninstalling.");
            Uninstall(receipt, record);
            Dialog(T(L"Uninstalled. Re-scan music in GTA IV to refresh the track list.", L"已卸载。请在 GTA IV 重新扫描音乐以刷新列表。"));
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
        receipt.layout = {fs::weakly_canonical(root / L"plugins"), fs::weakly_canonical(music), app};
        Require(receipt.layout.plugins.parent_path() == root, "The plugins directory redirects outside the game folder");
        VerifyInstalled(receipt, true);
        const auto targets = receipt.layout.Targets();
        for (size_t i = 0; i < targets.size(); ++i) receipt.hashes[i] = Digest(payload / targets[i].filename());
    }
    if (Dialog(T(L"Install or repair these files?", L"安装或修复以下文件？") + List(receipt), MB_OKCANCEL) != IDOK) return 0;
    Require(!Running(), "Close GTA IV and the tray application before continuing.");
    VerifyInstalled(receipt, !existing);
    if (!existing) { fs::create_directories(app); Store(record, receipt); }
    Install(receipt, payload);
    const auto launcher = receipt.layout.Targets().back();
    Dialog(T(L"Installed. Start your audio source, use a separate active output in Volume Mixer, then run a Complete Scan in GTA IV and tune to Independence FM. See the quick-start guide in the ZIP.",
             L"安装完成。播放音源并在音量合成器指定活动副输出，再在 GTA IV 完整扫描音乐并切到 Independence FM。详见压缩包内指南。"));
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
