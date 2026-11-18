#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace probe {

class AsyncLogger {
public:
    static AsyncLogger& Instance();

    void Init(HMODULE selfModule);
    void Shutdown();

    int64_t NowMicros() const;

    // Non-blocking enqueue to preallocated ring buffer; flushed by low-priority background thread
    void LogJsonEvent(const char* eventType, int32_t readerId, const char* readerKind, const char* jsonFieldsFmt, ...);
    void LogText(const char* fmt, ...);

    static std::string WideToUtf8Escaped(const wchar_t* wstr);
    static std::string GuidToString(const GUID& guid);

private:
    AsyncLogger() = default;
    ~AsyncLogger() = default;

    struct Slot {
        volatile LONG ready; // 0 = empty, 1 = writing, 2 = ready
        bool isJson;
        char text[760];
    };

    static constexpr uint32_t kRingSize = 32768; // Power of two (~25MB BSS, handles tens of thousands of samples)
    static constexpr uint32_t kRingMask = kRingSize - 1;

    static DWORD WINAPI WorkerThreadProc(LPVOID param);
    void DrainAvailable();

    Slot ring_[kRingSize]{};
    alignas(64) volatile LONG writeIdx_ = 0;
    alignas(64) uint32_t readIdx_ = 0;
    volatile LONG droppedCount_ = 0;
    volatile LONG running_ = 0;
    bool initialized_ = false;

    HANDLE wakeEvent_ = nullptr;
    HANDLE workerThread_ = nullptr;
    HANDLE logFile_ = INVALID_HANDLE_VALUE;
    HANDLE jsonlFile_ = INVALID_HANDLE_VALUE;

    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER qpcStart_{};
};

} // namespace probe
