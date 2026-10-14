#pragma once

#include <afxwin.h>
#include <string>
#include <vector>

namespace util {

// ---------------------------------------------------------------- text ----
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::string WideToUtf8(const CStringW& s);

// ---------------------------------------------------------------- paths ----
// %APPDATA%\WinWhisper (created on demand)
CStringW AppDataDir();
CStringW EnsureAppDataDir();

// ------------------------------------------------------------------ log ----
void SetLogEnabled(bool enabled);
bool IsLogEnabled();
void Log(const wchar_t* fmt, ...);

// --------------------------------------------------------------- numbers ---
CStringW FormatDouble(double value, int digits = 1);
bool ParseDouble(const CStringW& text, double& out);
int ParseInt(const CStringW& text, int fallback);

// ------------------------------------------------------------- utilities ---
// Remembers the window that had focus before we popped up our overlay.
void SetPendingForeground(HWND hwnd);
HWND TakePendingForeground();

// ---------------------------------------------------------------- crash ----
// A shipped build has no debugger attached, so the faulting address, the symbol
// name, the source line and a minidump go to the log and next to it. Without
// this a crash report says only "0xc0000005 in WinWhisper.exe".
void InstallCrashHandler();

}  // namespace util