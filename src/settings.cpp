#include "settings.h"

#include <shlwapi.h>
#include <wincrypt.h>

#include <stdexcept>
#include <vector>

#include "util.h"

namespace {

constexpr wchar_t kSecServer[] = L"server";
constexpr wchar_t kSecVad[] = L"vad";
constexpr wchar_t kSecInput[] = L"input";
constexpr wchar_t kSecApp[] = L"app";
constexpr wchar_t kSecPanel[] = L"panel";

const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// -------------------------------------------------------------- INI glue ---

CStringW GetStr(const wchar_t* section, const wchar_t* key, const CStringW& def, const CStringW& file)
{
    WCHAR buf[2048] = {};
    ::GetPrivateProfileStringW(section, key, def, buf, ARRAYSIZE(buf), file);
    return CStringW(buf);
}

double GetNum(const wchar_t* section, const wchar_t* key, double def, const CStringW& file)
{
    const CStringW raw = GetStr(section, key, L"", file);
    double v = 0;
    if (raw.IsEmpty() || !util::ParseDouble(raw, v))
        return def;
    return v;
}

bool GetBool(const wchar_t* section, const wchar_t* key, bool def, const CStringW& file)
{
    const CStringW raw = GetStr(section, key, L"", file);
    if (raw.IsEmpty())
        return def;
    return raw.CompareNoCase(L"1") == 0 || raw.CompareNoCase(L"true") == 0 || raw.CompareNoCase(L"on") == 0;
}

void PutStr(const wchar_t* section, const wchar_t* key, const CStringW& value, const CStringW& file)
{
    ::WritePrivateProfileStringW(section, key, value, file);
}

void PutNum(const wchar_t* section, const wchar_t* key, double value, const wchar_t* fmt,
            const CStringW& file)
{
    CStringW s;
    s.Format(fmt, value);
    PutStr(section, key, s, file);
}

void PutBool(const wchar_t* section, const wchar_t* key, bool value, const CStringW& file)
{
    PutStr(section, key, value ? L"1" : L"0", file);
}

// -------------------------------------------------------------- base64 -----

const wchar_t kB64[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

CStringW Base64Encode(const std::vector<unsigned char>& data)
{
    CStringW out;
    out.Preallocate(static_cast<int>((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned int v = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < data.size())
            v |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < data.size())
            v |= data[i + 2];

        out.AppendChar(kB64[(v >> 18) & 0x3F]);
        out.AppendChar(kB64[(v >> 12) & 0x3F]);
        out.AppendChar(i + 1 < data.size() ? kB64[(v >> 6) & 0x3F] : L'=');
        out.AppendChar(i + 2 < data.size() ? kB64[v & 0x3F] : L'=');
    }
    return out;
}

std::vector<unsigned char> Base64Decode(const CStringW& text)
{
    int table[256];
    for (int i = 0; i < 256; ++i)
        table[i] = -1;
    for (int i = 0; i < 64; ++i)
        table[static_cast<unsigned char>(kB64[i])] = i;

    std::vector<unsigned char> out;
    out.reserve(text.GetLength() * 3 / 4 + 3);

    unsigned int buffer = 0;
    int bits = 0;
    for (int i = 0; i < text.GetLength(); ++i) {
        const int v = table[static_cast<unsigned char>(text[i])];
        if (v < 0)
            continue;  // skip '=' and whitespace
        buffer = (buffer << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

// --------------------------------------------------------------- Settings ---

CStringW Settings::ConfigPath() const
{
    return util::EnsureAppDataDir() + L"\\settings.ini";
}

void Settings::ClampToSaneRanges()
{
    if (silenceMs < 150) silenceMs = 150;
    if (silenceMs > 5000) silenceMs = 5000;
    if (minSpeechMs < 50) minSpeechMs = 50;
    if (minSpeechMs > 5000) minSpeechMs = 5000;
    if (maxSegmentMs < 2000) maxSegmentMs = 2000;
    if (maxSegmentMs > 29000) maxSegmentMs = 29000;   // Whisper's hard window is 30 s
    if (thresholdRatio < 1.2) thresholdRatio = 1.2;
    if (thresholdRatio > 20.0) thresholdRatio = 20.0;
    if (connectTimeoutMs < 1000) connectTimeoutMs = 1000;
    if (receiveTimeoutMs < 1000) receiveTimeoutMs = 1000;
    baseUrl.Trim();
    baseUrl.TrimRight(L'/');
    model.Trim();
}

void Settings::Load()
{
    const CStringW file = ConfigPath();
    if (!::PathFileExistsW(file))
        return;

    baseUrl = GetStr(kSecServer, L"base_url", baseUrl, file);
    const CStringW keyBlob = GetStr(kSecServer, L"api_key_dpapi", L"", file);
    if (!keyBlob.IsEmpty()) {
        try {
            const std::string plain = UnprotectSecret(util::WideToUtf8(keyBlob));
            apiKey = CStringW(util::Utf8ToWide(plain).c_str());
        } catch (...) {
            apiKey.Empty();  // blob unreadable (e.g. different user / profile wiped)
        }
    }
    model = GetStr(kSecServer, L"model", model, file);
    language = GetStr(kSecServer, L"language", language, file);
    prompt = GetStr(kSecServer, L"prompt", prompt, file);
    glossary = GetStr(kSecServer, L"glossary", glossary, file);
    audioFormat = GetStr(kSecServer, L"audio_format", audioFormat, file);
    aacBitrate = util::ParseInt(GetStr(kSecServer, L"aac_bitrate", L"", file), aacBitrate);
    if (aacBitrate < 16000) aacBitrate = 16000;
    if (aacBitrate > 128000) aacBitrate = 128000;
    if (audioFormat.CompareNoCase(L"wav") != 0 && audioFormat.CompareNoCase(L"m4a") != 0)
        audioFormat = L"m4a";
    connectTimeoutMs = util::ParseInt(GetStr(kSecServer, L"connect_timeout_ms", L"", file), connectTimeoutMs);
    receiveTimeoutMs = util::ParseInt(GetStr(kSecServer, L"receive_timeout_ms", L"", file), receiveTimeoutMs);

    silenceMs = GetNum(kSecVad, L"silence_ms", silenceMs, file);
    minSpeechMs = GetNum(kSecVad, L"min_speech_ms", minSpeechMs, file);
    maxSegmentMs = GetNum(kSecVad, L"max_segment_ms", maxSegmentMs, file);
    thresholdRatio = GetNum(kSecVad, L"threshold_ratio", thresholdRatio, file);

    hotkey = GetStr(kSecInput, L"hotkey", hotkey, file);
    appendSpace = GetBool(kSecInput, L"append_space", appendSpace, file);
    clipboardFallback = GetBool(kSecInput, L"clipboard_fallback", clipboardFallback, file);
    usePromptContext = GetBool(kSecInput, L"use_prompt_context", usePromptContext, file);

    autoStart = GetBool(kSecApp, L"auto_start", autoStart, file);
    debugLog = GetBool(kSecApp, L"debug_log", debugLog, file);

    panelPlaced = GetBool(kSecPanel, L"placed", panelPlaced, file);
    panelX = util::ParseInt(GetStr(kSecPanel, L"x", L"", file), panelX);
    panelY = util::ParseInt(GetStr(kSecPanel, L"y", L"", file), panelY);

    ClampToSaneRanges();
}

void Settings::Save() const
{
    const CStringW file = ConfigPath();
    util::EnsureAppDataDir();

    PutStr(kSecServer, L"base_url", baseUrl, file);
    if (apiKey.IsEmpty()) {
        PutStr(kSecServer, L"api_key_dpapi", CStringW(), file);
    } else {
        const CStringW blob(util::Utf8ToWide(ProtectSecret(util::WideToUtf8(apiKey))).c_str());
        PutStr(kSecServer, L"api_key_dpapi", blob, file);
    }
    PutStr(kSecServer, L"model", model, file);
    PutStr(kSecServer, L"language", language, file);
    PutStr(kSecServer, L"prompt", prompt, file);
    PutStr(kSecServer, L"glossary", glossary, file);
    PutStr(kSecServer, L"audio_format", audioFormat, file);
    CStringW bitrate;
    bitrate.Format(L"%d", aacBitrate);
    PutStr(kSecServer, L"aac_bitrate", bitrate, file);

    CStringW text;
    text.Format(L"%d", connectTimeoutMs);
    PutStr(kSecServer, L"connect_timeout_ms", text, file);
    text.Format(L"%d", receiveTimeoutMs);
    PutStr(kSecServer, L"receive_timeout_ms", text, file);

    PutNum(kSecVad, L"silence_ms", silenceMs, L"%.0f", file);
    PutNum(kSecVad, L"min_speech_ms", minSpeechMs, L"%.0f", file);
    PutNum(kSecVad, L"max_segment_ms", maxSegmentMs, L"%.0f", file);
    PutNum(kSecVad, L"threshold_ratio", thresholdRatio, L"%.2f", file);

    PutStr(kSecInput, L"hotkey", hotkey, file);
    PutBool(kSecInput, L"append_space", appendSpace, file);
    PutBool(kSecInput, L"clipboard_fallback", clipboardFallback, file);
    PutBool(kSecInput, L"use_prompt_context", usePromptContext, file);

    PutBool(kSecApp, L"auto_start", autoStart, file);
    PutBool(kSecApp, L"debug_log", debugLog, file);

    PutBool(kSecPanel, L"placed", panelPlaced, file);
    PutNum(kSecPanel, L"x", panelX, L"%.0f", file);
    PutNum(kSecPanel, L"y", panelY, L"%.0f", file);
}

// ----------------------------------------------------------------- DPAPI ----

std::string ProtectSecret(const std::string& plain)
{
    DATA_BLOB in = {};
    in.pbData = reinterpret_cast<PBYTE>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB out = {};
    if (!::CryptProtectData(&in, L"WinWhisper API key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out)) {
        throw std::runtime_error("CryptProtectData failed");
    }
    const std::vector<unsigned char> bytes(out.pbData, out.pbData + out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return util::WideToUtf8(Base64Encode(bytes));
}

std::string UnprotectSecret(const std::string& base64Blob)
{
    const std::vector<unsigned char> bytes = Base64Decode(CStringW(util::Utf8ToWide(base64Blob).c_str()));
    if (bytes.empty())
        throw std::runtime_error("empty blob");

    DATA_BLOB in = {};
    in.pbData = const_cast<BYTE*>(bytes.data());
    in.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB out = {};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("CryptUnprotectData failed");
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return plain;
}

// ------------------------------------------------------------- autostart ----
//
// Two places in the registry describe a startup entry:
//
//   ...\CurrentVersion\Run\WinWhisper                       what logon runs
//   ...\Explorer\StartupApproved\Run\WinWhisper             on/off from the UI
//
// Task Manager's "Startup apps" tab does not delete the first one when you
// switch an app off: it writes a 12-byte blob into the second, whose first byte
// is even when the entry is allowed and odd when it is blocked. An app that only
// checks Run would keep claiming it starts with Windows while Windows quietly
// skips it.
const wchar_t* const kStartupApprovedKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

const wchar_t* const kRunValueName = L"WinWhisper";

CStringW CurrentExePath()
{
    // MAX_PATH is not enough on machines with long paths enabled.
    std::vector<wchar_t> buffer(32768);
    const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size())
        return CStringW();
    return CStringW(buffer.data(), static_cast<int>(len));
}

// Resolves an 8.3 short name to the real one and expands any relative parts, so
// "C:\PROGRA~1\WinWhisper\WinWhisper.exe" still compares equal to the full path
// of the running copy.
CStringW NormalizePath(const CStringW& path)
{
    if (path.IsEmpty())
        return path;
    WCHAR full[32768] = {};
    if (::GetFullPathNameW(path.GetString(), ARRAYSIZE(full), full, nullptr) == 0)
        return path;
    WCHAR longName[32768] = {};
    if (::GetLongPathNameW(full, longName, ARRAYSIZE(longName)) > 0)
        return CStringW(longName);
    return CStringW(full);
}

// The Run value is a command line ("C:\path\WinWhisper.exe" --autostart), so the
// executable is the first token, quoted or not.
CStringW ExecutableFromCommand(const CStringW& command)
{
    CStringW text = command;
    text.Trim();
    if (text.IsEmpty())
        return text;
    if (text[0] == L'"') {
        const int closing = text.Find(L'"', 1);
        if (closing > 1)
            return text.Mid(1, closing - 1);
        return text.Mid(1);
    }
    const int space = text.Find(L' ');
    return space > 0 ? text.Left(space) : text;
}

bool ReadRunCommand(CStringW* command)
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = 0;
    bool found = false;
    if (::RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t) &&
        bytes <= 64 * 1024) {
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        if (::RegQueryValueExW(key, kRunValueName, nullptr, &type,
                               reinterpret_cast<BYTE*>(buffer.data()), &bytes) == ERROR_SUCCESS) {
            *command = CStringW(buffer.data());
            found = true;
        }
    }
    ::RegCloseKey(key);
    return found;
}

// No StartupApproved value at all means "never touched by the user": allowed.
bool ReadStartupApproved()
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return true;

    BYTE data[64] = {};
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const LONG status = ::RegQueryValueExW(key, kRunValueName, nullptr, &type, data, &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || bytes < 1 || type != REG_BINARY)
        return true;
    // 0x02 = enabled, 0x03 = disabled (and 0x06/0x07 for entries created by the
    // Start menu) - the low bit carries the state.
    return (data[0] & 0x01) == 0;
}

// Lets Windows run the entry again after Task Manager switched it off.
void ClearStartupApprovedBlock()
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kStartupApprovedKey, 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    // The layout Windows itself writes: state byte plus the timestamp of the
    // change. 0x02, then a zeroed FILETIME.
    BYTE enabled[12] = {};
    enabled[0] = 0x02;
    ::RegSetValueExW(key, kRunValueName, 0, REG_BINARY, enabled, sizeof(enabled));
    ::RegCloseKey(key);
}

AutoStartStatus GetAutoStartStatus()
{
    AutoStartStatus status;
    status.registered = ReadRunCommand(&status.command);
    if (!status.registered)
        return status;

    status.approved = ReadStartupApproved();
    const CStringW registeredExe = NormalizePath(ExecutableFromCommand(status.command));
    const CStringW runningExe = NormalizePath(CurrentExePath());
    status.pointsHere = !registeredExe.IsEmpty() && !runningExe.IsEmpty() &&
                        ::CompareStringOrdinal(registeredExe.GetString(), -1,
                                               runningExe.GetString(), -1, TRUE) == CSTR_EQUAL;
    return status;
}

bool SetAutoStartEnabled(bool enable)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                          nullptr) != ERROR_SUCCESS) {
        util::Log(L"[autostart] RegCreateKeyEx(Run) failed, err=%lu", ::GetLastError());
        return false;
    }

    bool ok = true;
    if (enable) {
        const CStringW exe = CurrentExePath();
        if (exe.IsEmpty()) {
            ok = false;
        } else {
            // Quoted: the path may contain spaces, and without the quotes Windows
            // would try to run "C:\Program".
            CStringW cmd;
            cmd.Format(L"\"%s\" --autostart", exe.GetString());
            ok = ::RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(cmd.GetString()),
                                  static_cast<DWORD>((cmd.GetLength() + 1) * sizeof(wchar_t))) ==
                 ERROR_SUCCESS;
        }
    } else {
        const LONG status = ::RegDeleteValueW(key, kRunValueName);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    ::RegCloseKey(key);

    if (ok && enable) {
        // Only when the entry really is blocked. Writing this key on every enable
        // would be pointless - and it is not what an ordinary application touches,
        // which is exactly the kind of thing Defender's behaviour model flags
        // (a copy in %TEMP% was once quarantined as Behavior:Win32/Persistence.A!ml
        // for writing the Run key; the same build writing it from its install
        // folder was never touched).
        if (!ReadStartupApproved())
            ClearStartupApprovedBlock();
    }
    // Switching the entry off leaves any StartupApproved blob behind: it is inert
    // while the Run value is gone, and enabling again rewrites it if it blocks.

    const AutoStartStatus after = GetAutoStartStatus();
    util::Log(L"[autostart] %s -> %s (registered=%d approved=%d pointsHere=%d cmd=%s)",
              enable ? L"on" : L"off", ok ? L"ok" : L"failed", after.registered ? 1 : 0,
              after.approved ? 1 : 0, after.pointsHere ? 1 : 0, after.command.GetString());
    return ok;
}

bool IsAutoStartEnabled()
{
    const AutoStartStatus status = GetAutoStartStatus();
    return status.registered && status.approved;
}

// --------------------------------------------------------------- hotkey -----

namespace {

struct NamedKey {
    const wchar_t* name;
    UINT vk;
};

const NamedKey kNamedKeys[] = {
    {L"Space", VK_SPACE},          {L"Enter", VK_RETURN},      {L"Escape", VK_ESCAPE},
    {L"Tab", VK_TAB},              {L"Backspace", VK_BACK},    {L"Insert", VK_INSERT},
    {L"Delete", VK_DELETE},        {L"Home", VK_HOME},         {L"End", VK_END},
    {L"PageUp", VK_PRIOR},         {L"PageDown", VK_NEXT},     {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT},          {L"Up", VK_UP},             {L"Down", VK_DOWN},
    {L"Plus", VK_OEM_PLUS},        {L"Minus", VK_OEM_MINUS},   {L"Comma", VK_OEM_COMMA},
    {L"Period", VK_OEM_PERIOD},    {L"Slash", VK_OEM_2},       {L"Semicolon", VK_OEM_1},
    {L"Quote", VK_OEM_7},          {L"BracketLeft", VK_OEM_4}, {L"BracketRight", VK_OEM_6},
    {L"Backslash", VK_OEM_5},      {L"Grave", VK_OEM_3},       {L"CapsLock", VK_CAPITAL},
};

}  // namespace

bool ParseHotkey(const CStringW& spec, UINT& mods, UINT& vk)
{
    mods = 0;
    vk = 0;

    CStringW s = spec;
    s.Trim();
    if (s.IsEmpty())
        return false;

    // Split on '+' (VK_OEM_PLUS is spelled "Plus" so a bare '+' is always a separator).
    std::vector<CStringW> parts;
    int start = 0;
    for (int i = 0; i <= s.GetLength(); ++i) {
        if (i == s.GetLength() || s[i] == L'+') {
            parts.push_back(s.Mid(start, i - start));
            start = i + 1;
        }
    }

    for (size_t i = 0; i < parts.size(); ++i) {
        CStringW tok = parts[i];
        tok.Trim();
        if (tok.IsEmpty())
            continue;

        if (tok.CompareNoCase(L"Ctrl") == 0 || tok.CompareNoCase(L"Control") == 0) {
            mods |= MOD_CONTROL;
        } else if (tok.CompareNoCase(L"Alt") == 0) {
            mods |= MOD_ALT;
        } else if (tok.CompareNoCase(L"Shift") == 0) {
            mods |= MOD_SHIFT;
        } else if (tok.CompareNoCase(L"Win") == 0 || tok.CompareNoCase(L"Windows") == 0) {
            mods |= MOD_WIN;
        } else {
            if (tok.GetLength() == 1) {
                const wchar_t ch = towupper(tok[0]);
                if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
                    vk = static_cast<UINT>(ch);
                else
                    return false;
            } else if ((tok[0] == L'F' || towlower(tok[0]) == L'f') &&
                       util::ParseInt(tok.Mid(1), -1) >= 1 &&
                       util::ParseInt(tok.Mid(1), -1) <= 24) {
                vk = VK_F1 + (util::ParseInt(tok.Mid(1), 1) - 1);
            } else {
                bool found = false;
                for (const NamedKey& nk : kNamedKeys) {
                    if (tok.CompareNoCase(nk.name) == 0) {
                        vk = nk.vk;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
        }
    }

    if (vk == 0)
        return false;
    return true;
}

CStringW HotkeyDisplayName(UINT vk)
{
    wchar_t buf[64] = {};
    // GetKeyNameText wants an lParam-shaped value: the scan code in bits 16-23,
    // not the virtual-key code. Passing the VK directly yields nothing.
    const UINT scan = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const LONG param = static_cast<LONG>(scan) << 16;
    if (::GetKeyNameTextW(param, buf, ARRAYSIZE(buf)) > 0 && buf[0] != L'\0')
        return CStringW(buf);

    for (const NamedKey& nk : kNamedKeys) {
        if (nk.vk == vk)
            return nk.name;
    }
    CStringW s;
    s.Format(L"VK_%02X", vk);
    return s;
}

CStringW FormatHotkey(UINT mods, UINT vk)
{
    CStringW out;
    if (mods & MOD_ALT)
        out += L"Alt+";
    if (mods & MOD_CONTROL)
        out += L"Ctrl+";
    if (mods & MOD_SHIFT)
        out += L"Shift+";
    if (mods & MOD_WIN)
        out += L"Win+";
    out += HotkeyDisplayName(vk);
    return out;
}

CStringW HotkeyDisplayName(UINT mods, UINT vk)
{
    return FormatHotkey(mods, vk);
}