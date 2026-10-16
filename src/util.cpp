#include "util.h"

#include <dbghelp.h>
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

void WriteAliveMarker(const CStringW& text)
{
    const CStringW path = EnsureAppDataDir() + L"\\alive.txt";
    HANDLE file = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    CStringW line;
    line.Format(L"%02d:%02d:%02d %s\r\n", st.wHour, st.wMinute, st.wSecond, text.GetString());

    DWORD written = 0;
    ::WriteFile(file, line.GetString(), static_cast<DWORD>(line.GetLength() * sizeof(wchar_t)),
                &written, nullptr);
    ::CloseHandle(file);
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

// ---------------------------------------------------------------- crash ----

namespace {

// Resolves an address against the PDB that sits next to the executable and
// writes "module!function+offset (file:line)" into the log.
void ReportAddress(HANDLE process, DWORD64 address, const wchar_t* what)
{
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    wchar_t name[512] = {};
    if (::SymFromAddr(process, address, &displacement, symbol)) {
        ::MultiByteToWideChar(CP_UTF8, 0, symbol->Name, -1, name, ARRAYSIZE(name));
    } else {
        wcscpy_s(name, L"<unknown>");
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    wchar_t location[512] = {};
    if (::SymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
        wchar_t file[400] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, line.FileName, -1, file, ARRAYSIZE(file));
        swprintf_s(location, L" (%s:%lu)", file, line.LineNumber);
    }

    Log(L"[crash] %s 0x%llX = %s+0x%llX%s", what, address, name, displacement, location);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const DWORD64 address = reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress);
    Log(L"[crash] exception 0x%08X at 0x%p", code, info->ExceptionRecord->ExceptionAddress);

    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    // The PDB lives next to the exe; the running directory may be anything.
    wchar_t modulePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    if (wchar_t* slash = wcsrchr(modulePath, L'\\'))
        *slash = L'\0';

    if (::SymInitializeW(process, modulePath, TRUE)) {
        ReportAddress(process, address, L"faulting");

        // Walk the stack so the caller chain is visible too.
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        for (int depth = 0; depth < 20; ++depth) {
            if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, ::GetCurrentThread(), &frame,
                               &context, nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64,
                               nullptr))
                break;
            if (frame.AddrPC.Offset == 0)
                break;
            ReportAddress(process, frame.AddrPC.Offset, L"  frame");
        }

        // A dump makes it possible to look at the wreckage later.
        const CStringW dump = EnsureAppDataDir() + L"\\crash.dmp";
        HANDLE file = ::CreateFileW(dump.GetString(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION details = {};
            details.ThreadId = ::GetCurrentThreadId();
            details.ExceptionPointers = info;
            details.ClientPointers = FALSE;
            if (::MiniDumpWriteDump(process, ::GetCurrentProcessId(), file,
                                    static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs |
                                                               MiniDumpWithIndirectlyReferencedMemory),
                                    &details, nullptr, nullptr))
                Log(L"[crash] dump: %s", dump.GetString());
            ::CloseHandle(file);
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void InstallCrashHandler()
{
    ::SetUnhandledExceptionFilter(CrashFilter);
}

}  // namespace util