#include "util.h"

#include <shlwapi.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace util {

// ---------------------------------------------------------------- text ----

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], need);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (need <= 0)
        return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], need, nullptr, nullptr);
    return out;
}

std::string WideToUtf8(const CStringW& s)
{
    return WideToUtf8(std::wstring(s.GetString(), static_cast<size_t>(s.GetLength())));
}

// ---------------------------------------------------------------- paths ----

CStringW AppDataDir()
{
    WCHAR path[MAX_PATH] = {};
    DWORD n = ::GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return CStringW();
    return CStringW(path, static_cast<int>(n)) + L"\\WinWhisper";
}

CStringW EnsureAppDataDir()
{
    const CStringW dir = AppDataDir();
    if (!dir.IsEmpty() && !::PathIsDirectoryW(dir)) {
        if (::CreateDirectoryW(dir, nullptr) == FALSE && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return CStringW();
    }
    return dir;
}

// ------------------------------------------------------------------ log ----

namespace {

std::mutex g_logMutex;
bool g_logEnabled = false;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
constexpr DWORD kMaxLogBytes = 2 * 1024 * 1024;

void OpenLogLocked()
{
    if (g_logFile != INVALID_HANDLE_VALUE)
        return;
    const CStringW path = EnsureAppDataDir() + L"\\winwhisper.log";
    g_logFile = ::CreateFileW(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = {};
        if (::GetFileSizeEx(g_logFile, &size) && size.QuadPart > kMaxLogBytes) {
            ::CloseHandle(g_logFile);
            ::MoveFileW(path, path + L".1");  // keep exactly one previous log
            g_logFile = ::CreateFileW(path, FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
}

}  // namespace

void SetLogEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logEnabled = enabled;
    if (!enabled && g_logFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

bool IsLogEnabled()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logEnabled;
}

void Log(const wchar_t* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logEnabled)
        return;
    OpenLogLocked();
    if (g_logFile == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME st = {};
    ::GetLocalTime(&st);

    wchar_t head[64] = {};
    ::swprintf_s(head, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    wchar_t body[2048] = {};
    va_list args;
    va_start(args, fmt);
    ::_vsnwprintf_s(body, _TRUNCATE, fmt, args);
    va_end(args);

    CStringW line;
    line.Format(L"%s%s\r\n", head, body);

    DWORD written = 0;
    ::WriteFile(g_logFile, line.GetString(), static_cast<DWORD>(line.GetLength() * sizeof(wchar_t)),
                &written, nullptr);
}

// --------------------------------------------------------------- numbers ---

CStringW FormatDouble(double value, int digits)
{
    CStringW s;
    s.Format(L"%.*f", digits, value);
    return s;
}

bool ParseDouble(const CStringW& text, double& out)
{
    CStringW s = text;
    s.Trim();
    if (s.IsEmpty())
        return false;
    wchar_t* end = nullptr;
    const double v = ::wcstod(s, &end);
    if (end == s.GetString())
        return false;
    out = v;
    return true;
}

int ParseInt(const CStringW& text, int fallback)
{
    double v = 0;
    if (!ParseDouble(text, v))
        return fallback;
    return static_cast<int>(v);
}

// ------------------------------------------------------------- utilities ---

namespace {
std::mutex g_fgMutex;
HWND g_pendingForeground = nullptr;
}  // namespace

void SetPendingForeground(HWND hwnd)
{
    std::lock_guard<std::mutex> lock(g_fgMutex);
    g_pendingForeground = hwnd;
}

HWND TakePendingForeground()
{
    std::lock_guard<std::mutex> lock(g_fgMutex);
    HWND h = g_pendingForeground;
    g_pendingForeground = nullptr;
    return h;
}

}  // namespace util