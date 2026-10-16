#pragma once

#include <afxwin.h>

#include <string>

// ---------------------------------------------------------------------------
//  Everything the user can tune. Persisted as INI; the API key is stored as a
//  DPAPI blob so it never sits in plain text on disk.
// ---------------------------------------------------------------------------
struct Settings {
    // --- Whisper endpoint ---------------------------------------------------
    CStringW baseUrl = L"http://127.0.0.1:8000/v1";
    CStringW apiKey;                 // never persisted in clear text
    CStringW model = L"whisper-1";
    CStringW language;               // empty = let the server detect it
    CStringW prompt;                 // style hint handed to Whisper as `prompt`
    // Terms that are likely to come up: project names, brands, people. Whisper
    // biases its decoding towards what it sees, and this list goes last in the
    // prompt, where it has the most influence.
    CStringW glossary;
    CStringW audioFormat = L"m4a";   // "m4a" = AAC upload, "wav" = lossless
    int aacBitrate = 48000;          // bits per second for the AAC encoder

    // --- Endpointing -------------------------------------------------------
    double silenceMs = 700.0;
    double minSpeechMs = 250.0;
    double maxSegmentMs = 20000.0;
    double thresholdRatio = 3.0;

    // --- Text injection ----------------------------------------------------
    // Win+Shift+Z by default: free on a stock Windows 11 and reachable with the left
    // hand. Windows itself uses Win+H, and Win+Shift+A/C/M/P/R/S/T/V/W are taken.
    // The Win key can also bring the shell forward, so the app remembers the last
    // window that really held a caret and prefers it over the taskbar or Start.
    CStringW hotkey = L"Win+Shift+Z";
    bool appendSpace = true;
    bool newlineAsShiftEnter = true;
    bool clipboardFallback = true;
    bool usePromptContext = true;

    // --- Application -------------------------------------------------------
    bool autoStart = false;
    bool debugLog = false;

    // --- HTTP timeouts -----------------------------------------------------
    int connectTimeoutMs = 10000;
    int receiveTimeoutMs = 120000;

    CStringW ConfigPath() const;
    void Load();
    void Save() const;
    void ClampToSaneRanges();
};

// --- hotkey helpers ---------------------------------------------------------
bool ParseHotkey(const CStringW& spec, UINT& mods, UINT& vk);
CStringW FormatHotkey(UINT mods, UINT vk);
CStringW HotkeyDisplayName(UINT mods, UINT vk);   // "Win+Shift+H"
CStringW HotkeyDisplayName(UINT vk);              // key name only

// --- secrets ----------------------------------------------------------------
std::string ProtectSecret(const std::string& plain);
std::string UnprotectSecret(const std::string& base64Blob);

// --- start-up ---------------------------------------------------------------
// The registry, not the INI, is what Windows actually reads at logon, so the
// state is queried rather than assumed. Task Manager switches a startup entry
// off without deleting it, and the INI knows nothing about a copy that was
// registered from another folder, hence the three separate flags.
struct AutoStartStatus {
    bool registered = false;   // HKCU\...\Run\WinWhisper exists
    bool approved = true;      // ...and Task Manager has not switched it off
    bool pointsHere = false;   // ...and it runs this very executable
    CStringW command;          // raw command line from the Run entry
};

AutoStartStatus GetAutoStartStatus();
bool SetAutoStartEnabled(bool enable);
bool IsAutoStartEnabled();