#include "stage_e_engine.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kUnifiedWindowTitle[] = L"GTA IV Independence FM — Audio Bridge";

constexpr UINT kWmTrayCallback = WM_APP + 10;
constexpr UINT_PTR kTimerUiRefresh = 1001;

// Main panel bottom buttons
constexpr int kIdBtnToggleBridge    = 2001;
constexpr int kIdBtnOpenMixer       = 2002;
constexpr int kIdBtnOpenLogs        = 2003;
constexpr int kIdBtnExitApp         = 2004;

// Tray context menu commands
constexpr int kIdTrayShowPanel      = 3001;
constexpr int kIdTrayToggle         = 3002;
constexpr int kIdTraySettings       = 3003;
constexpr int kIdTrayUsageGuide     = 3004;
constexpr int kIdTrayOpenMixer      = 3005;
constexpr int kIdTrayOpenLogs       = 3006;
constexpr int kIdTrayExit           = 3007;

// Top menu bar commands
constexpr int kIdMenuSettings       = 4001;
constexpr int kIdMenuToggleBridge   = 4002;
constexpr int kIdMenuOpenMixer      = 4003;
constexpr int kIdMenuOpenLogs       = 4004;
constexpr int kIdMenuMinimizeTray   = 4005;
constexpr int kIdMenuExit           = 4006;
constexpr int kIdMenuUsageGuide     = 4007;
constexpr int kIdMenuAbout          = 4008;

// Modal dialog child control IDs
constexpr int kIdDlgLangCombo       = 5001;
constexpr int kIdDlgGuideEdit       = 5002;
constexpr int kIdDlgOpenMixer       = 5003;
constexpr int kIdDlgRadioAppleMusic = 5004;
constexpr int kIdDlgRadioCustomApp  = 5005;
constexpr int kIdDlgSourceAppCombo  = 5006;
constexpr int kIdDlgBtnRefreshApps  = 5007;
constexpr int kIdDlgBtnBrowseExe    = 5008;

constexpr int kBaseClientWidth      = 620;
constexpr int kBaseClientHeight     = 468;

constexpr wchar_t kSettingsDlgClassName[] = L"GTAIV_AppleMusic_Radio_SettingsDlg";
constexpr wchar_t kGuideDlgClassName[]    = L"GTAIV_AppleMusic_Radio_GuideDlg";

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE h) noexcept : h_(h) {}
    ~ScopedHandle() { Reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    void Reset(HANDLE h = nullptr) noexcept {
        if (h_ && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
        h_ = h;
    }

    HANDLE Get() const noexcept { return h_; }
    bool IsValid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

struct UiStrings {
    const wchar_t* fontFace;
    const wchar_t* windowTitle;

    // Top menu bar
    const wchar_t* menuOptions;
    const wchar_t* menuSettings;
    const wchar_t* menuStartBridge;
    const wchar_t* menuStopBridge;
    const wchar_t* menuOpenMixer;
    const wchar_t* menuOpenLogs;
    const wchar_t* menuMinimizeTray;
    const wchar_t* menuExit;
    const wchar_t* menuHelp;
    const wchar_t* menuUsageGuide;
    const wchar_t* menuAbout;

    // Main panel buttons & footer
    const wchar_t* btnStartBridge;
    const wchar_t* btnStopBridge;
    const wchar_t* btnOpenMixer;
    const wchar_t* btnOpenLogs;
    const wchar_t* btnExitApp;
    const wchar_t* footerHint;

    // Card 2 fallback & Card 3 telemetry templates
    const char* routingDetectingTitleUtf8;
    const char* routingDetectingDetailUtf8;
    const wchar_t* levelActiveFmt;
    const wchar_t* levelSilentText;
    const wchar_t* row1ActiveFmt;
    const wchar_t* row1PollingFmt;
    const wchar_t* row1StoppedText;
    const wchar_t* row2Fmt;
    const wchar_t* row2StreamingTag;
    const wchar_t* row2IdleTag;
    const wchar_t* row3ExcludedPrefix;
    const wchar_t* row3ExcludedSuffix;
    const wchar_t* row3DefaultFmt;
    const wchar_t* row4ActiveFmt;
    const wchar_t* row4IdlePrefix;

    // Tray menu & balloon alerts
    const wchar_t* trayShowPanel;
    const wchar_t* traySettings;
    const wchar_t* trayUsageGuide;
    const wchar_t* trayOpenMixer;
    const wchar_t* trayOpenLogs;
    const wchar_t* trayExit;
    const wchar_t* balloonRoutingTitle;
    const wchar_t* balloonRoutingHintSuffix;
    const wchar_t* balloonSilenceTitle;
    const wchar_t* balloonSilenceBodyApple;
    const wchar_t* balloonSilenceBodyGenericFmt;

    // Settings dialog (Language + Stream Anything Audio Source Picker)
    const wchar_t* settingsTitle;
    const wchar_t* settingsGroupLang;
    const wchar_t* settingsLangDesc;
    const wchar_t* settingsLangAuto;
    const wchar_t* settingsLangZh;
    const wchar_t* settingsLangEn;
    const wchar_t* settingsGroupSource;
    const wchar_t* settingsRadioAppleMusic;
    const wchar_t* settingsRadioCustomApp;
    const wchar_t* settingsBtnRefreshApps;
    const wchar_t* settingsBtnBrowseExe;
    const wchar_t* settingsComboTagPlaying;
    const wchar_t* settingsComboTagIdle;
    const wchar_t* settingsComboTagPreselected;
    const wchar_t* settingsComboEmptyHint;
    const wchar_t* settingsSourceNote;
    const wchar_t* settingsBrowseDialogTitle;
    const wchar_t* settingsForbiddenTitle;
    const wchar_t* settingsForbiddenBodyFmt;
    const wchar_t* settingsAppleRedirectTitle;
    const wchar_t* settingsAppleRedirectBodyFmt;
    const wchar_t* settingsNeedTargetTitle;
    const wchar_t* settingsNeedTargetBody;
    const wchar_t* settingsConfigPathPrefix;
    const wchar_t* settingsBtnSave;
    const wchar_t* settingsBtnCancel;
    const wchar_t* settingsSaveFailedTitle;
    const wchar_t* settingsSaveFailedPrefix;

    // Usage guide & About dialogs
    const wchar_t* guideWindowTitle;
    const wchar_t* guideHeaderTitle;
    const wchar_t* guideBodyText;
    const wchar_t* guideBtnOpenMixer;
    const wchar_t* guideBtnClose;
    const wchar_t* aboutTitle;
};

const UiStrings& GetUiStrings(stagee::UiLanguage lang) noexcept {
    static const UiStrings kZh{
        L"Microsoft YaHei UI",
        kUnifiedWindowTitle,

        L"选项(&O)",
        L"设置(&S)...\tCtrl+,",
        L"启动桥接(&B)",
        L"停止桥接(&B)",
        L"打开系统音量合成器(&M)",
        L"打开日志目录(&L)",
        L"最小化到系统托盘(&T)",
        L"退出程序(&X)",
        L"帮助(&H)",
        L"使用说明(&G)...\tF1",
        L"关于(&A)...",

        L"启动桥接",
        L"停止桥接",
        L"打开音量合成器",
        L"打开日志目录",
        L"退出程序",
        L"提示：右上角 [×] 最小化驻留托盘；切换音源 (Stream Anything)、语言与查看教程请点击顶部菜单『选项 / 帮助』。",

        "正在检测音频输出拓扑...",
        "等待音频端点与会话状态更新。",
        L"实时捕获电平 (1s)    Peak: %.1f dBFS  |  RMS: %.1f dBFS",
        L"实时捕获电平 (1s)    无有效信号 (-∞ dBFS)",
        L"捕获源：%ls (PID %lu)  |  格式：%ls  |  重连：%llu  |  运行：%ls",
        L"捕获源：等待 %ls (每 350ms 自动发现)  |  运行：%ls",
        L"捕获源：未运行（点击下方『启动桥接』开启自动捕获）",
        L"游戏电台：活跃读取槽 %u (%ls)  |  水位 %.2f s  |  欠载 %llu (%llu帧)  |  冲突 %llu",
        L"游戏正在读取",
        L"等待游戏读取",
        L"多音源隔离：已自动排除同机活跃音源 [",
        L"]",
        L"多音源隔离：按进程树独立捕获 %ls，其他桌面程序与系统提示音不会混入电台",
        L"会话日志：%ls (%.1f KB，目录自动保留最近 %u 份会话)",
        L"日志目录：",

        L"显示诊断面板",
        L"设置...",
        L"使用说明...",
        L"打开系统音量合成器",
        L"打开日志目录",
        L"退出程序（安全刷空缓冲）",
        L"GTA IV 电台桥接：路由重叠警告",
        L"\n点击托盘图标打开面板并跳转音量合成器处理。",
        L"GTA IV 电台桥接：持续无声告警",
        L"请检查 Apple Music『设置 -> 播放 -> 杜比全景声』是否已关闭。",
        L"请检查 %ls 应用内音量/标签页是否被静音，或是否开启了独占音频模式 (WASAPI Exclusive / ASIO)。",

        L"设置 — GTA IV Independence FM — Audio Bridge",
        L"界面与诊断语言 (Language)",
        L"选择菜单、实时状态卡片与托盘提示的显示语言（保存后立即生效，不中断音乐播放）：",
        L"跟随系统默认 (System Default)",
        L"简体中文 (Simplified Chinese)",
        L"English (英语)",
        L"音频捕获源 — Stream Anything (Audio Capture Source)",
        L"Apple Music（智能预设：自动定位真正的发声后台 AMPLibraryAgent.exe）",
        L"自定义发声程序（仅列出当前有音频会话的程序，或浏览 .exe 指定）：",
        L"刷新会话",
        L"浏览 .exe...",
        L"[正在发声] ",
        L"[暂停/待命] ",
        L"[已指定 · 当前无会话] ",
        L"（当前无其他音频会话 — 请先让目标播放声音后点『刷新会话』，或点『浏览 .exe...』）",
        L"提示：保存后立即平滑热切换音源（不断开游戏电台缓冲）。请选择兼容的共享模式音频应用；不支持独占模式 (ASIO / WASAPI Exclusive)。",
        L"选择要接入 GTA IV 独立电台的发声程序 (.exe)",
        L"禁止选择的捕获目标",
        L"不能将『%ls』设为电台捕获源。\n\n捕获 GTA IV 游戏本体、桥接器自身或系统宿主进程（explorer.exe / svchost.exe / audiodg.exe）会导致车内电台声音无限回授啸叫或混入系统杂音。\n请选择独立的音乐播放器或浏览器程序。",
        L"已自动切换至 Apple Music 智能预设",
        L"检测到你选择了 Apple Music 相关组件（%ls）。\n\n由于 Windows 版 Apple Music 界面壳进程（AppleMusic.exe）本身不发声，已为你自动切换至『Apple Music 智能预设』模式，自动追踪真正的发声后台 AMPLibraryAgent.exe。",
        L"尚未选择自定义发声程序",
        L"请先从下拉列表中选择一个有音频会话的程序，或点击『浏览 .exe...』指定目标程序文件。",
        L"配置文件：",
        L"保存",
        L"取消",
        L"保存设置失败",
        L"无法写入配置文件，本次修改仅在当前运行期间生效：\n",

        L"使用说明 — GTA IV Independence FM — Audio Bridge",
        L"GTA IV Independence FM — Audio Bridge 快速上手说明",
        L"【1. 三步开始收听（支持 Apple Music 与兼容的 Windows 音频应用）】\r\n"
        L"  ① 运行本程序 iv_apple_music_radio.exe（默认自动开启桥接并驻留系统托盘）。\r\n"
        L"  ② 默认自动捕获 Windows 版 Apple Music；若需接入其他兼容的音频程序，请打开『选项 -> 设置 (Ctrl+,)』切换至『自定义发声程序 (Stream Anything)』。\r\n"
        L"  ③ 在 GTA IV 的声音设置中执行完整音乐扫描，驾驶车辆并将车载电台切至『Independence FM』即可收听。\r\n\r\n"
        L"【2. 双音频设备防重音（必设，消除车外双重音乐声）】\r\n"
        L"  • 若发声程序与 GTA IV 共用同一耳机/音箱，你既会听到桌面直出的原声，又会听到游戏车内的电台声。\r\n"
        L"  • 解决方法：点击下方『打开系统音量合成器』，保持 GTA IV 为默认耳机/音箱，将音源指向活动的副输出（如 HDMI 或 USB 音频）。保持 Windows 中应用未静音，使用物理上不出声的副输出。\r\n\r\n"
        L"【3. 音频格式注意事项（杜比全景声与独占模式）】\r\n"
        L"  • Apple Music 用户：请在『设置 -> 播放 -> 杜比全景声 (Dolby Atmos)』中设为『关闭』。\r\n"
        L"  • 自定义播放器用户：请使用默认共享音频输出（Shared WASAPI / DirectSound），切勿开启 WASAPI 独占 (Exclusive) 或 ASIO（独占流绕过系统混音器，无法被按进程环回捕获）。\r\n\r\n"
        L"【4. 缓冲延迟与切歌注意事项】\r\n"
        L"  • 初次切入电台会有约 5.3 秒静音握手，游戏缓冲使切源或切歌延迟数秒，载体交接时也可能短暂静音。\r\n"
        L"  • 切歌/暂停请直接操作音乐程序或使用键盘多媒体键；请勿在游戏内按 B / N 键切歌（会触发游戏重置载体并静音 5.3 秒）。\r\n\r\n"
        L"【5. 状态指示灯与托盘说明】\r\n"
        L"  • 绿色：车内正常供音且音频路由已隔离  |  蓝色：就绪待命（未上车切台或已暂停）\r\n"
        L"  • 橙色：单端点/路由待确认或曲间弱音  |  红色：路由重叠外放、持续无声或互斥冲突\r\n"
        L"  • 点击窗口右上角 [×] 仅最小化到系统托盘；结束游玩请点击『退出程序』以安全刷空缓冲。",
        L"打开系统音量合成器",
        L"关闭",
        L"关于 — GTA IV Independence FM — Audio Bridge",
    };

    static const UiStrings kEn{
        L"Segoe UI",
        kUnifiedWindowTitle,

        L"&Options",
        L"&Settings...\tCtrl+,",
        L"Start &Bridge",
        L"Stop &Bridge",
        L"Open Volume &Mixer",
        L"Open &Log Folder",
        L"Minimize to &Tray",
        L"E&xit (Flush Buffer)",
        L"&Help",
        L"Quick Start &Guide...\tF1",
        L"&About...",

        L"Start Bridge",
        L"Stop Bridge",
        L"Volume Mixer",
        L"Log Folder",
        L"Exit App",
        L"Tip: Closing [×] minimizes to tray. Use 'Options / Help' above for Stream Anything source picker, Language & Guide.",

        "Detecting audio output topology...",
        "Waiting for audio endpoints and session state update.",
        L"Live Capture Level (1s)    Peak: %.1f dBFS  |  RMS: %.1f dBFS",
        L"Live Capture Level (1s)    No Signal (-∞ dBFS)",
        L"Source: %ls (PID %lu)  |  Format: %ls  |  Restarts: %llu  |  Uptime: %ls",
        L"Source: Waiting for %ls (polling every 350ms)  |  Uptime: %ls",
        L"Source: Stopped (Click 'Start Bridge' below to begin auto-capture)",
        L"In-Game Radio: Readers %u (%ls)  |  Lead %.2f s  |  Underruns %llu (%lluf)  |  Conflicts %llu",
        L"Game reading",
        L"Waiting for game",
        L"App Isolation: Excluding other active audio [",
        L"]",
        L"App Isolation: Per-process tree capture (%ls); other desktop/system sounds will not bleed in",
        L"Session Log: %ls (%.1f KB, keeping latest %u sessions)",
        L"Log Folder: ",

        L"Show Diagnostic Panel",
        L"Settings...",
        L"Quick Start Guide...",
        L"Open Volume Mixer",
        L"Open Log Folder",
        L"Exit (Flush Buffer Safely)",
        L"GTA IV Radio Bridge: Routing Overlap Warning",
        L"\nClick the tray icon to open the panel and adjust Windows Volume Mixer.",
        L"GTA IV Radio Bridge: Sustained Silence Warning",
        L"Please ensure 'Dolby Atmos' is set to 'Off' in Apple Music -> Settings -> Playback.",
        L"Check whether %ls is muted in-app, or using Exclusive Audio Mode (WASAPI Exclusive / ASIO).",

        L"Settings — GTA IV Independence FM — Audio Bridge",
        L"Interface & Diagnostic Language",
        L"Choose the display language for menus, live diagnostic cards, and tray alerts (applies instantly without audio interruption):",
        L"System Default (Auto)",
        L"简体中文 (Simplified Chinese)",
        L"English",
        L"Audio Capture Source — Stream Anything",
        L"Apple Music (Smart Preset: Auto-locate AMPLibraryAgent.exe audio engine)",
        L"Custom Audio App (Lists apps with active/idle audio sessions, or browse .exe):",
        L"Refresh",
        L"Browse .exe...",
        L"[Playing] ",
        L"[Idle] ",
        L"[Selected · No Session] ",
        L"(No other audio sessions found — play audio & click 'Refresh', or click 'Browse .exe...')",
        L"Tip: Saving hot-switches the source smoothly without resetting the in-game radio buffer. Use a compatible shared-mode audio app. Does not support WASAPI Exclusive / ASIO.",
        L"Select Audio Application (.exe) for GTA IV Independence FM",
        L"Forbidden Capture Target",
        L"Cannot select '%ls' as the radio capture source.\n\nCapturing GTA IV itself, this audio bridge, or Windows system hosts (explorer.exe / svchost.exe / audiodg.exe) would cause an audio feedback howling loop or system-wide noise bleed.\nPlease choose a standalone media player or web browser.",
        L"Switched to Apple Music Smart Preset",
        L"You selected an Apple Music component (%ls).\n\nBecause AppleMusic.exe is a silent UI shell on Windows, we automatically switched to the 'Apple Music Smart Preset' mode to track the actual audio engine (AMPLibraryAgent.exe).",
        L"No Custom Audio App Selected",
        L"Please select an application with an audio session from the dropdown list, or click 'Browse .exe...' to choose an executable.",
        L"Config File: ",
        L"Save",
        L"Cancel",
        L"Failed to Save Settings",
        L"Could not write settings file; changes apply to this session only:\n",

        L"Quick Start Guide — GTA IV Independence FM — Audio Bridge",
        L"GTA IV Independence FM — Audio Bridge Quick Start Guide",
        L"[1. Three Steps to Listen (Apple Music & Stream Anything)]\r\n"
        L"  1) Launch iv_apple_music_radio.exe (auto-starts the bridge and stays in the system tray).\r\n"
        L"  2) Apple Music for Windows is captured automatically out of the box. To stream from another compatible Windows audio app, open 'Options -> Settings (Ctrl+,)' and select 'Custom Audio App (Stream Anything)'.\r\n"
        L"  3) Run Complete Scan for Music in GTA IV, enter a vehicle and tune the radio to 'Independence FM'.\r\n\r\n"
        L"[2. Split-Endpoint Routing (Required to Prevent Double Audio)]\r\n"
        L"  • If your music app and GTA IV share the same speakers/headphones, you will hear desktop audio on top of the in-car radio.\r\n"
        L"  • Fix: Click 'Open Volume Mixer' below. Keep GTA IV on your main headphones/speakers, and assign your music app's Output Device to an active secondary output (for example HDMI or USB audio). Keep the app unmuted in Windows; choose a physically quiet output.\r\n\r\n"
        L"[3. Audio Format Notes (Dolby Atmos & Exclusive Mode)]\r\n"
        L"  • Apple Music: Go to 'Settings -> Playback -> Dolby Atmos' and set it to 'Off'.\r\n"
        L"  • Custom Players: Keep standard Shared WASAPI / DirectSound output enabled. Do NOT enable WASAPI Exclusive or ASIO (exclusive streams bypass the Windows audio engine and cannot be captured via process loopback).\r\n\r\n"
        L"[4. Buffer Latency & Skipping Tracks]\r\n"
        L"  • Tuning in initially takes ~5.3 s of handshake silence, and the in-game buffer delays source changes by several seconds. Carrier handoffs can also introduce a short silence.\r\n"
        L"  • Skip or pause tracks directly in your music app or via media keys. Avoid pressing 'B' or 'N' inside GTA IV (which forces a 5.3 s radio track reset).\r\n\r\n"
        L"[5. Status Colors & System Tray]\r\n"
        L"  • Green: Streaming cleanly in-game with isolated routing  |  Blue: Ready / Paused / Waiting for radio\r\n"
        L"  • Amber: Single-endpoint / routing unconfirmed or brief quiet passage  |  Red: Routing overlap / Silence / Conflict\r\n"
        L"  • Clicking [×] minimizes to the system tray. Use 'Exit App' when done playing to flush the shared buffer cleanly.",
        L"Open Volume Mixer",
        L"Close",
        L"About — GTA IV Independence FM — Audio Bridge",
    };

    return (stagee::ResolveEffectiveLanguage(lang) == stagee::UiLanguage::EnUs) ? kEn : kZh;
}

struct LauncherCliOptions {
    bool headless = false;
    bool startMinimized = false;
    bool autoStart = true;
    bool showHelp = false;

    std::wstring configFilePath;
    bool hasCliLanguageOverride = false;
    stagee::UiLanguage cliLanguageOverride = stagee::UiLanguage::Auto;
    bool hasCliSourceOverride = false;

    std::wstring instanceName = L"Local\\GTAIV_AppleMusic_Radio_Launcher_v1";
    std::wstring readyEventName;
    std::wstring stopEventName;
    std::wstring statusFilePath;
    double durationSec = 0.0;

    stagee::StageEEngineOptions engine{};
};

bool ParseUint32W(const wchar_t* s, uint32_t& out) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(s, &end, 10);
    if (!end || *end != L'\0') return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool ParseUint64W(const wchar_t* s, uint64_t& out) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    const unsigned long long v = std::wcstoull(s, &end, 10);
    if (!end || *end != L'\0') return false;
    out = static_cast<uint64_t>(v);
    return true;
}

bool ParseDoubleW(const wchar_t* s, double& out) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    const double v = std::wcstod(s, &end);
    if (!end || *end != L'\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ParseCommandLineOptions(LauncherCliOptions& opts, std::string& outError) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        outError = "CommandLineToArgvW failed";
        return false;
    }

    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            opts.showHelp = true;
        } else if (arg == L"--headless") {
            opts.headless = true;
        } else if (arg == L"--start-minimized" || arg == L"--tray-only") {
            opts.startMinimized = true;
        } else if (arg == L"--no-auto-start") {
            opts.autoStart = false;
        } else if (arg == L"--config-path" && i + 1 < argc) {
            opts.configFilePath = argv[++i];
        } else if (arg == L"--lang" && i + 1 < argc) {
            const std::string langUtf8 = staged::WideToUtf8(argv[++i]);
            stagee::UiLanguage parsed = stagee::UiLanguage::Auto;
            if (!stagee::ParseUiLanguageCode(langUtf8, parsed)) {
                outError = "Invalid --lang value (expected auto, zh, en, zh-CN, or en-US): " + langUtf8;
                ok = false;
                break;
            }
            opts.hasCliLanguageOverride = true;
            opts.cliLanguageOverride = parsed;
        } else if (arg == L"--instance-name" && i + 1 < argc) {
            opts.instanceName = argv[++i];
        } else if (arg == L"--mapping-name" && i + 1 < argc) {
            opts.engine.mappingName = argv[++i];
        } else if (arg == L"--log-dir" && i + 1 < argc) {
            opts.engine.logger.logDirectory = argv[++i];
        } else if (arg == L"--telemetry-jsonl" && i + 1 < argc) {
            opts.engine.logger.explicitFilePath = argv[++i];
        } else if (arg == L"--keep-logs" && i + 1 < argc) {
            if (!ParseUint32W(argv[++i], opts.engine.logger.maxRetainedSessions)) {
                outError = "Invalid --keep-logs value";
                ok = false;
                break;
            }
        } else if (arg == L"--max-log-mb" && i + 1 < argc) {
            uint64_t mb = 0ULL;
            if (!ParseUint64W(argv[++i], mb) || mb == 0ULL) {
                outError = "Invalid --max-log-mb value";
                ok = false;
                break;
            }
            opts.engine.logger.maxBytesPerPart = mb * 1024ULL * 1024ULL;
        } else if (arg == L"--max-log-bytes" && i + 1 < argc) {
            if (!ParseUint64W(argv[++i], opts.engine.logger.maxBytesPerPart)) {
                outError = "Invalid --max-log-bytes value";
                ok = false;
                break;
            }
        } else if (arg == L"--prefill-ms" && i + 1 < argc) {
            if (!ParseUint32W(argv[++i], opts.engine.prefillMs)) {
                outError = "Invalid --prefill-ms value";
                ok = false;
                break;
            }
        } else if (arg == L"--telemetry-interval-ms" && i + 1 < argc) {
            if (!ParseUint32W(argv[++i], opts.engine.telemetryIntervalMs)) {
                outError = "Invalid --telemetry-interval-ms value";
                ok = false;
                break;
            }
        } else if (arg == L"--topology-interval-ms" && i + 1 < argc) {
            if (!ParseUint32W(argv[++i], opts.engine.topologyPollIntervalMs)) {
                outError = "Invalid --topology-interval-ms value";
                ok = false;
                break;
            }
        } else if (arg == L"--target-pid" && i + 1 < argc) {
            if (!ParseUint32W(argv[++i], opts.engine.targetPid)) {
                outError = "Invalid --target-pid value";
                ok = false;
                break;
            }
        } else if ((arg == L"--target-exe" || arg == L"--source-exe") && i + 1 < argc) {
            const std::wstring targetArg = argv[++i];
            if (stagee::IsForbiddenCaptureTargetExe(targetArg)) {
                outError = "Forbidden capture target executable: " + staged::WideToUtf8(targetArg);
                ok = false;
                break;
            }
            opts.hasCliSourceOverride = true;
            opts.engine.targetExe = targetArg;
        } else if (arg == L"--duration-sec" && i + 1 < argc) {
            if (!ParseDoubleW(argv[++i], opts.durationSec) || opts.durationSec < 0.0) {
                outError = "Invalid --duration-sec value";
                ok = false;
                break;
            }
        } else if (arg == L"--ready-event" && i + 1 < argc) {
            opts.readyEventName = argv[++i];
        } else if (arg == L"--stop-event" && i + 1 < argc) {
            opts.stopEventName = argv[++i];
        } else if (arg == L"--status-file" && i + 1 < argc) {
            opts.statusFilePath = argv[++i];
        } else {
            outError = "Unknown or incomplete argument: " + staged::WideToUtf8(arg);
            ok = false;
            break;
        }
    }

    LocalFree(argv);
    return ok;
}

HANDLE OpenOrCreateNamedEventW(const std::wstring& name, bool manualReset) {
    if (name.empty()) {
        return nullptr;
    }
    HANDLE h = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!h) {
        h = CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, name.c_str());
    }
    return h;
}

void SignalNamedEventW(const std::wstring& name) {
    if (name.empty()) {
        return;
    }
    ScopedHandle h(OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
    if (!h.IsValid()) {
        h.Reset(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
    }
    if (h.IsValid()) {
        SetEvent(h.Get());
    }
}

void WriteLauncherStatusJson(const std::wstring& statusPath,
                             bool duplicateInstance,
                             bool startedOk,
                             stagee::UiLanguage configuredLang,
                             const stagee::StageELiveSnapshot& snap,
                             const std::string& errorUtf8,
                             int exitCode) {
    if (statusPath.empty()) {
        return;
    }
    const stagee::UiLanguage effLang = stagee::ResolveEffectiveLanguage(configuredLang);
    char buf[3072]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "{\n"
        "  \"duplicate_instance\": %s,\n"
        "  \"started_ok\": %s,\n"
        "  \"configured_language\": \"%s\",\n"
        "  \"effective_language\": \"%s\",\n"
        "  \"source_mode\": \"%s\",\n"
        "  \"target_display_name\": \"%s\",\n"
        "  \"clean_shutdown_verified\": %s,\n"
        "  \"clean_shutdown_ok\": %s,\n"
        "  \"session_generation\": %llu,\n"
        "  \"shutdown_queued_frames\": %llu,\n"
        "  \"shutdown_committed_frames\": %llu,\n"
        "  \"shutdown_holdback_frames\": %u,\n"
        "  \"retained_log_sessions\": %u,\n"
        "  \"active_log_part_index\": %u,\n"
        "  \"active_log_path\": \"%s\",\n"
        "  \"log_directory\": \"%s\",\n"
        "  \"headline_state\": \"%s\",\n"
        "  \"headline_title\": \"%s\",\n"
        "  \"routing_state\": \"%s\",\n"
        "  \"error\": \"%s\",\n"
        "  \"exit_code\": %d\n"
        "}\n",
        duplicateInstance ? "true" : "false",
        startedOk ? "true" : "false",
        staged::EscapeJsonString(stagee::UiLanguageToCode(configuredLang)).c_str(),
        staged::EscapeJsonString(stagee::UiLanguageToCode(effLang)).c_str(),
        staged::EscapeJsonString(stagee::AudioSourceModeToCode(snap.activeSource.mode)).c_str(),
        staged::EscapeJsonString(snap.targetDisplayNameUtf8).c_str(),
        snap.lastShutdownVerified ? "true" : "false",
        snap.lastShutdownClean ? "true" : "false",
        static_cast<unsigned long long>(snap.sessionGeneration),
        static_cast<unsigned long long>(snap.lastShutdownQueuedFrames),
        static_cast<unsigned long long>(snap.lastShutdownCommittedFrames),
        static_cast<unsigned>(snap.lastShutdownHoldbackFrames),
        static_cast<unsigned>(snap.retainedLogSessions),
        static_cast<unsigned>(snap.activeLogPartIndex),
        staged::EscapeJsonString(staged::WideToUtf8(snap.activeLogPath)).c_str(),
        staged::EscapeJsonString(staged::WideToUtf8(snap.logDirectory)).c_str(),
        staged::EscapeJsonString(stagee::PipelineHeadlineStateName(snap.report.headline)).c_str(),
        staged::EscapeJsonString(snap.report.headlineTitleUtf8).c_str(),
        staged::EscapeJsonString(stagee::RoutingIsolationStateName(snap.routing.state)).c_str(),
        staged::EscapeJsonString(errorUtf8).c_str(),
        exitCode);

    std::string writeErr;
    staged::WriteTextFileAtomic(statusPath, buf, writeErr);
}

// ============================================================================
// High-DPI, Modal Centering & Dynamic GDI Tray Badge Icon Helpers
// ============================================================================

void EnablePerMonitorDpiAwareness() noexcept {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) {
        return;
    }
    using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
    const auto fn = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
    if (fn) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == ((DPI_AWARENESS_CONTEXT)-4)
        fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    }
}

UINT QueryWindowDpi(HWND hwnd) noexcept {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32 && hwnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        const auto fn = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(hUser32, "GetDpiForWindow"));
        if (fn) {
            const UINT dpi = fn(hwnd);
            if (dpi >= 96u) {
                return dpi;
            }
        }
    }
    HDC hdc = GetDC(hwnd);
    UINT dpi = 96u;
    if (hdc) {
        const int logPx = GetDeviceCaps(hdc, LOGPIXELSX);
        if (logPx >= 96) {
            dpi = static_cast<UINT>(logPx);
        }
        ReleaseDC(hwnd, hdc);
    }
    return dpi;
}

int ScalePx(int px, UINT dpi) noexcept {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

HFONT CreateUiFontPx(int ptHeightPx, int weight, UINT dpi, const wchar_t* faceName) {
    return CreateFontW(
        -ScalePx(ptHeightPx, dpi),
        0, 0, 0,
        weight,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        faceName);
}

POINT ComputeCenteredOriginForOwner(HWND owner, int winW, int winH) noexcept {
    RECT refRc{};
    if (owner && IsWindowVisible(owner) && !IsIconic(owner) && GetWindowRect(owner, &refRc)) {
        // Center over visible owner window, clamped to monitor work area
        HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        int x = refRc.left + ((refRc.right - refRc.left) - winW) / 2;
        int y = refRc.top + ((refRc.bottom - refRc.top) - winH) / 2;
        if (GetMonitorInfoW(mon, &mi)) {
            x = std::clamp<int>(x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - winW));
            y = std::clamp<int>(y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - winH));
        }
        return POINT{ x, y };
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &refRc, 0);
    const int x = refRc.left + std::max<int>(0, ((refRc.right - refRc.left) - winW) / 2);
    const int y = refRc.top + std::max<int>(0, ((refRc.bottom - refRc.top) - winH) / 2);
    return POINT{ x, y };
}

COLORREF AccentColorRefForBadge(stagee::TrayBadgeColor c) noexcept {
    switch (c) {
        case stagee::TrayBadgeColor::Gray:  return RGB(120, 128, 140);
        case stagee::TrayBadgeColor::Blue:  return RGB(37, 99, 235);
        case stagee::TrayBadgeColor::Green: return RGB(22, 163, 74);
        case stagee::TrayBadgeColor::Amber: return RGB(217, 119, 6);
        case stagee::TrayBadgeColor::Red:   return RGB(220, 38, 38);
    }
    return RGB(120, 128, 140);
}

HICON CreateDynamicBadgeIcon(stagee::TrayBadgeColor badge, int sizePx) {
    const int sz = std::clamp(sizePx, 16, 64);

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = sz;
    bi.bV5Height = -sz; // Top-down ARGB
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP hColorBmp = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!hColorBmp || !bits) {
        if (hColorBmp) DeleteObject(hColorBmp);
        return nullptr;
    }

    const COLORREF cr = AccentColorRefForBadge(badge);
    const uint32_t r = GetRValue(cr);
    const uint32_t g = GetGValue(cr);
    const uint32_t b = GetBValue(cr);

    auto* px = static_cast<uint32_t*>(bits);
    const double center = (static_cast<double>(sz) - 1.0) * 0.5;
    const double outerR = static_cast<double>(sz) * 0.46;

    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            const double dx = static_cast<double>(x) - center;
            const double dy = static_cast<double>(y) - center;
            const double dist = std::sqrt(dx * dx + dy * dy);

            if (dist > outerR + 0.8) {
                px[y * sz + x] = 0x00000000u;
                continue;
            }

            double cov = 1.0;
            if (dist > outerR - 0.6) {
                cov = std::clamp((outerR + 0.8 - dist) / 1.4, 0.0, 1.0);
            }

            // Three vertical white audio equalizer bars in the center
            const double nx = dx / outerR;
            const double ny = std::abs(dy) / outerR;
            const bool inLeftBar  = (nx >= -0.46 && nx <= -0.24 && ny <= 0.34);
            const bool inMidBar   = (nx >= -0.11 && nx <=  0.11 && ny <= 0.54);
            const bool inRightBar = (nx >=  0.24 && nx <=  0.46 && ny <= 0.40);
            const bool isWhiteFg  = inLeftBar || inMidBar || inRightBar;

            const uint32_t alpha = static_cast<uint32_t>(std::llround(cov * 255.0));
            const uint32_t pr = isWhiteFg ? ((255u * alpha) / 255u) : ((r * alpha) / 255u);
            const uint32_t pg = isWhiteFg ? ((255u * alpha) / 255u) : ((g * alpha) / 255u);
            const uint32_t pb = isWhiteFg ? ((255u * alpha) / 255u) : ((b * alpha) / 255u);
            px[y * sz + x] = (alpha << 24) | (pr << 16) | (pg << 8) | pb;
        }
    }

    HBITMAP hMaskBmp = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMaskBmp;
    ii.hbmColor = hColorBmp;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hMaskBmp);
    DeleteObject(hColorBmp);
    return hIcon;
}

std::wstring FormatUptimeCompact(double sec) {
    if (sec < 0.0 || !std::isfinite(sec)) {
        sec = 0.0;
    }
    const uint64_t total = static_cast<uint64_t>(sec);
    const uint64_t hh = total / 3600ULL;
    const uint64_t mm = (total % 3600ULL) / 60ULL;
    const uint64_t ss = total % 60ULL;
    wchar_t buf[64]{};
    if (hh > 0ULL) {
        std::swprintf(buf, std::size(buf), L"%lluh %02llum %02llus",
                      static_cast<unsigned long long>(hh),
                      static_cast<unsigned long long>(mm),
                      static_cast<unsigned long long>(ss));
    } else {
        std::swprintf(buf, std::size(buf), L"%02llum %02llus",
                      static_cast<unsigned long long>(mm),
                      static_cast<unsigned long long>(ss));
    }
    return buf;
}

// ============================================================================
// Modal Dialog Window Procedures (Settings & Quick Start Guide)
// ============================================================================

struct SourcePickerItem {
    std::wstring exeName;
    std::wstring exeFullPath;
    bool isValidTarget = false;
};

struct SettingsDialogState {
    HWND owner = nullptr;
    HINSTANCE hInst = nullptr;
    UINT dpi = 96u;
    stagee::UiLanguage effectiveUiLang = stagee::UiLanguage::ZhCn;
    const UiStrings* ui = nullptr;
    std::wstring configPath;
    stagee::UiLanguage selectedLanguage = stagee::UiLanguage::Auto;
    stagee::AudioSourceConfig selectedSource{};
    std::wstring rememberedCustomExeName;
    std::wstring rememberedCustomExePath;
    std::vector<SourcePickerItem> pickerItems;
    bool accepted = false;
    bool closed = false;

    HWND comboLang = nullptr;
    HWND radioAppleMusic = nullptr;
    HWND radioCustomApp = nullptr;
    HWND comboSourceApps = nullptr;
    HWND btnRefreshApps = nullptr;
    HWND btnBrowseExe = nullptr;
    HFONT fontBold = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HBRUSH bgBrush = nullptr;
};

void CloseModalDialogSafely(HWND hDlg, HWND owner, bool& closedFlag) noexcept {
    if (closedFlag) {
        return;
    }
    closedFlag = true;
    // Enable owner BEFORE DestroyWindow so Windows returns foreground activation to owner cleanly
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        SetActiveWindow(owner);
    }
    DestroyWindow(hDlg);
}

void SyncSettingsSourceControls(SettingsDialogState* st) noexcept {
    if (!st) {
        return;
    }
    const bool customChecked =
        st->radioCustomApp && (SendMessageW(st->radioCustomApp, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (st->comboSourceApps) {
        EnableWindow(st->comboSourceApps, customChecked ? TRUE : FALSE);
    }
    if (st->btnRefreshApps) {
        EnableWindow(st->btnRefreshApps, customChecked ? TRUE : FALSE);
    }
    if (st->btnBrowseExe) {
        // Keep Browse always enabled so clicking it can auto-switch to Custom Audio App seamlessly
        EnableWindow(st->btnBrowseExe, TRUE);
    }
}

void RefreshSettingsAudioSessions(SettingsDialogState* st) {
    if (!st || !st->comboSourceApps || !st->ui) {
        return;
    }
    const UiStrings& s = *st->ui;

    staged::AudioTopologyReport topo;
    std::string topoErr;
    staged::DiscoverAudioTopology(topo, topoErr);

    const std::vector<stagee::SelectableAudioApp> sessionApps = stagee::EnumerateSelectableAudioApps(topo);

    SendMessageW(st->comboSourceApps, CB_RESETCONTENT, 0, 0);
    st->pickerItems.clear();

    int matchedIndex = -1;

    // Populate ONLY applications that currently hold WASAPI audio sessions (Playing first, then Idle),
    // excluding Apple Music family (covered by Smart Preset Radio 1) and forbidden targets.
    for (const auto& app : sessionApps) {
        if (app.isAppleMusicFamily) {
            continue;
        }
        std::wstring label = app.hasActiveSession ? s.settingsComboTagPlaying : s.settingsComboTagIdle;
        label += app.exeName;
        if (!app.sessionDisplayName.empty() &&
            !staged::EqualsIgnoreCase(app.sessionDisplayName, app.exeName)) {
            label += L" — " + app.sessionDisplayName;
        }
        label += L" (PID " + std::to_wstring(app.rootPid);
        if (!app.endpointName.empty()) {
            label += L" · " + app.endpointName;
        }
        label += L")";

        const int idx = static_cast<int>(st->pickerItems.size());
        st->pickerItems.push_back(SourcePickerItem{ app.exeName, app.exeFullPath, true });
        SendMessageW(st->comboSourceApps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));

        if (!st->rememberedCustomExeName.empty()) {
            const bool exactPathMatch =
                !st->rememberedCustomExePath.empty() &&
                !app.exeFullPath.empty() &&
                staged::EqualsIgnoreCase(st->rememberedCustomExePath, app.exeFullPath);
            const bool leafMatch = staged::EqualsIgnoreCase(st->rememberedCustomExeName, app.exeName);
            if (exactPathMatch) {
                matchedIndex = idx;
            } else if (leafMatch && matchedIndex < 0) {
                matchedIndex = idx;
            }
        }
    }

    // Preserve pre-selected / browsed custom .exe at the top when it has not opened an audio session yet
    if (matchedIndex < 0 && !st->rememberedCustomExeName.empty() &&
        !stagee::IsForbiddenCaptureTargetExe(st->rememberedCustomExeName) &&
        !stagee::IsAppleMusicShellOrAgentExe(st->rememberedCustomExeName)) {
        std::wstring preLabel = std::wstring(s.settingsComboTagPreselected) + st->rememberedCustomExeName;
        if (!st->rememberedCustomExePath.empty()) {
            preLabel += L" (" + st->rememberedCustomExePath + L")";
        }
        st->pickerItems.insert(
            st->pickerItems.begin(),
            SourcePickerItem{ st->rememberedCustomExeName, st->rememberedCustomExePath, true });
        SendMessageW(st->comboSourceApps, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(preLabel.c_str()));
        matchedIndex = 0;
    }

    if (st->pickerItems.empty()) {
        st->pickerItems.push_back(SourcePickerItem{ L"", L"", false });
        SendMessageW(
            st->comboSourceApps,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(s.settingsComboEmptyHint));
        matchedIndex = 0;
    } else if (matchedIndex < 0) {
        matchedIndex = 0;
        if (st->pickerItems[0].isValidTarget && st->rememberedCustomExeName.empty()) {
            st->rememberedCustomExeName = st->pickerItems[0].exeName;
            st->rememberedCustomExePath = st->pickerItems[0].exeFullPath;
        }
    }

    SendMessageW(st->comboSourceApps, CB_SETCURSEL, static_cast<WPARAM>(matchedIndex), 0);
}

void HandleBrowseCustomExe(HWND hwnd, SettingsDialogState* st) {
    if (!st || !st->ui) {
        return;
    }
    const UiStrings& s = *st->ui;
    const bool en = (st->effectiveUiLang == stagee::UiLanguage::EnUs);

    std::vector<wchar_t> fileBuf(4096u, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = en
        ? L"Executable Files (*.exe)\0*.exe\0"
        : L"\x53EF\x6267\x884C\x7A0B\x5E8F (*.exe)\0*.exe\0";
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
    ofn.lpstrTitle = s.settingsBrowseDialogTitle;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    const std::wstring chosenPath(fileBuf.data());
    const std::wstring leaf = staged::ExtractLeafFilename(chosenPath);
    if (leaf.empty()) {
        return;
    }

    if (stagee::IsForbiddenCaptureTargetExe(chosenPath)) {
        wchar_t body[768]{};
        std::swprintf(body, std::size(body), s.settingsForbiddenBodyFmt, leaf.c_str());
        MessageBoxW(hwnd, body, s.settingsForbiddenTitle, MB_ICONWARNING | MB_OK);
        return;
    }

    if (stagee::IsAppleMusicShellOrAgentExe(chosenPath)) {
        if (st->radioAppleMusic) SendMessageW(st->radioAppleMusic, BM_SETCHECK, BST_CHECKED, 0);
        if (st->radioCustomApp)  SendMessageW(st->radioCustomApp, BM_SETCHECK, BST_UNCHECKED, 0);
        SyncSettingsSourceControls(st);

        wchar_t body[768]{};
        std::swprintf(body, std::size(body), s.settingsAppleRedirectBodyFmt, leaf.c_str());
        MessageBoxW(hwnd, body, s.settingsAppleRedirectTitle, MB_ICONINFORMATION | MB_OK);
        return;
    }

    st->rememberedCustomExeName = leaf;
    st->rememberedCustomExePath = chosenPath;
    if (st->radioAppleMusic) SendMessageW(st->radioAppleMusic, BM_SETCHECK, BST_UNCHECKED, 0);
    if (st->radioCustomApp)  SendMessageW(st->radioCustomApp, BM_SETCHECK, BST_CHECKED, 0);
    RefreshSettingsAudioSessions(st);
    SyncSettingsSourceControls(st);
}

LRESULT CALLBACK SettingsDialogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialogState* st = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        st = static_cast<SettingsDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    } else {
        st = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!st) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CREATE: {
            const UINT dpi = st->dpi;
            const UiStrings& s = *st->ui;

            st->fontBold  = CreateUiFontPx(13, FW_SEMIBOLD, dpi, s.fontFace);
            st->fontBody  = CreateUiFontPx(12, FW_NORMAL, dpi, s.fontFace);
            st->fontSmall = CreateUiFontPx(11, FW_NORMAL, dpi, s.fontFace);
            st->bgBrush   = CreateSolidBrush(RGB(248, 250, 252));

            const int pad = ScalePx(16, dpi);
            const int innerW = ScalePx(540, dpi) - 2 * pad;

            // Group 1: Interface Language
            HWND grpLang = CreateWindowExW(
                0, L"BUTTON", s.settingsGroupLang,
                WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                pad, ScalePx(14, dpi), innerW, ScalePx(104, dpi),
                hwnd, nullptr, st->hInst, nullptr);
            SendMessageW(grpLang, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBold), TRUE);

            HWND lblLang = CreateWindowExW(
                0, L"STATIC", s.settingsLangDesc,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                pad + ScalePx(14, dpi), ScalePx(36, dpi), innerW - ScalePx(28, dpi), ScalePx(34, dpi),
                hwnd, nullptr, st->hInst, nullptr);
            SendMessageW(lblLang, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            st->comboLang = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                pad + ScalePx(14, dpi), ScalePx(74, dpi), innerW - ScalePx(28, dpi), ScalePx(160, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgLangCombo)), st->hInst, nullptr);
            SendMessageW(st->comboLang, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);
            SendMessageW(st->comboLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.settingsLangAuto));
            SendMessageW(st->comboLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.settingsLangZh));
            SendMessageW(st->comboLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.settingsLangEn));
            int selIdx = 0;
            if (st->selectedLanguage == stagee::UiLanguage::ZhCn) selIdx = 1;
            else if (st->selectedLanguage == stagee::UiLanguage::EnUs) selIdx = 2;
            SendMessageW(st->comboLang, CB_SETCURSEL, static_cast<WPARAM>(selIdx), 0);

            // Group 2: Audio Capture Source ("Stream Anything")
            HWND grpSrc = CreateWindowExW(
                0, L"BUTTON", s.settingsGroupSource,
                WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                pad, ScalePx(128, dpi), innerW, ScalePx(200, dpi),
                hwnd, nullptr, st->hInst, nullptr);
            SendMessageW(grpSrc, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBold), TRUE);

            st->radioAppleMusic = CreateWindowExW(
                0, L"BUTTON", s.settingsRadioAppleMusic,
                WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON,
                pad + ScalePx(14, dpi), ScalePx(152, dpi), innerW - ScalePx(28, dpi), ScalePx(22, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgRadioAppleMusic)), st->hInst, nullptr);
            SendMessageW(st->radioAppleMusic, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            st->radioCustomApp = CreateWindowExW(
                0, L"BUTTON", s.settingsRadioCustomApp,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
                pad + ScalePx(14, dpi), ScalePx(178, dpi), innerW - ScalePx(28, dpi), ScalePx(22, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgRadioCustomApp)), st->hInst, nullptr);
            SendMessageW(st->radioCustomApp, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            const int rowInnerW = innerW - ScalePx(28, dpi);
            const int ctrlGap = ScalePx(8, dpi);
            const int refreshW = ScalePx(84, dpi);
            const int browseW = ScalePx(98, dpi);
            const int comboW = std::max<int>(ScalePx(160, dpi), rowInnerW - refreshW - browseW - 2 * ctrlGap);
            const int rowX = pad + ScalePx(14, dpi);

            st->comboSourceApps = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                rowX, ScalePx(207, dpi), comboW, ScalePx(220, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgSourceAppCombo)), st->hInst, nullptr);
            SendMessageW(st->comboSourceApps, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);
            SendMessageW(st->comboSourceApps, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(ScalePx(492, dpi)), 0);

            st->btnRefreshApps = CreateWindowExW(
                0, L"BUTTON", s.settingsBtnRefreshApps,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                rowX + comboW + ctrlGap, ScalePx(205, dpi), refreshW, ScalePx(28, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnRefreshApps)), st->hInst, nullptr);
            SendMessageW(st->btnRefreshApps, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            st->btnBrowseExe = CreateWindowExW(
                0, L"BUTTON", s.settingsBtnBrowseExe,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                rowX + comboW + ctrlGap + refreshW + ctrlGap, ScalePx(205, dpi), browseW, ScalePx(28, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgBtnBrowseExe)), st->hInst, nullptr);
            SendMessageW(st->btnBrowseExe, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            HWND lblSrcNote = CreateWindowExW(
                0, L"STATIC", s.settingsSourceNote,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                pad + ScalePx(14, dpi), ScalePx(242, dpi), innerW - ScalePx(28, dpi), ScalePx(76, dpi),
                hwnd, nullptr, st->hInst, nullptr);
            SendMessageW(lblSrcNote, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontSmall), TRUE);

            const bool initCustom = (st->selectedSource.mode == stagee::AudioSourceMode::CustomProcess);
            SendMessageW(st->radioAppleMusic, BM_SETCHECK, initCustom ? BST_UNCHECKED : BST_CHECKED, 0);
            SendMessageW(st->radioCustomApp, BM_SETCHECK, initCustom ? BST_CHECKED : BST_UNCHECKED, 0);
            RefreshSettingsAudioSessions(st);
            SyncSettingsSourceControls(st);

            // Config file path caption
            const std::wstring cfgLine = std::wstring(s.settingsConfigPathPrefix) + st->configPath;
            HWND lblCfg = CreateWindowExW(
                0, L"STATIC", cfgLine.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                pad, ScalePx(338, dpi), innerW, ScalePx(20, dpi),
                hwnd, nullptr, st->hInst, nullptr);
            SendMessageW(lblCfg, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontSmall), TRUE);

            // Save / Cancel buttons
            const int btnW = ScalePx(108, dpi);
            const int btnH = ScalePx(32, dpi);
            const int btnY = ScalePx(364, dpi);
            const int cancelX = pad + innerW - btnW;
            const int saveX = cancelX - ScalePx(10, dpi) - btnW;

            HWND btnSave = CreateWindowExW(
                0, L"BUTTON", s.settingsBtnSave,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                saveX, btnY, btnW, btnH,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), st->hInst, nullptr);
            SendMessageW(btnSave, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            HWND btnCancel = CreateWindowExW(
                0, L"BUTTON", s.settingsBtnCancel,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                cancelX, btnY, btnW, btnH,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), st->hInst, nullptr);
            SendMessageW(btnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            return 0;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, st->bgBrush);
            return 1;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(31, 41, 55));
            return reinterpret_cast<LRESULT>(st->bgBrush);
        }

        case WM_COMMAND: {
            const int cmd = LOWORD(wParam);
            const int notifyCode = HIWORD(wParam);

            if (cmd == kIdDlgRadioAppleMusic || cmd == kIdDlgRadioCustomApp) {
                SyncSettingsSourceControls(st);
                if (cmd == kIdDlgRadioCustomApp && st->comboSourceApps) {
                    const LRESULT sel = SendMessageW(st->comboSourceApps, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && static_cast<size_t>(sel) < st->pickerItems.size() &&
                        st->pickerItems[static_cast<size_t>(sel)].isValidTarget) {
                        st->rememberedCustomExeName = st->pickerItems[static_cast<size_t>(sel)].exeName;
                        st->rememberedCustomExePath = st->pickerItems[static_cast<size_t>(sel)].exeFullPath;
                    }
                }
                return 0;
            }

            if (cmd == kIdDlgSourceAppCombo && notifyCode == CBN_SELCHANGE) {
                const LRESULT sel = st->comboSourceApps
                    ? SendMessageW(st->comboSourceApps, CB_GETCURSEL, 0, 0)
                    : CB_ERR;
                if (sel >= 0 && static_cast<size_t>(sel) < st->pickerItems.size() &&
                    st->pickerItems[static_cast<size_t>(sel)].isValidTarget) {
                    st->rememberedCustomExeName = st->pickerItems[static_cast<size_t>(sel)].exeName;
                    st->rememberedCustomExePath = st->pickerItems[static_cast<size_t>(sel)].exeFullPath;
                }
                return 0;
            }

            if (cmd == kIdDlgBtnRefreshApps) {
                RefreshSettingsAudioSessions(st);
                return 0;
            }

            if (cmd == kIdDlgBtnBrowseExe) {
                HandleBrowseCustomExe(hwnd, st);
                return 0;
            }

            if (cmd == IDOK) {
                const LRESULT langSel = st->comboLang ? SendMessageW(st->comboLang, CB_GETCURSEL, 0, 0) : 0;
                if (langSel == 1) st->selectedLanguage = stagee::UiLanguage::ZhCn;
                else if (langSel == 2) st->selectedLanguage = stagee::UiLanguage::EnUs;
                else st->selectedLanguage = stagee::UiLanguage::Auto;

                const bool customChecked =
                    st->radioCustomApp && (SendMessageW(st->radioCustomApp, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (!customChecked) {
                    st->selectedSource = stagee::AudioSourceConfig{};
                    st->selectedSource.mode = stagee::AudioSourceMode::AppleMusicPreset;
                } else {
                    const LRESULT appSel = st->comboSourceApps
                        ? SendMessageW(st->comboSourceApps, CB_GETCURSEL, 0, 0)
                        : CB_ERR;
                    if (appSel < 0 || static_cast<size_t>(appSel) >= st->pickerItems.size() ||
                        !st->pickerItems[static_cast<size_t>(appSel)].isValidTarget) {
                        const UiStrings& s = *st->ui;
                        MessageBoxW(hwnd, s.settingsNeedTargetBody, s.settingsNeedTargetTitle, MB_ICONWARNING | MB_OK);
                        return 0;
                    }
                    const SourcePickerItem& picked = st->pickerItems[static_cast<size_t>(appSel)];
                    st->selectedSource.mode = stagee::AudioSourceMode::CustomProcess;
                    st->selectedSource.customExeName = picked.exeName;
                    st->selectedSource.customExePath = picked.exeFullPath;
                }

                st->accepted = true;
                CloseModalDialogSafely(hwnd, st->owner, st->closed);
                return 0;
            }
            if (cmd == IDCANCEL) {
                st->accepted = false;
                CloseModalDialogSafely(hwnd, st->owner, st->closed);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            st->accepted = false;
            CloseModalDialogSafely(hwnd, st->owner, st->closed);
            return 0;

        case WM_NCDESTROY:
            if (st->fontBold)  { DeleteObject(st->fontBold);  st->fontBold = nullptr; }
            if (st->fontBody)  { DeleteObject(st->fontBody);  st->fontBody = nullptr; }
            if (st->fontSmall) { DeleteObject(st->fontSmall); st->fontSmall = nullptr; }
            if (st->bgBrush)   { DeleteObject(st->bgBrush);   st->bgBrush = nullptr; }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct GuideDialogState {
    HWND owner = nullptr;
    HINSTANCE hInst = nullptr;
    UINT dpi = 96u;
    const UiStrings* ui = nullptr;
    bool closed = false;

    HWND editBox = nullptr;
    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HBRUSH bgBrush = nullptr;
    HBRUSH whiteBrush = nullptr;
};

LRESULT CALLBACK GuideDialogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GuideDialogState* st = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        st = static_cast<GuideDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    } else {
        st = reinterpret_cast<GuideDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!st) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CREATE: {
            const UINT dpi = st->dpi;
            const UiStrings& s = *st->ui;

            st->fontTitle  = CreateUiFontPx(15, FW_BOLD, dpi, s.fontFace);
            st->fontBody   = CreateUiFontPx(12, FW_NORMAL, dpi, s.fontFace);
            st->bgBrush    = CreateSolidBrush(RGB(248, 250, 252));
            st->whiteBrush = CreateSolidBrush(RGB(255, 255, 255));

            const int pad = ScalePx(16, dpi);
            const int innerW = ScalePx(580, dpi) - 2 * pad;

            HWND lblHdr = CreateWindowExW(
                0, L"STATIC", s.guideHeaderTitle,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                pad, ScalePx(14, dpi), innerW, ScalePx(26, dpi),
                hwnd, nullptr, st->hInst, nullptr);
            SendMessageW(lblHdr, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontTitle), TRUE);

            st->editBox = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                s.guideBodyText,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                pad, ScalePx(46, dpi), innerW, ScalePx(332, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgGuideEdit)), st->hInst, nullptr);
            SendMessageW(st->editBox, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);
            const int marginPx = ScalePx(8, dpi);
            SendMessageW(st->editBox, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(marginPx, marginPx));
            SendMessageW(st->editBox, EM_SETSEL, static_cast<WPARAM>(-1), 0);

            const int btnH = ScalePx(32, dpi);
            const int btnY = ScalePx(390, dpi);
            const int mixerW = ScalePx(176, dpi);
            const int closeW = ScalePx(108, dpi);

            HWND btnMixer = CreateWindowExW(
                0, L"BUTTON", s.guideBtnOpenMixer,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                pad, btnY, mixerW, btnH,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDlgOpenMixer)), st->hInst, nullptr);
            SendMessageW(btnMixer, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            HWND btnClose = CreateWindowExW(
                0, L"BUTTON", s.guideBtnClose,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                pad + innerW - closeW, btnY, closeW, btnH,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), st->hInst, nullptr);
            SendMessageW(btnClose, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontBody), TRUE);

            return 0;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, st->bgBrush);
            return 1;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND ctrl = reinterpret_cast<HWND>(lParam);
            if (ctrl == st->editBox) {
                SetBkColor(hdc, RGB(255, 255, 255));
                SetTextColor(hdc, RGB(31, 41, 55));
                return reinterpret_cast<LRESULT>(st->whiteBrush);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(17, 24, 39));
            return reinterpret_cast<LRESULT>(st->bgBrush);
        }

        case WM_COMMAND: {
            const int cmd = LOWORD(wParam);
            if (cmd == kIdDlgOpenMixer) {
                ShellExecuteW(hwnd, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (cmd == IDOK || cmd == IDCANCEL) {
                CloseModalDialogSafely(hwnd, st->owner, st->closed);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            CloseModalDialogSafely(hwnd, st->owner, st->closed);
            return 0;

        case WM_NCDESTROY:
            if (st->fontTitle)  { DeleteObject(st->fontTitle);  st->fontTitle = nullptr; }
            if (st->fontBody)   { DeleteObject(st->fontBody);   st->fontBody = nullptr; }
            if (st->bgBrush)    { DeleteObject(st->bgBrush);    st->bgBrush = nullptr; }
            if (st->whiteBrush) { DeleteObject(st->whiteBrush); st->whiteBrush = nullptr; }
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterDialogClassesOnce(HINSTANCE hInst, HICON appIcon) {
    static bool s_registered = false;
    if (s_registered) {
        return;
    }

    WNDCLASSEXW wcSettings{};
    wcSettings.cbSize = sizeof(wcSettings);
    wcSettings.style = CS_HREDRAW | CS_VREDRAW;
    wcSettings.lpfnWndProc = &SettingsDialogWndProc;
    wcSettings.hInstance = hInst;
    wcSettings.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcSettings.hIcon = appIcon;
    wcSettings.hIconSm = appIcon;
    wcSettings.lpszClassName = kSettingsDlgClassName;
    RegisterClassExW(&wcSettings);

    WNDCLASSEXW wcGuide{};
    wcGuide.cbSize = sizeof(wcGuide);
    wcGuide.style = CS_HREDRAW | CS_VREDRAW;
    wcGuide.lpfnWndProc = &GuideDialogWndProc;
    wcGuide.hInstance = hInst;
    wcGuide.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcGuide.hIcon = appIcon;
    wcGuide.hIconSm = appIcon;
    wcGuide.lpszClassName = kGuideDlgClassName;
    RegisterClassExW(&wcGuide);

    s_registered = true;
}

// ============================================================================
// GUI Application Controller
// ============================================================================

class LauncherWindowApp {
public:
    LauncherWindowApp(HINSTANCE hInst,
                      const LauncherCliOptions& opts,
                      const std::wstring& settingsFilePath,
                      const stagee::LauncherSettings& initialSettings,
                      HANDLE activateEvent,
                      HANDLE externalStopEvent)
        : hInst_(hInst),
          opts_(opts),
          settingsFilePath_(settingsFilePath),
          settings_(initialSettings),
          activeLanguagePref_(opts.engine.language),
          activateEvent_(activateEvent),
          externalStopEvent_(externalStopEvent) {
        QueryPerformanceFrequency(&qpcFreq_);
        QueryPerformanceCounter(&appStartQpc_);
        engine_.SetLanguage(activeLanguagePref_);
    }

    ~LauncherWindowApp() {
        RemoveTrayIcon();
        if (hMenuBar_) {
            DestroyMenu(hMenuBar_);
            hMenuBar_ = nullptr;
            hOptionsMenu_ = nullptr;
        }
        DestroyCachedFonts();
        for (HICON& ic : badgeIcons_) {
            if (ic) {
                DestroyIcon(ic);
                ic = nullptr;
            }
        }
    }

    int RunGui() {
        const int smCx = GetSystemMetrics(SM_CXSMICON);
        const int iconPx = (smCx > 0) ? smCx : 24;
        for (int i = 0; i < 5; ++i) {
            badgeIcons_[i] = CreateDynamicBadgeIcon(static_cast<stagee::TrayBadgeColor>(i), iconPx);
        }

        const HICON blueIcon = badgeIcons_[static_cast<size_t>(stagee::TrayBadgeColor::Blue)];
        RegisterDialogClassesOnce(hInst_, blueIcon);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &LauncherWindowApp::StaticWndProc;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = blueIcon;
        wc.hIconSm = blueIcon;
        wc.hbrBackground = nullptr; // Custom double-buffered paint
        wc.lpszClassName = L"GTAIV_AppleMusic_Radio_StageE1_Wnd";
        if (!RegisterClassExW(&wc)) {
            return 1;
        }

        const UiStrings& ui = CurrentStrings();
        const DWORD wndStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rc{ 0, 0, kBaseClientWidth, kBaseClientHeight };
        AdjustWindowRectEx(&rc, wndStyle, TRUE /* bMenu */, 0);

        hwnd_ = CreateWindowExW(
            0,
            wc.lpszClassName,
            ui.windowTitle,
            wndStyle,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rc.right - rc.left,
            rc.bottom - rc.top,
            nullptr,
            nullptr,
            hInst_,
            this);
        if (!hwnd_) {
            return 1;
        }

        RebuildMenuBar();
        dpi_ = QueryWindowDpi(hwnd_);
        RebuildFontsAndLayoutForDpi();
        ApplyStaticButtonLabels();

        if (opts_.autoStart) {
            std::string startErr;
            engineStartedOk_ = engine_.Start(opts_.engine, startErr);
            lastStartError_ = startErr;
        }

        latestSnap_ = engine_.GetSnapshot();
        InstallTrayIcon();
        SyncControlsFromSnapshot();

        if (!opts_.startMinimized) {
            ShowWindow(hwnd_, SW_SHOW);
            UpdateWindow(hwnd_);
        }

        SignalNamedEventW(opts_.readyEventName);
        SetTimer(hwnd_, kTimerUiRefresh, 66u, nullptr); // ~15 Hz smooth refresh

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!modalDialogOpen_ && msg.message == WM_KEYDOWN) {
                const HWND rootWnd = GetAncestor(msg.hwnd, GA_ROOT);
                if (rootWnd == hwnd_) {
                    if (msg.wParam == VK_F1) {
                        ShowUsageGuideDialog();
                        continue;
                    }
                    if (msg.wParam == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) < 0)) {
                        ShowSettingsDialog();
                        continue;
                    }
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        engine_.Stop();
        latestSnap_ = engine_.GetSnapshot();
        WriteLauncherStatusJson(
            opts_.statusFilePath,
            false,
            engineStartedOk_,
            activeLanguagePref_,
            latestSnap_,
            lastStartError_,
            exitCode_);
        return exitCode_;
    }

private:
    stagee::UiLanguage EffectiveLang() const noexcept {
        return stagee::ResolveEffectiveLanguage(activeLanguagePref_);
    }

    const UiStrings& CurrentStrings() const noexcept {
        return GetUiStrings(activeLanguagePref_);
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        LauncherWindowApp* self = nullptr;
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<LauncherWindowApp*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<LauncherWindowApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) {
            return self->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE:
                CreateChildButtons(hwnd);
                return 0;

            case WM_ERASEBKGND:
                return 1; // Suppress background flicker

            case WM_DPICHANGED: {
                dpi_ = HIWORD(wParam);
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                if (suggested) {
                    SetWindowPos(
                        hwnd,
                        nullptr,
                        suggested->left,
                        suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                RebuildFontsAndLayoutForDpi();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_TIMER:
                if (wParam == kTimerUiRefresh) {
                    OnPeriodicTick();
                }
                return 0;

            case WM_COMMAND:
                OnCommand(LOWORD(wParam));
                return 0;

            case kWmTrayCallback:
                OnTrayNotification(lParam);
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                PaintDiagnosticPanel(hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_CLOSE:
                if (!realExitRequested_) {
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                }
                DestroyWindow(hwnd);
                return 0;

            case WM_QUERYENDSESSION:
                return TRUE;

            case WM_ENDSESSION:
                if (wParam != FALSE) {
                    PerformCleanExit();
                }
                return 0;

            case WM_DESTROY:
                KillTimer(hwnd, kTimerUiRefresh);
                RemoveTrayIcon();
                if (hMenuBar_) {
                    SetMenu(hwnd, nullptr);
                    DestroyMenu(hMenuBar_);
                    hMenuBar_ = nullptr;
                    hOptionsMenu_ = nullptr;
                }
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void CreateChildButtons(HWND hwnd) {
        const UiStrings& s = CurrentStrings();
        btnToggle_ = CreateWindowExW(
            0, L"BUTTON", s.btnStopBridge,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnToggleBridge)), hInst_, nullptr);
        btnMixer_ = CreateWindowExW(
            0, L"BUTTON", s.btnOpenMixer,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 120, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnOpenMixer)), hInst_, nullptr);
        btnLogs_ = CreateWindowExW(
            0, L"BUTTON", s.btnOpenLogs,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 110, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnOpenLogs)), hInst_, nullptr);
        btnExit_ = CreateWindowExW(
            0, L"BUTTON", s.btnExitApp,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 100, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnExitApp)), hInst_, nullptr);
    }

    void RebuildMenuBar() {
        if (!hwnd_) {
            return;
        }
        const UiStrings& s = CurrentStrings();

        HMENU newBar = CreateMenu();
        HMENU optMenu = CreatePopupMenu();
        HMENU helpMenu = CreatePopupMenu();
        if (!newBar || !optMenu || !helpMenu) {
            if (optMenu) DestroyMenu(optMenu);
            if (helpMenu) DestroyMenu(helpMenu);
            if (newBar) DestroyMenu(newBar);
            return;
        }

        AppendMenuW(optMenu, MF_STRING, kIdMenuSettings, s.menuSettings);
        AppendMenuW(optMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(optMenu, MF_STRING, kIdMenuToggleBridge,
                    latestSnap_.running ? s.menuStopBridge : s.menuStartBridge);
        AppendMenuW(optMenu, MF_STRING, kIdMenuOpenMixer, s.menuOpenMixer);
        AppendMenuW(optMenu, MF_STRING, kIdMenuOpenLogs, s.menuOpenLogs);
        AppendMenuW(optMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(optMenu, MF_STRING, kIdMenuMinimizeTray, s.menuMinimizeTray);
        AppendMenuW(optMenu, MF_STRING, kIdMenuExit, s.menuExit);

        AppendMenuW(helpMenu, MF_STRING, kIdMenuUsageGuide, s.menuUsageGuide);
        AppendMenuW(helpMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(helpMenu, MF_STRING, kIdMenuAbout, s.menuAbout);

        AppendMenuW(newBar, MF_POPUP, reinterpret_cast<UINT_PTR>(optMenu), s.menuOptions);
        AppendMenuW(newBar, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), s.menuHelp);

        HMENU oldBar = hMenuBar_;
        hMenuBar_ = newBar;
        hOptionsMenu_ = optMenu;
        desiredMenuToggleLabel_.clear();

        SetMenu(hwnd_, hMenuBar_);
        DrawMenuBar(hwnd_);
        if (oldBar) {
            DestroyMenu(oldBar);
        }
    }

    void ApplyStaticButtonLabels() {
        const UiStrings& s = CurrentStrings();
        if (hwnd_)      SetWindowTextW(hwnd_, s.windowTitle);
        if (btnMixer_)  SetWindowTextW(btnMixer_, s.btnOpenMixer);
        if (btnLogs_)   SetWindowTextW(btnLogs_, s.btnOpenLogs);
        if (btnExit_)   SetWindowTextW(btnExit_, s.btnExitApp);
        desiredToggleLabel_.clear();
    }

    void DestroyCachedFonts() noexcept {
        if (fontTitle_)  { DeleteObject(fontTitle_);  fontTitle_ = nullptr; }
        if (fontBold_)   { DeleteObject(fontBold_);   fontBold_ = nullptr; }
        if (fontBody_)   { DeleteObject(fontBody_);   fontBody_ = nullptr; }
        if (fontSmall_)  { DeleteObject(fontSmall_);  fontSmall_ = nullptr; }
    }

    void RebuildFontsAndLayoutForDpi() {
        DestroyCachedFonts();

        const wchar_t* face = CurrentStrings().fontFace;
        fontTitle_ = CreateUiFontPx(16, FW_BOLD, dpi_, face);
        fontBold_  = CreateUiFontPx(13, FW_SEMIBOLD, dpi_, face);
        fontBody_  = CreateUiFontPx(12, FW_NORMAL, dpi_, face);
        fontSmall_ = CreateUiFontPx(11, FW_NORMAL, dpi_, face);

        const DWORD wndStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        RECT rc{ 0, 0, ScalePx(kBaseClientWidth, dpi_), ScalePx(kBaseClientHeight, dpi_) };
        AdjustWindowRectEx(&rc, wndStyle, (hMenuBar_ != nullptr) ? TRUE : FALSE, 0);
        SetWindowPos(
            hwnd_,
            nullptr,
            0, 0,
            rc.right - rc.left,
            rc.bottom - rc.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        const int pad = ScalePx(16, dpi_);
        const int gap = ScalePx(10, dpi_);
        const int btnY = ScalePx(402, dpi_);
        const int btnH = ScalePx(32, dpi_);
        const int totalW = ScalePx(kBaseClientWidth, dpi_) - 2 * pad;
        const int btnW = (totalW - 3 * gap) / 4;

        HWND btns[4] = { btnToggle_, btnMixer_, btnLogs_, btnExit_ };
        for (int i = 0; i < 4; ++i) {
            if (btns[i]) {
                SendMessageW(btns[i], WM_SETFONT, reinterpret_cast<WPARAM>(fontBody_), TRUE);
                SetWindowPos(
                    btns[i],
                    nullptr,
                    pad + i * (btnW + gap),
                    btnY,
                    btnW,
                    btnH,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    void ApplyLanguageAndRefreshUi(stagee::UiLanguage newPref) {
        activeLanguagePref_ = newPref;
        opts_.engine.language = newPref;
        engine_.SetLanguage(newPref);

        RebuildMenuBar();
        RebuildFontsAndLayoutForDpi();
        ApplyStaticButtonLabels();

        latestSnap_ = engine_.GetSnapshot();
        SyncControlsFromSnapshot();
        UpdateTrayStateAndBalloon();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RestoreAndActivatePanel() {
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
    }

    void OnPeriodicTick() {
        if (activateEvent_ && WaitForSingleObject(activateEvent_, 0u) == WAIT_OBJECT_0) {
            RestoreAndActivatePanel();
        }
        if (externalStopEvent_ && WaitForSingleObject(externalStopEvent_, 0u) == WAIT_OBJECT_0) {
            PerformCleanExit();
            return;
        }
        if (opts_.durationSec > 0.0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - appStartQpc_.QuadPart) /
                static_cast<double>(qpcFreq_.QuadPart);
            if (elapsed >= opts_.durationSec) {
                PerformCleanExit();
                return;
            }
        }

        latestSnap_ = engine_.GetSnapshot();
        SyncControlsFromSnapshot();
        UpdateTrayStateAndBalloon();

        if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void SyncControlsFromSnapshot() {
        const UiStrings& s = CurrentStrings();
        const wchar_t* desiredBtn = latestSnap_.running ? s.btnStopBridge : s.btnStartBridge;
        if (btnToggle_ && desiredToggleLabel_ != desiredBtn) {
            desiredToggleLabel_ = desiredBtn;
            SetWindowTextW(btnToggle_, desiredBtn);
        }
        const wchar_t* desiredMenu = latestSnap_.running ? s.menuStopBridge : s.menuStartBridge;
        if (hOptionsMenu_ && desiredMenuToggleLabel_ != desiredMenu) {
            desiredMenuToggleLabel_ = desiredMenu;
            ModifyMenuW(hOptionsMenu_, kIdMenuToggleBridge, MF_BYCOMMAND | MF_STRING, kIdMenuToggleBridge, desiredMenu);
        }
    }

    void OpenWindowsVolumeMixer() {
        ShellExecuteW(hwnd_, L"open", L"ms-settings:apps-volume", nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenLogFolder() {
        std::wstring dir = latestSnap_.logDirectory;
        if (dir.empty()) {
            dir = opts_.engine.logger.logDirectory.empty()
                ? stagee::ResolveDefaultLogDirectory()
                : opts_.engine.logger.logDirectory;
        }
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(hwnd_, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void ToggleBridgeRunState() {
        if (engine_.IsRunning()) {
            engine_.Stop();
        } else {
            opts_.engine.language = activeLanguagePref_;
            std::string err;
            engineStartedOk_ = engine_.Start(opts_.engine, err);
            lastStartError_ = err;
        }
        latestSnap_ = engine_.GetSnapshot();
        SyncControlsFromSnapshot();
        UpdateTrayStateAndBalloon();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RunOwnedModalWindow(HWND hDlg, bool& closedFlag) {
        if (!hDlg) {
            return;
        }
        modalDialogOpen_ = true;
        EnableWindow(hwnd_, FALSE);
        ShowWindow(hDlg, SW_SHOWNORMAL);
        UpdateWindow(hDlg);

        MSG msg{};
        while (!closedFlag) {
            const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
            if (gm <= 0) {
                if (gm == 0) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                }
                CloseModalDialogSafely(hDlg, hwnd_, closedFlag);
                break;
            }
            if (!IsDialogMessageW(hDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        modalDialogOpen_ = false;
    }

    void ShowSettingsDialog() {
        if (modalDialogOpen_ || !hwnd_) {
            return;
        }
        RestoreAndActivatePanel();

        SettingsDialogState st{};
        st.owner = hwnd_;
        st.hInst = hInst_;
        st.dpi = QueryWindowDpi(hwnd_);
        st.effectiveUiLang = EffectiveLang();
        st.ui = &CurrentStrings();
        st.configPath = settingsFilePath_;
        st.selectedLanguage = activeLanguagePref_;
        st.selectedSource = engine_.IsRunning() ? engine_.GetAudioSource() : settings_.source;
        if (st.selectedSource.mode == stagee::AudioSourceMode::CustomProcess) {
            st.rememberedCustomExeName = st.selectedSource.customExeName;
            st.rememberedCustomExePath = st.selectedSource.customExePath;
        }

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        RECT rc{ 0, 0, ScalePx(540, st.dpi), ScalePx(408, st.dpi) };
        AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
        const int winW = rc.right - rc.left;
        const int winH = rc.bottom - rc.top;
        const POINT pt = ComputeCenteredOriginForOwner(hwnd_, winW, winH);

        HWND hDlg = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kSettingsDlgClassName,
            st.ui->settingsTitle,
            style,
            pt.x, pt.y, winW, winH,
            hwnd_,
            nullptr,
            hInst_,
            &st);
        if (!hDlg) {
            return;
        }

        RunOwnedModalWindow(hDlg, st.closed);

        if (st.accepted) {
            settings_.language = st.selectedLanguage;
            settings_.source = st.selectedSource;
            opts_.engine.source = st.selectedSource;
            opts_.engine.targetExe.clear();
            opts_.engine.targetPid = 0u;

            engine_.SetAudioSource(st.selectedSource);
            ApplyLanguageAndRefreshUi(st.selectedLanguage);

            std::string saveErr;
            if (!stagee::SaveLauncherSettings(settingsFilePath_, settings_, saveErr)) {
                const UiStrings& curS = CurrentStrings();
                const std::wstring warnMsg = std::wstring(curS.settingsSaveFailedPrefix) + staged::Utf8ToWide(saveErr);
                MessageBoxW(hwnd_, warnMsg.c_str(), curS.settingsSaveFailedTitle, MB_ICONWARNING | MB_OK);
            }
        }
    }

    void ShowUsageGuideDialog() {
        if (modalDialogOpen_ || !hwnd_) {
            return;
        }
        RestoreAndActivatePanel();

        GuideDialogState st{};
        st.owner = hwnd_;
        st.hInst = hInst_;
        st.dpi = QueryWindowDpi(hwnd_);
        st.ui = &CurrentStrings();

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        RECT rc{ 0, 0, ScalePx(580, st.dpi), ScalePx(436, st.dpi) };
        AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
        const int winW = rc.right - rc.left;
        const int winH = rc.bottom - rc.top;
        const POINT pt = ComputeCenteredOriginForOwner(hwnd_, winW, winH);

        HWND hDlg = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            kGuideDlgClassName,
            st.ui->guideWindowTitle,
            style,
            pt.x, pt.y, winW, winH,
            hwnd_,
            nullptr,
            hInst_,
            &st);
        if (!hDlg) {
            return;
        }

        RunOwnedModalWindow(hDlg, st.closed);
    }

    void ShowAboutDialog() {
        if (modalDialogOpen_ || !hwnd_) {
            return;
        }
        RestoreAndActivatePanel();
        const bool en = (EffectiveLang() == stagee::UiLanguage::EnUs);
        const UiStrings& s = CurrentStrings();
        const std::wstring logDir = !latestSnap_.logDirectory.empty()
            ? latestSnap_.logDirectory
            : (opts_.engine.logger.logDirectory.empty() ? stagee::ResolveDefaultLogDirectory() : opts_.engine.logger.logDirectory);
        const std::wstring curSrcW = staged::Utf8ToWide(
            latestSnap_.targetDisplayNameUtf8.empty()
                ? stagee::FormatAudioSourceDisplayNameUtf8(settings_.source)
                : latestSnap_.targetDisplayNameUtf8);

        std::wstring text;
        if (en) {
            text =
                L"GTA IV Independence FM — Audio Bridge (Stage E1 + Stream Anything)\n\n"
                L"Active Audio Source:\n  " + curSrcW + L"\n\n"
                L"Architecture:\n"
                L"  • x64 WASAPI Per-Process Tree Loopback (48 kHz / 2ch / 16-bit PCM)\n"
                L"  • Lock-Free Cross-Bitness Shared Memory Ring Buffer\n"
                L"  • x86 WMF IWMSyncReader In-Game Radio Consumer (IVRadioProbe.asi)\n\n"
                L"Settings File:\n  " + settingsFilePath_ + L"\n\n"
                L"Session Log Directory:\n  " + logDir;
        } else {
            text =
                L"GTA IV Independence FM — Audio Bridge (Stage E1 + Stream Anything)\n\n"
                L"当前捕获音源：\n  " + curSrcW + L"\n\n"
                L"管线架构：\n"
                L"  • x64 WASAPI 按进程树隔离环回捕获 (48 kHz / 2ch / 16-bit PCM)\n"
                L"  • 跨位数无锁共享内存环形缓冲 (Local\\GTAIV_StageC_IpcRing_v1)\n"
                L"  • x86 WMF IWMSyncReader 游戏车载电台消费端 (IVRadioProbe.asi)\n\n"
                L"配置文件路径：\n  " + settingsFilePath_ + L"\n\n"
                L"会话日志目录：\n  " + logDir;
        }
        MessageBoxW(hwnd_, text.c_str(), s.aboutTitle, MB_ICONINFORMATION | MB_OK);
    }

    void PerformCleanExit() {
        if (realExitRequested_) {
            return;
        }
        realExitRequested_ = true;
        engine_.Stop();
        latestSnap_ = engine_.GetSnapshot();
        RemoveTrayIcon();
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    void OnCommand(int cmdId) {
        switch (cmdId) {
            case kIdBtnToggleBridge:
            case kIdTrayToggle:
            case kIdMenuToggleBridge:
                ToggleBridgeRunState();
                break;

            case kIdBtnOpenMixer:
            case kIdTrayOpenMixer:
            case kIdMenuOpenMixer:
                OpenWindowsVolumeMixer();
                break;

            case kIdBtnOpenLogs:
            case kIdTrayOpenLogs:
            case kIdMenuOpenLogs:
                OpenLogFolder();
                break;

            case kIdMenuSettings:
            case kIdTraySettings:
                ShowSettingsDialog();
                break;

            case kIdMenuUsageGuide:
            case kIdTrayUsageGuide:
                ShowUsageGuideDialog();
                break;

            case kIdMenuAbout:
                ShowAboutDialog();
                break;

            case kIdMenuMinimizeTray:
                if (hwnd_) {
                    ShowWindow(hwnd_, SW_HIDE);
                }
                break;

            case kIdTrayShowPanel:
                RestoreAndActivatePanel();
                break;

            case kIdBtnExitApp:
            case kIdTrayExit:
            case kIdMenuExit:
                PerformCleanExit();
                break;

            default:
                break;
        }
    }

    void InstallTrayIcon() {
        if (trayInstalled_ || !hwnd_) {
            return;
        }
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1u;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kWmTrayCallback;

        const auto colorIdx = static_cast<size_t>(latestSnap_.report.badgeColor);
        nid.hIcon = badgeIcons_[colorIdx < 5u ? colorIdx : 0u];
        const std::wstring tipW = staged::Utf8ToWide(latestSnap_.report.trayTooltipUtf8);
        wcsncpy_s(nid.szTip, tipW.c_str(), _TRUNCATE);

        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            trayInstalled_ = true;
            lastTrayBadge_ = latestSnap_.report.badgeColor;
            lastTrayTipW_ = tipW;
        }
    }

    void UpdateTrayStateAndBalloon() {
        if (!trayInstalled_ || !hwnd_) {
            return;
        }

        const stagee::TrayBadgeColor curBadge = latestSnap_.report.badgeColor;
        const std::wstring curTipW = staged::Utf8ToWide(latestSnap_.report.trayTooltipUtf8);

        const bool newRoutingWarn = latestSnap_.report.hasRoutingConflict && !prevHadRoutingConflict_;
        const bool newSilenceWarn =
            (latestSnap_.report.headline == stagee::PipelineHeadlineState::SustainedZeroAudioWarning) &&
            (prevHeadline_ != stagee::PipelineHeadlineState::SustainedZeroAudioWarning);

        prevHadRoutingConflict_ = latestSnap_.report.hasRoutingConflict;
        prevHeadline_ = latestSnap_.report.headline;

        const bool needBalloon = !IsWindowVisible(hwnd_) && (newRoutingWarn || newSilenceWarn);
        if (!needBalloon && curBadge == lastTrayBadge_ && curTipW == lastTrayTipW_) {
            return;
        }

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1u;
        nid.uFlags = NIF_ICON | NIF_TIP;

        const auto colorIdx = static_cast<size_t>(curBadge);
        nid.hIcon = badgeIcons_[colorIdx < 5u ? colorIdx : 0u];
        wcsncpy_s(nid.szTip, curTipW.c_str(), _TRUNCATE);

        if (needBalloon) {
            const UiStrings& s = CurrentStrings();
            nid.uFlags |= NIF_INFO;
            nid.dwInfoFlags = NIIF_WARNING;
            if (newRoutingWarn) {
                wcsncpy_s(nid.szInfoTitle, s.balloonRoutingTitle, _TRUNCATE);
                const std::wstring body = staged::Utf8ToWide(latestSnap_.routing.bannerTitleUtf8) +
                    s.balloonRoutingHintSuffix;
                wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
            } else {
                wcsncpy_s(nid.szInfoTitle, s.balloonSilenceTitle, _TRUNCATE);
                if (latestSnap_.activeSource.mode == stagee::AudioSourceMode::AppleMusicPreset) {
                    wcsncpy_s(nid.szInfo, s.balloonSilenceBodyApple, _TRUNCATE);
                } else {
                    const std::wstring targetW = staged::Utf8ToWide(
                        latestSnap_.targetDisplayNameUtf8.empty()
                            ? std::string("Custom App")
                            : latestSnap_.targetDisplayNameUtf8);
                    wchar_t body[256]{};
                    std::swprintf(body, std::size(body), s.balloonSilenceBodyGenericFmt, targetW.c_str());
                    wcsncpy_s(nid.szInfo, body, _TRUNCATE);
                }
            }
        }

        Shell_NotifyIconW(NIM_MODIFY, &nid);
        lastTrayBadge_ = curBadge;
        lastTrayTipW_ = curTipW;

        if (nid.hIcon) {
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(nid.hIcon));
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(nid.hIcon));
        }
    }

    void RemoveTrayIcon() noexcept {
        if (!trayInstalled_ || !hwnd_) {
            return;
        }
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1u;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayInstalled_ = false;
    }

    void ShowTrayContextMenu() {
        if (modalDialogOpen_) {
            RestoreAndActivatePanel();
            return;
        }
        const UiStrings& s = CurrentStrings();
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;

        AppendMenuW(hMenu, MF_STRING, kIdTrayShowPanel, s.trayShowPanel);
        SetMenuDefaultItem(hMenu, kIdTrayShowPanel, FALSE);
        AppendMenuW(hMenu, MF_STRING, kIdTrayToggle, latestSnap_.running ? s.btnStopBridge : s.btnStartBridge);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, kIdTraySettings, s.traySettings);
        AppendMenuW(hMenu, MF_STRING, kIdTrayUsageGuide, s.trayUsageGuide);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, kIdTrayOpenMixer, s.trayOpenMixer);
        AppendMenuW(hMenu, MF_STRING, kIdTrayOpenLogs, s.trayOpenLogs);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, kIdTrayExit, s.trayExit);

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(hMenu);
    }

    void OnTrayNotification(LPARAM lParam) {
        const UINT ev = LOWORD(lParam);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK || ev == NIN_BALLOONUSERCLICK) {
            RestoreAndActivatePanel();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            ShowTrayContextMenu();
        }
    }

    static void FillSolidRectGdi(HDC dc, const RECT& r, COLORREF color) {
        HBRUSH br = CreateSolidBrush(color);
        FillRect(dc, &r, br);
        DeleteObject(br);
    }

    static void DrawCardBox(HDC dc, const RECT& r, COLORREF bg, COLORREF border, COLORREF leftStripe, int stripeW) {
        FillSolidRectGdi(dc, r, bg);
        HBRUSH borderBr = CreateSolidBrush(border);
        FrameRect(dc, &r, borderBr);
        DeleteObject(borderBr);

        if (stripeW > 0) {
            RECT sr = r;
            sr.right = std::min(r.right, r.left + stripeW);
            FillSolidRectGdi(dc, sr, leftStripe);
        }
    }

    void PaintDiagnosticPanel(HDC targetDc) {
        RECT clientRc{};
        GetClientRect(hwnd_, &clientRc);
        const int cw = clientRc.right - clientRc.left;
        const int ch = clientRc.bottom - clientRc.top;
        if (cw <= 0 || ch <= 0) {
            return;
        }

        const UiStrings& s = CurrentStrings();

        HDC memDc = CreateCompatibleDC(targetDc);
        HBITMAP memBmp = CreateCompatibleBitmap(targetDc, cw, ch);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);
        SetBkMode(memDc, TRANSPARENT);

        // Background slate
        FillSolidRectGdi(memDc, clientRc, RGB(244, 246, 250));

        const int pad = ScalePx(16, dpi_);
        const int stripeW = ScalePx(5, dpi_);

        // 1. Primary Pipeline Status Card
        const RECT card1{ pad, ScalePx(14, dpi_), cw - pad, ScalePx(104, dpi_) };
        const COLORREF accent1 = AccentColorRefForBadge(latestSnap_.report.badgeColor);
        const bool headlineIsWarn =
            (latestSnap_.report.headline == stagee::PipelineHeadlineState::SustainedZeroAudioWarning) ||
            (latestSnap_.report.headline == stagee::PipelineHeadlineState::TargetMutedInMixerWarning) ||
            (latestSnap_.report.headline == stagee::PipelineHeadlineState::ProducerMutexBusy);
        const COLORREF bg1 = headlineIsWarn ? RGB(254, 242, 242) : RGB(255, 255, 255);
        const COLORREF bd1 = headlineIsWarn ? RGB(248, 180, 180) : RGB(220, 226, 236);
        DrawCardBox(memDc, card1, bg1, bd1, accent1, stripeW);

        {
            SelectObject(memDc, fontTitle_);
            SetTextColor(memDc, headlineIsWarn ? RGB(153, 27, 27) : RGB(17, 24, 39));
            RECT titleRc{ card1.left + ScalePx(14, dpi_), card1.top + ScalePx(10, dpi_),
                          card1.right - ScalePx(12, dpi_), card1.top + ScalePx(36, dpi_) };
            const std::wstring tW = staged::Utf8ToWide(latestSnap_.report.headlineTitleUtf8);
            DrawTextW(memDc, tW.c_str(), -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(memDc, fontBody_);
            SetTextColor(memDc, headlineIsWarn ? RGB(127, 29, 29) : RGB(75, 85, 99));
            RECT detailRc{ card1.left + ScalePx(14, dpi_), card1.top + ScalePx(38, dpi_),
                           card1.right - ScalePx(12, dpi_), card1.bottom - ScalePx(8, dpi_) };
            const std::wstring dW = staged::Utf8ToWide(latestSnap_.report.headlineDetailUtf8);
            DrawTextW(memDc, dW.c_str(), -1, &detailRc, DT_LEFT | DT_WORDBREAK);
        }

        // 2. Split-Endpoint Routing Isolation Card
        const RECT card2{ pad, ScalePx(114, dpi_), cw - pad, ScalePx(216, dpi_) };
        const bool routeConflict = latestSnap_.report.hasRoutingConflict;
        const bool routeOk = (latestSnap_.routing.state == stagee::RoutingIsolationState::IsolatedOk);
        const COLORREF routeStripe = routeConflict
            ? RGB(220, 38, 38)
            : (routeOk ? RGB(22, 163, 74) : RGB(217, 119, 6));
        const COLORREF bg2 = routeConflict
            ? RGB(255, 241, 242)
            : (routeOk ? RGB(240, 253, 244) : RGB(255, 251, 235));
        const COLORREF bd2 = routeConflict
            ? RGB(251, 165, 165)
            : (routeOk ? RGB(167, 243, 208) : RGB(252, 211, 77));
        DrawCardBox(memDc, card2, bg2, bd2, routeStripe, stripeW);

        {
            SelectObject(memDc, fontBold_);
            SetTextColor(memDc, routeConflict ? RGB(153, 27, 27) : (routeOk ? RGB(21, 128, 61) : RGB(146, 64, 14)));
            RECT rTitleRc{ card2.left + ScalePx(14, dpi_), card2.top + ScalePx(9, dpi_),
                           card2.right - ScalePx(12, dpi_), card2.top + ScalePx(31, dpi_) };
            const std::string routeTitle = latestSnap_.routing.bannerTitleUtf8.empty()
                ? std::string(s.routingDetectingTitleUtf8)
                : latestSnap_.routing.bannerTitleUtf8;
            const std::wstring rtW = staged::Utf8ToWide(routeTitle);
            DrawTextW(memDc, rtW.c_str(), -1, &rTitleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(memDc, fontBody_);
            SetTextColor(memDc, routeConflict ? RGB(127, 29, 29) : RGB(55, 65, 81));
            RECT rBodyRc{ card2.left + ScalePx(14, dpi_), card2.top + ScalePx(33, dpi_),
                          card2.right - ScalePx(12, dpi_), card2.bottom - ScalePx(8, dpi_) };
            const std::string routeDetail = latestSnap_.routing.bannerDetailUtf8.empty()
                ? std::string(s.routingDetectingDetailUtf8)
                : latestSnap_.routing.bannerDetailUtf8;
            const std::wstring rdW = staged::Utf8ToWide(routeDetail);
            DrawTextW(memDc, rdW.c_str(), -1, &rBodyRc, DT_LEFT | DT_WORDBREAK);
        }

        // 3. Live PCM Meter & Telemetry Details Card
        const RECT card3{ pad, ScalePx(226, dpi_), cw - pad, ScalePx(390, dpi_) };
        DrawCardBox(memDc, card3, RGB(255, 255, 255), RGB(220, 226, 236), RGB(100, 116, 139), 0);

        {
            const int innerL = card3.left + ScalePx(14, dpi_);
            const int innerR = card3.right - ScalePx(14, dpi_);

            // Level bar label & dBFS readout
            SelectObject(memDc, fontBold_);
            SetTextColor(memDc, RGB(31, 41, 55));
            RECT lvlLabelRc{ innerL, card3.top + ScalePx(10, dpi_), innerR, card3.top + ScalePx(28, dpi_) };
            wchar_t lvlText[192]{};
            if (latestSnap_.running && latestSnap_.level.audibleFrames1s > 0ULL) {
                std::swprintf(
                    lvlText,
                    std::size(lvlText),
                    s.levelActiveFmt,
                    latestSnap_.level.peakDbfs1s,
                    latestSnap_.level.rmsDbfs1s);
            } else {
                std::swprintf(lvlText, std::size(lvlText), L"%ls", s.levelSilentText);
            }
            DrawTextW(memDc, lvlText, -1, &lvlLabelRc, DT_LEFT | DT_SINGLELINE);

            // Level bar track (-72 dBFS .. 0 dBFS)
            const RECT barBg{ innerL, card3.top + ScalePx(32, dpi_), innerR, card3.top + ScalePx(48, dpi_) };
            FillSolidRectGdi(memDc, barBg, RGB(229, 231, 235));

            if (latestSnap_.running && latestSnap_.level.peakAbs1s > 0) {
                auto dbToRatio = [](double db) -> double {
                    return std::clamp((db + 72.0) / 72.0, 0.0, 1.0);
                };
                const int barW = barBg.right - barBg.left;
                const int rmsW = static_cast<int>(std::llround(dbToRatio(latestSnap_.level.rmsDbfs1s) * barW));
                const int peakX = barBg.left + static_cast<int>(std::llround(dbToRatio(latestSnap_.level.peakDbfs1s) * barW));

                if (rmsW > 0) {
                    RECT rmsRc = barBg;
                    rmsRc.right = std::min(barBg.right, barBg.left + rmsW);
                    const COLORREF meterColor = (latestSnap_.level.peakDbfs1s > -1.0)
                        ? RGB(234, 88, 12)
                        : RGB(16, 185, 129);
                    FillSolidRectGdi(memDc, rmsRc, meterColor);
                }
                if (peakX > barBg.left) {
                    const LONG pkLeft = std::max<LONG>(barBg.left, static_cast<LONG>(peakX - ScalePx(2, dpi_)));
                    const LONG pkRight = std::min<LONG>(barBg.right, static_cast<LONG>(peakX + 1));
                    RECT pkRc{ pkLeft, barBg.top, pkRight, barBg.bottom };
                    FillSolidRectGdi(memDc, pkRc, RGB(15, 118, 110));
                }
            }

            // Four telemetry rows
            SelectObject(memDc, fontBody_);
            SetTextColor(memDc, RGB(55, 65, 81));
            int rowY = card3.top + ScalePx(58, dpi_);
            const int rowStep = ScalePx(24, dpi_);

            auto drawRow = [&](const std::wstring& line) {
                RECT rr{ innerL, rowY, innerR, rowY + rowStep };
                DrawTextW(memDc, line.c_str(), -1, &rr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                rowY += rowStep;
            };

            const std::wstring targetDispW = staged::Utf8ToWide(
                latestSnap_.targetDisplayNameUtf8.empty()
                    ? stagee::FormatAudioSourceDisplayNameUtf8(settings_.source)
                    : latestSnap_.targetDisplayNameUtf8);

            // Row 1: Target & Uptime
            if (latestSnap_.running && latestSnap_.captureStreamActive && latestSnap_.captureTargetPid != 0u) {
                wchar_t r1[280]{};
                std::swprintf(
                    r1,
                    std::size(r1),
                    s.row1ActiveFmt,
                    staged::Utf8ToWide(latestSnap_.captureTargetExeUtf8).c_str(),
                    static_cast<unsigned long>(latestSnap_.captureTargetPid),
                    staged::Utf8ToWide(latestSnap_.captureFormatUtf8).c_str(),
                    static_cast<unsigned long long>(latestSnap_.timeline.streamRestartCount),
                    FormatUptimeCompact(latestSnap_.uptimeSec).c_str());
                drawRow(r1);
            } else if (latestSnap_.running) {
                wchar_t r1[280]{};
                std::swprintf(
                    r1,
                    std::size(r1),
                    s.row1PollingFmt,
                    targetDispW.c_str(),
                    FormatUptimeCompact(latestSnap_.uptimeSec).c_str());
                drawRow(r1);
            } else {
                drawRow(s.row1StoppedText);
            }

            // Row 2: GTA IV IPC Consumer Status
            {
                const double wmSec = static_cast<double>(latestSnap_.ipc.watermarkFrames) / 48000.0;
                wchar_t r2[280]{};
                std::swprintf(
                    r2,
                    std::size(r2),
                    s.row2Fmt,
                    static_cast<unsigned>(latestSnap_.ipc.consumerActiveReaders),
                    latestSnap_.ipc.consumerActiveReaders > 0u ? s.row2StreamingTag : s.row2IdleTag,
                    wmSec,
                    static_cast<unsigned long long>(latestSnap_.ipc.underrunEvents),
                    static_cast<unsigned long long>(latestSnap_.ipc.underrunFrames),
                    static_cast<unsigned long long>(latestSnap_.ipc.overwriteConflictEvents));
                drawRow(r2);
            }

            // Row 3: Multi-session process isolation status
            if (!latestSnap_.routing.otherActiveMediaAppsUtf8.empty()) {
                std::string joined;
                for (size_t i = 0u; i < latestSnap_.routing.otherActiveMediaAppsUtf8.size(); ++i) {
                    if (i > 0u) joined += ", ";
                    joined += latestSnap_.routing.otherActiveMediaAppsUtf8[i];
                }
                drawRow(std::wstring(s.row3ExcludedPrefix) + staged::Utf8ToWide(joined) + s.row3ExcludedSuffix);
            } else {
                wchar_t r3[280]{};
                std::swprintf(r3, std::size(r3), s.row3DefaultFmt, targetDispW.c_str());
                drawRow(r3);
            }

            // Row 4: Active rotating session log
            if (!latestSnap_.activeLogPath.empty()) {
                const std::wstring leaf = staged::ExtractLeafFilename(latestSnap_.activeLogPath);
                const double kb = static_cast<double>(latestSnap_.activeLogPartBytes) / 1024.0;
                wchar_t r4[320]{};
                std::swprintf(
                    r4,
                    std::size(r4),
                    s.row4ActiveFmt,
                    leaf.c_str(),
                    kb,
                    static_cast<unsigned>(opts_.engine.logger.maxRetainedSessions));
                drawRow(r4);
            } else {
                const std::wstring defDir = opts_.engine.logger.logDirectory.empty()
                    ? stagee::ResolveDefaultLogDirectory()
                    : opts_.engine.logger.logDirectory;
                drawRow(std::wstring(s.row4IdlePrefix) + defDir);
            }
        }

        // 4. Footer Tray & Menu Hint
        {
            SelectObject(memDc, fontSmall_);
            SetTextColor(memDc, RGB(107, 114, 128));
            RECT footRc{ pad, ScalePx(440, dpi_), cw - pad, ch - ScalePx(4, dpi_) };
            DrawTextW(
                memDc,
                s.footerHint,
                -1,
                &footRc,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        BitBlt(targetDc, 0, 0, cw, ch, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);
    }

    HINSTANCE hInst_ = nullptr;
    LauncherCliOptions opts_{};
    std::wstring settingsFilePath_;
    stagee::LauncherSettings settings_{};
    stagee::UiLanguage activeLanguagePref_ = stagee::UiLanguage::Auto;
    HANDLE activateEvent_ = nullptr;
    HANDLE externalStopEvent_ = nullptr;

    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER appStartQpc_{};

    HWND hwnd_ = nullptr;
    HMENU hMenuBar_ = nullptr;
    HMENU hOptionsMenu_ = nullptr;
    HWND btnToggle_ = nullptr;
    HWND btnMixer_ = nullptr;
    HWND btnLogs_ = nullptr;
    HWND btnExit_ = nullptr;
    std::wstring desiredToggleLabel_;
    std::wstring desiredMenuToggleLabel_;

    UINT dpi_ = 96u;
    HFONT fontTitle_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HICON badgeIcons_[5]{};

    bool modalDialogOpen_ = false;
    bool trayInstalled_ = false;
    stagee::TrayBadgeColor lastTrayBadge_ = stagee::TrayBadgeColor::Gray;
    std::wstring lastTrayTipW_;
    bool prevHadRoutingConflict_ = false;
    stagee::PipelineHeadlineState prevHeadline_ = stagee::PipelineHeadlineState::BridgeStopped;

    bool engineStartedOk_ = false;
    std::string lastStartError_;
    bool realExitRequested_ = false;
    int exitCode_ = 0;

    stagee::StageEBridgeEngine engine_;
    stagee::StageELiveSnapshot latestSnap_{};
};

int RunHeadlessMode(const LauncherCliOptions& opts, HANDLE externalStopEvent) {
    stagee::StageEBridgeEngine engine;
    std::string startErr;
    const bool started = engine.Start(opts.engine, startErr);
    if (!started) {
        const stagee::StageELiveSnapshot failSnap = engine.GetSnapshot();
        WriteLauncherStatusJson(opts.statusFilePath, false, false, opts.engine.language, failSnap, startErr, 1);
        return 1;
    }

    SignalNamedEventW(opts.readyEventName);

    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER t0{};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&t0);

    while (true) {
        if (externalStopEvent && WaitForSingleObject(externalStopEvent, 20u) == WAIT_OBJECT_0) {
            break;
        }
        if (!externalStopEvent) {
            Sleep(20u);
        }
        if (opts.durationSec > 0.0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - t0.QuadPart) /
                static_cast<double>(qpcFreq.QuadPart);
            if (elapsed >= opts.durationSec) {
                break;
            }
        }
    }

    engine.Stop();
    const stagee::StageELiveSnapshot finalSnap = engine.GetSnapshot();
    const int exitCode = (finalSnap.lastShutdownVerified && finalSnap.lastShutdownClean) ? 0 : 2;
    WriteLauncherStatusJson(opts.statusFilePath, false, true, opts.engine.language, finalSnap, "", exitCode);
    return exitCode;
}

class ScopedStaComInit {
public:
    ScopedStaComInit() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninit_ = (hr == S_OK || hr == S_FALSE);
    }
    ~ScopedStaComInit() {
        if (uninit_) {
            CoUninitialize();
        }
    }
    ScopedStaComInit(const ScopedStaComInit&) = delete;
    ScopedStaComInit& operator=(const ScopedStaComInit&) = delete;

private:
    bool uninit_ = false;
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    EnablePerMonitorDpiAwareness();
    ScopedStaComInit staCom;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    LauncherCliOptions opts;
    std::string parseErr;
    if (!ParseCommandLineOptions(opts, parseErr)) {
        const stagee::UiLanguage fallbackLang = stagee::ResolveEffectiveLanguage(
            opts.hasCliLanguageOverride ? opts.cliLanguageOverride : stagee::UiLanguage::Auto);
        if (!opts.headless) {
            const bool en = (fallbackLang == stagee::UiLanguage::EnUs);
            const std::wstring msg = (en ? L"Invalid command-line argument:\n" : L"命令行参数错误：\n") +
                                     staged::Utf8ToWide(parseErr);
            MessageBoxW(
                nullptr,
                msg.c_str(),
                kUnifiedWindowTitle,
                MB_ICONERROR | MB_OK);
        }
        return 2;
    }

    const std::wstring settingsPath = opts.configFilePath.empty()
        ? stagee::ResolveDefaultSettingsFilePath()
        : opts.configFilePath;

    stagee::LauncherSettings persistedSettings{};
    std::string loadSettingsErr;
    if (!stagee::LoadLauncherSettings(settingsPath, persistedSettings, loadSettingsErr)) {
        persistedSettings = stagee::LauncherSettings{};
    }

    opts.engine.language = opts.hasCliLanguageOverride
        ? opts.cliLanguageOverride
        : persistedSettings.language;
    opts.engine.source = persistedSettings.source;

    if (opts.showHelp) {
        const bool en = (stagee::ResolveEffectiveLanguage(opts.engine.language) == stagee::UiLanguage::EnUs);
        const wchar_t* helpTextZh =
            L"GTA IV Independence FM — Audio Bridge (Stage E1 + Stream Anything)\n\n"
            L"日常使用：直接双击 iv_apple_music_radio.exe 即可自动发现配置的发声程序并驻留系统托盘。\n"
            L"界面顶部菜单栏『选项 -> 设置』支持切换语言（中/英）以及选择兼容的 Windows 音频应用 (Stream Anything)。\n\n"
            L"可选命令行参数：\n"
            L"  --lang <auto|zh|en>      指定本次界面语言（也可在界面『选项 -> 设置』中永久保存）\n"
            L"  --source-exe <exe|path>  指定本次捕获的发声程序（例如 Spotify.exe 或完整路径）\n"
            L"  --config-path <path>     指定设置 JSON 文件路径\n"
            L"  --start-minimized        启动后直接最小化驻留托盘\n"
            L"  --no-auto-start          打开面板但不自动启动桥接\n"
            L"  --log-dir <dir>          自定义会话日志目录（默认写入仓库或程序旁侧 logs/）\n"
            L"  --keep-logs <N>          保留最近 N 份会话日志（默认 10）\n"
            L"  --max-log-mb <MB>        单个日志文件分片上限（默认 20 MiB）\n"
            L"  --telemetry-jsonl <path> 固定写入单个遥测文件路径\n"
            L"  --headless               无界面后台运行模式（供自动化测试与脚本调用）";
        const wchar_t* helpTextEn =
            L"GTA IV Independence FM — Audio Bridge (Stage E1 + Stream Anything)\n\n"
            L"Daily Usage: Double-click iv_apple_music_radio.exe to auto-discover your configured audio app and stay in the system tray.\n"
            L"Use 'Options -> Settings' in the top menu bar to switch language or pick a compatible Windows audio app (Stream Anything).\n\n"
            L"Optional CLI Flags:\n"
            L"  --lang <auto|zh|en>      Override UI language for this run (or persist via Options -> Settings)\n"
            L"  --source-exe <exe|path>  Override target audio executable for this run (e.g. Spotify.exe)\n"
            L"  --config-path <path>     Custom settings JSON path\n"
            L"  --start-minimized        Start minimized directly in the system tray\n"
            L"  --no-auto-start          Open the panel without auto-starting the bridge\n"
            L"  --log-dir <dir>          Custom session log directory\n"
            L"  --keep-logs <N>          Retain latest N session logs (default: 10)\n"
            L"  --max-log-mb <MB>        Max size per log file part in MiB (default: 20)\n"
            L"  --telemetry-jsonl <path> Write telemetry to a single explicit JSONL file\n"
            L"  --headless               Run headless in background (for automated test gates)";
        if (!opts.headless) {
            MessageBoxW(
                nullptr,
                en ? helpTextEn : helpTextZh,
                kUnifiedWindowTitle,
                MB_ICONINFORMATION | MB_OK);
        }
        return 0;
    }

    // Single-Instance Management: wake up existing instance and exit cleanly with code 0
    const std::wstring singleMutexName = opts.instanceName + L"_Mutex";
    const std::wstring activateEventName = opts.instanceName + L"_ActivateEvent";

    ScopedHandle hSingleMutex(CreateMutexW(nullptr, FALSE, singleMutexName.c_str()));
    if (!hSingleMutex.IsValid()) {
        return 1;
    }

    const DWORD singleWait = WaitForSingleObject(hSingleMutex.Get(), 0u);
    if (singleWait == WAIT_TIMEOUT) {
        SignalNamedEventW(activateEventName);
        stagee::StageELiveSnapshot emptySnap{};
        emptySnap.activeSource = opts.engine.source;
        emptySnap.targetDisplayNameUtf8 = stagee::FormatAudioSourceDisplayNameUtf8(opts.engine.source);
        WriteLauncherStatusJson(opts.statusFilePath, true, false, opts.engine.language, emptySnap, "", 0);
        return 0;
    }
    if (singleWait != WAIT_OBJECT_0 && singleWait != WAIT_ABANDONED) {
        return 1;
    }

    ScopedHandle hActivateEvent(CreateEventW(nullptr, FALSE, FALSE, activateEventName.c_str()));
    ScopedHandle hExternalStopEvent(OpenOrCreateNamedEventW(opts.stopEventName, true));

    int rc = 0;
    if (opts.headless) {
        rc = RunHeadlessMode(opts, hExternalStopEvent.Get());
    } else {
        LauncherWindowApp app(
            hInstance,
            opts,
            settingsPath,
            persistedSettings,
            hActivateEvent.Get(),
            hExternalStopEvent.Get());
        rc = app.RunGui();
    }

    ReleaseMutex(hSingleMutex.Get());
    return rc;
}
