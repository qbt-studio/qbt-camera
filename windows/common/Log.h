#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

// Minimal UTF-8 logger: OutputDebugString plus an optional file.
namespace qbt {

inline std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::wstring& LogPath() {
    static std::wstring path;
    return path;
}

inline void SetLogFile(std::wstring path) {
    std::lock_guard lock(LogMutex());
    LogPath() = std::move(path);
}

inline void Log(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME time;
    GetLocalTime(&time);
    char line[1200];
    snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%lu] %s\n", time.wHour, time.wMinute, time.wSecond,
             time.wMilliseconds, GetCurrentProcessId(), message);
    OutputDebugStringA(line);

    std::lock_guard lock(LogMutex());
    if (LogPath().empty()) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, LogPath().c_str(), L"ab") == 0 && file) {
        fputs(line, file);
        fclose(file);
    }
}

} // namespace qbt
