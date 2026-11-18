#include "async_logger.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace probe {

AsyncLogger& AsyncLogger::Instance() {
    static AsyncLogger inst;
    return inst;
}

void AsyncLogger::Init(HMODULE selfModule) {
    if (initialized_) {
        return;
    }
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&qpcStart_);

    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(selfModule, modulePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
    std::wstring dir = lastSlash ? std::wstring(modulePath, lastSlash + 1) : L".\\";

    std::wstring logPath = dir + L"IVRadioProbe.log";
    std::wstring jsonlPath = dir + L"IVRadioProbe.jsonl";

    logFile_ = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    jsonlFile_ = CreateFileW(jsonlPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    InterlockedExchange(&running_, 1);
    workerThread_ = CreateThread(nullptr, 0, &AsyncLogger::WorkerThreadProc, this, 0, nullptr);
    if (workerThread_) {
        SetThreadPriority(workerThread_, THREAD_PRIORITY_BELOW_NORMAL);
    }
    initialized_ = true;
}

void AsyncLogger::Shutdown() {
    if (!initialized_) {
        return;
    }
    InterlockedExchange(&running_, 0);
    if (wakeEvent_) {
        SetEvent(wakeEvent_);
    }
    if (workerThread_) {
        // Note: During DllMain(DLL_PROCESS_DETACH) on process exit, waiting too long can deadlock loader lock,
        // so bound wait to 150ms and then do a direct final drain.
        WaitForSingleObject(workerThread_, 150);
        CloseHandle(workerThread_);
        workerThread_ = nullptr;
    }
    DrainAvailable();

    LONG dropped = InterlockedCompareExchange(&droppedCount_, 0, 0);
    if (dropped > 0 && logFile_ != INVALID_HANDLE_VALUE) {
        char tail[128];
        int len = std::snprintf(tail, sizeof(tail), "[WARN] Dropped ring log entries: %ld\r\n", dropped);
        if (len > 0) {
            DWORD written = 0;
            WriteFile(logFile_, tail, static_cast<DWORD>(len), &written, nullptr);
        }
    }

    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    if (logFile_ != INVALID_HANDLE_VALUE) {
        CloseHandle(logFile_);
        logFile_ = INVALID_HANDLE_VALUE;
    }
    if (jsonlFile_ != INVALID_HANDLE_VALUE) {
        CloseHandle(jsonlFile_);
        jsonlFile_ = INVALID_HANDLE_VALUE;
    }
    initialized_ = false;
}

int64_t AsyncLogger::NowMicros() const {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    int64_t ticks = now.QuadPart - qpcStart_.QuadPart;
    return (ticks * 1000000LL) / qpcFreq_.QuadPart;
}

void AsyncLogger::LogJsonEvent(const char* eventType, int32_t readerId, const char* readerKind, const char* jsonFieldsFmt, ...) {
    if (!initialized_) {
        return;
    }
    uint32_t idx = static_cast<uint32_t>(InterlockedIncrement(&writeIdx_) - 1) & kRingMask;
    Slot& s = ring_[idx];
    if (InterlockedCompareExchange(&s.ready, 1, 0) != 0) {
        InterlockedIncrement(&droppedCount_);
        return;
    }

    int64_t tsUs = NowMicros();
    DWORD tid = GetCurrentThreadId();

    char extra[520]{};
    if (jsonFieldsFmt && jsonFieldsFmt[0] != '\0') {
        va_list args;
        va_start(args, jsonFieldsFmt);
        std::vsnprintf(extra, sizeof(extra), jsonFieldsFmt, args);
        va_end(args);
    }

    s.isJson = true;
    if (extra[0] != '\0') {
        std::snprintf(s.text, sizeof(s.text),
                      "{\"ts_us\":%lld,\"tid\":%lu,\"event\":\"%s\",\"reader_id\":%d,\"kind\":\"%s\",%s}\n",
                      static_cast<long long>(tsUs), static_cast<unsigned long>(tid),
                      eventType, readerId, readerKind ? readerKind : "global", extra);
    } else {
        std::snprintf(s.text, sizeof(s.text),
                      "{\"ts_us\":%lld,\"tid\":%lu,\"event\":\"%s\",\"reader_id\":%d,\"kind\":\"%s\"}\n",
                      static_cast<long long>(tsUs), static_cast<unsigned long>(tid),
                      eventType, readerId, readerKind ? readerKind : "global");
    }

    InterlockedExchange(&s.ready, 2);
    if (wakeEvent_) {
        SetEvent(wakeEvent_);
    }
}

void AsyncLogger::LogText(const char* fmt, ...) {
    if (!initialized_) {
        return;
    }
    uint32_t idx = static_cast<uint32_t>(InterlockedIncrement(&writeIdx_) - 1) & kRingMask;
    Slot& s = ring_[idx];
    if (InterlockedCompareExchange(&s.ready, 1, 0) != 0) {
        InterlockedIncrement(&droppedCount_);
        return;
    }

    int64_t tsUs = NowMicros();
    DWORD tid = GetCurrentThreadId();

    char msg[640]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    s.isJson = false;
    std::snprintf(s.text, sizeof(s.text), "[%10.3f ms][TID:%5lu] %s\r\n",
                  static_cast<double>(tsUs) / 1000.0, static_cast<unsigned long>(tid), msg);

    InterlockedExchange(&s.ready, 2);
    if (wakeEvent_) {
        SetEvent(wakeEvent_);
    }
}

DWORD WINAPI AsyncLogger::WorkerThreadProc(LPVOID param) {
    auto* self = static_cast<AsyncLogger*>(param);
    while (InterlockedCompareExchange(&self->running_, 0, 0) != 0) {
        WaitForSingleObject(self->wakeEvent_, 40);
        self->DrainAvailable();
    }
    self->DrainAvailable();
    return 0;
}

void AsyncLogger::DrainAvailable() {
    while (true) {
        Slot& s = ring_[readIdx_ & kRingMask];
        LONG state = InterlockedCompareExchange(&s.ready, 2, 2);
        if (state != 2) {
            break;
        }
        size_t len = std::strlen(s.text);
        DWORD written = 0;
        if (s.isJson) {
            if (jsonlFile_ != INVALID_HANDLE_VALUE && len > 0) {
                WriteFile(jsonlFile_, s.text, static_cast<DWORD>(len), &written, nullptr);
            }
        } else {
            if (logFile_ != INVALID_HANDLE_VALUE && len > 0) {
                WriteFile(logFile_, s.text, static_cast<DWORD>(len), &written, nullptr);
            }
        }
        InterlockedExchange(&s.ready, 0);
        ++readIdx_;
    }
}

std::string AsyncLogger::WideToUtf8Escaped(const wchar_t* wstr) {
    if (!wstr) {
        return "";
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return "";
    }
    std::string raw(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, raw.data(), needed, nullptr, nullptr);

    std::string out;
    out.reserve(raw.size() + 16);
    for (unsigned char c : raw) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04x", c);
            out += hex;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string AsyncLogger::GuidToString(const GUID& g) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf),
                  "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  g.Data1, g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

} // namespace probe
