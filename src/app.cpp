// windows.h defines min/max macros that fight <algorithm>/<cmath>; this must
// come before any Windows header.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "app.h"

#include "overlay.h"
#include "resource.h"
#include "audio_encode.h"
#include "resampler.h"
#include "session.h"
#include "settings.h"
#include "settings_dlg.h"
#include "text_inserter.h"
#include "util.h"
#include "vad.h"
#include "wav.h"
#include "whisper_client.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

class CHostWindow;

// Defined below, once CHostWindow is complete.
void StopDictationFromHook(const wchar_t* reason);

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_TOGGLE_DICTATION = WM_APP + 2;
// Lets a second launch (or a script) ask the running copy for its settings
// window; WM_COMMAND cannot be posted across processes.
constexpr UINT WM_TOGGLE_SETTINGS = WM_APP + 3;

constexpr UINT ID_TRAY_LISTEN = 40001;
constexpr UINT ID_TRAY_SETTINGS = 40002;
constexpr UINT ID_TRAY_EXIT = 40003;
constexpr int HOTKEY_ID = 1;
constexpr UINT_PTR TIMER_UI = 1;

// How long an error stays on screen before the panel disappears again.
constexpr int kErrorTicks = 90;  // ~3 s at 33 ms

// --------------------------------------------------------------- app state --

CHostWindow* g_host = nullptr;
HHOOK g_keyboardHook = nullptr;
UINT g_hotkeyVk = 0;   // so the toggle itself does not count as "user typed"

UINT TaskbarCreatedMessage()
{
    static const UINT message = ::RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

// Modifiers alone are not "the user started typing": Shift capitalises, the
// Windows key opens the shell, and the hotkey needs its own modifiers.
bool IsModifierKey(DWORD vk)
{
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SCROLL:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_keyboardHook != nullptr &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        // Keys we inject ourselves land here too. Typing the recognised phrase
        // must never be mistaken for the user grabbing the keyboard - that bug
        // ended the dictation after every single phrase.
        const bool injected = info != nullptr && (info->flags & LLKHF_INJECTED) != 0;
        if (info != nullptr && !injected && !textins::IsTyping() &&
            !IsModifierKey(info->vkCode) && info->vkCode != g_hotkeyVk) {
            if (info->vkCode == VK_ESCAPE) {
                // ESC belongs to the dictation, not to the app underneath, so it
                // is swallowed. The overlay has no focus of its own, which is
                // exactly why a hook is needed to see it at all.
                StopDictationFromHook(L"ESC");
                return 1;
            }
            // Any other real key means the user went back to typing: stop, but
            // let the keystroke through - they are using it.
            StopDictationFromHook(L"клавиатура");
        }
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------- host -----

class CHostWindow : public CWnd {
public:
    void ApplyInitialSettings(const Settings& settings) { settings_ = settings; }
    bool CreateAndInit();
    void StartDictation();
    void StopDictation(const wchar_t* reason);

    bool IsDictating() const { return active_; }
    bool PanelRect(RECT* out)
    {
        if (out == nullptr || !overlay_.IsVisible())
            return false;
        overlay_.GetWindowRect(out);
        return true;    }

protected:
    afx_msg LRESULT OnTray(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnWinCommand(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnToggle(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnToggleSettings(WPARAM wParam, LPARAM lParam);
    afx_msg void OnHotKey(UINT id, UINT hotKeyId, UINT lParam);
    afx_msg LRESULT OnTaskbarCreated(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnQueryEndSession(WPARAM wParam, LPARAM lParam);
    afx_msg void OnTimer(UINT_PTR timerId);
    afx_msg void OnContextMenu(CWnd* parent, CPoint point);
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()

private:
    void AddTrayIcon();
    void UpdateTray(bool active);
    void ShowTrayBalloon(const CStringW& title, const CStringW& text);
    void HandleCommand(UINT id);
    bool RegisterHotkey(const CStringW& spec, CStringW& error);
    void ArmHook(bool armed);
    void OpenSettings();
    void Poll();
    void DeliverReadyText();
    static CStringW DescribeWindow(HWND window);

    Session session_;
    OverlayWindow overlay_;
    NOTIFYICONDATAW nid_{};
    bool iconAdded_ = false;
    bool hotkeyActive_ = false;
    bool active_ = false;
    int errorTicks_ = 0;
    HWND lastCaretWindow_ = nullptr;   // last non-shell foreground, for Win-key hotkeys
    Settings settings_;
};

BEGIN_MESSAGE_MAP(CHostWindow, CWnd)
    ON_MESSAGE(WM_TRAYICON, &CHostWindow::OnTray)
    ON_MESSAGE(WM_COMMAND, &CHostWindow::OnWinCommand)
    ON_MESSAGE(WM_TOGGLE_DICTATION, &CHostWindow::OnToggle)
    ON_MESSAGE(WM_TOGGLE_SETTINGS, &CHostWindow::OnToggleSettings)
    ON_WM_HOTKEY()
    ON_MESSAGE(WM_QUERYENDSESSION, &CHostWindow::OnQueryEndSession)
    ON_WM_TIMER()
    ON_WM_CONTEXTMENU()
    ON_WM_DESTROY()
END_MESSAGE_MAP()

bool CHostWindow::CreateAndInit()
{
    WNDCLASS wc = {};
    wc.hInstance = AfxGetInstanceHandle();
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.lpszClassName = L"WinWhisperHiddenHost";
    ::RegisterClassW(&wc);

    if (!CreateEx(WS_EX_TOOLWINDOW, L"WinWhisperHiddenHost", nullptr, WS_POPUP, 0, 0, 0, 0,
                  nullptr, nullptr, AfxGetInstanceHandle())) {
        util::Log(L"hidden window creation failed, err=%lu", ::GetLastError());
        return false;
    }

    if (!overlay_.Create()) {
        util::Log(L"overlay window creation failed");
        return false;
    }

    overlay_.onClose = [this] { StopDictation(L"закрыто"); };
    overlay_.onSettings = [this] {
        StopDictation(L"настройки");
        OpenSettings();
    };
    overlay_.onHelp = [this] {
        StopDictation(L"справка");
        ShowTrayBalloon(L"WinWhisper",
                        L"Горячая клавиша начинает и останавливает диктовку.\r\n"
                        L"ESC — остановить и вставить распознанное.\r\n"
                        L"Текст вставляется в окно, где стоял курсор.");
    };

    AddTrayIcon();

    CStringW error;
    if (!RegisterHotkey(settings_.hotkey, error)) {
        ShowTrayBalloon(L"WinWhisper — не удалось включить горячую клавишу", error);
        util::Log(L"hotkey failed: %s", error.GetString());
    }

    session_.SetSettings(settings_);
    SetTimer(TIMER_UI, 33, nullptr);
    return true;
}

void CHostWindow::AddTrayIcon()
{
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = GetSafeHwnd();
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = AfxGetApp()->LoadIcon(MAKEINTRESOURCE(IDI_APP));
    wcscpy_s(nid_.szTip, L"WinWhisper — готов");

    iconAdded_ = ::Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    if (!iconAdded_)
        util::Log(L"Shell_NotifyIcon(NIM_ADD) failed");
}

void CHostWindow::UpdateTray(bool listening)
{
    if (!iconAdded_)
        return;
    nid_.uFlags = NIF_ICON | NIF_TIP;
    nid_.hIcon = AfxGetApp()->LoadIcon(listening ? IDI_APP_ACTIVE : IDI_APP);
    wcscpy_s(nid_.szTip, listening ? L"WinWhisper — идёт запись" : L"WinWhisper — готов");
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void CHostWindow::ShowTrayBalloon(const CStringW& title, const CStringW& text)
{
    if (!iconAdded_)
        return;
    nid_.uFlags = NIF_INFO;
    nid_.dwInfoFlags = NIIF_INFO;
    nid_.hIcon = AfxGetApp()->LoadIcon(MAKEINTRESOURCE(IDI_APP));
    wcsncpy_s(nid_.szInfoTitle, title.GetString(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, text.GetString(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

bool CHostWindow::RegisterHotkey(const CStringW& spec, CStringW& error)
{
    ::UnregisterHotKey(GetSafeHwnd(), HOTKEY_ID);
    hotkeyActive_ = false;

    UINT mods = 0, vk = 0;
    if (!ParseHotkey(spec, mods, vk)) {
        error = L"Не удалось разобрать горячую клавишу: " + spec;
        return false;
    }
    if (!::RegisterHotKey(GetSafeHwnd(), HOTKEY_ID, mods, vk)) {
        CStringW text;
        text.Format(L"Клавиша %s занята другим приложением (код %lu).\r\n"
                    L"Смените её в настройках.",
                    HotkeyDisplayName(mods, vk).GetString(), ::GetLastError());
        error = text;
        return false;
    }
    hotkeyActive_ = true;
    g_hotkeyVk = vk;   // the hook must not treat the toggle itself as typing
    util::Log(L"hotkey: %s", HotkeyDisplayName(mods, vk).GetString());
    return true;
}

void CHostWindow::ArmHook(bool armed)
{
    if (armed == (g_keyboardHook != nullptr))
        return;

    if (armed) {
        g_keyboardHook = ::SetWindowsHookExW(WH_KEYBOARD_LL, &LowLevelKeyboardProc,
                                             AfxGetInstanceHandle(), 0);
        if (g_keyboardHook == nullptr)
            util::Log(L"WH_KEYBOARD_LL hook failed: %lu", ::GetLastError());
    } else if (g_keyboardHook != nullptr) {
        ::UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
}

void StopDictationFromHook(const wchar_t* reason)
{
    if (g_host != nullptr)
        g_host->StopDictation(reason);
}

// ------------------------------------------------------------- dictation ---

void CHostWindow::StartDictation()
{
    if (active_)
        return;

    // Where the text will land. The live foreground window is the truth, except
    // when it is the shell: a hotkey that involves the Windows key brings the
    // taskbar or Start menu forward, and "typing into the taskbar" is how text
    // used to disappear without a trace.
    HWND target = ::GetForegroundWindow();
    if (target == nullptr || target == GetSafeHwnd() ||
        (overlay_.GetSafeHwnd() != nullptr && target == overlay_.GetSafeHwnd()) ||
        textins::IsShellWindow(target)) {
        target = lastCaretWindow_;
    }
    if (target == overlay_.GetSafeHwnd() || target == GetSafeHwnd())
        target = nullptr;

    CStringW error;
    if (!session_.Start(target, &error)) {
        ShowTrayBalloon(L"WinWhisper", error);
        util::Log(L"start failed: %s", error.GetString());
        return;
    }

    OverlayModel model;
    model.state = OverlayState::Listening;
    model.status = L"Слушаю…";
    overlay_.Update(model);
    overlay_.ShowNear(target);

    active_ = true;
    errorTicks_ = 0;
    ArmHook(true);
    UpdateTray(true);
    util::Log(L"dictation started, target=0x%p (%s)", static_cast<void*>(target),
              DescribeWindow(target).GetString());
}

CStringW CHostWindow::DescribeWindow(HWND window)
{
    if (window == nullptr)
        return L"нет окна";
    wchar_t title[192] = {};
    // SendMessageTimeout, not GetWindowText: a hung app must not hang us.
    DWORD_PTR ignored = 0;
    ::SendMessageTimeoutW(window, WM_GETTEXT, ARRAYSIZE(title),
                          reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG | SMTO_BLOCK, 120,
                          &ignored);
    wchar_t cls[96] = {};
    ::GetClassNameW(window, cls, ARRAYSIZE(cls));
    CStringW text;
    text.Format(L"%s [%s]", title[0] != L'\0' ? title : L"(без заголовка)", cls);
    return text;
}

void CHostWindow::StopDictation(const wchar_t* reason)
{
    if (!active_)
        return;

    active_ = false;
    ArmHook(false);
    session_.Stop();
    UpdateTray(false);
    util::Log(L"dictation stopped (%s)", reason != nullptr ? reason : L"");
}

void CHostWindow::DeliverReadyText()
{
    CStringW text;
    CStringW error;
    bool isError = false;

    const HWND target = session_.Target();
    if (!session_.TakeReadyText(text, isError, error))
        return;

    if (isError) {
        OverlayModel model;
        model.state = OverlayState::Error;
        model.error = error;
        overlay_.Update(model);
        errorTicks_ = kErrorTicks;
        ShowTrayBalloon(L"WinWhisper — ошибка распознавания", error);
        util::Log(L"transcription failed: %s", error.GetString());
        return;
    }

    if (text.IsEmpty()) {
        // The server answers HTTP 200 with an empty text when the segment holds
        // no speech. Returning quietly here is what made a phrase look like it
        // had been swallowed, so say it out loud instead.
        OverlayModel model;
        model.state = OverlayState::Error;
        model.error = L"Не расслышал — повторите";
        overlay_.Update(model);
        errorTicks_ = kErrorTicks;
        util::Log(L"[stt] пустой ответ сервера, вставлять нечего");
        return;
    }

    InsertOptions options;
    options.appendSpace = settings_.appendSpace;
    options.newlineAsShiftEnter = settings_.newlineAsShiftEnter;
    options.clipboardFallback = settings_.clipboardFallback;

    CStringW insertError;
    const textins::Delivery delivery = textins::InsertVerbose(target, text, options, &insertError);
    switch (delivery) {
    case textins::Delivery::Typed:
        util::Log(L"inserted: %s", text.GetString());
        break;
    case textins::Delivery::Pasted:
        util::Log(L"inserted via clipboard: %s", text.GetString());
        break;
    case textins::Delivery::ClipboardOnly:
    default:
        // Never lose the words: they are on the clipboard, and the panel says so.
        ShowTrayBalloon(L"WinWhisper — текст в буфере обмена", insertError);
        OverlayModel model;
        model.state = OverlayState::Error;
        model.error = insertError;
        overlay_.Update(model);
        errorTicks_ = kErrorTicks;
        util::Log(L"insert deferred to clipboard: %s", text.GetString());
        break;
    }
}

void CHostWindow::Poll()
{
    // Remember where the caret lives while nothing is happening. A hotkey that
    // uses the Windows key brings the shell forward, so the live foreground at
    // press time is not always the window the user was typing in.
    if (!active_) {
        HWND foreground = ::GetForegroundWindow();
        if (foreground != nullptr && !textins::IsShellWindow(foreground))
            lastCaretWindow_ = foreground;
    } else {
        // Switching to another window ends the dictation, exactly as if the user
        // had pressed ESC: whatever was said so far is still transcribed and
        // typed into the window they started in.
        HWND foreground = ::GetForegroundWindow();
        const HWND target = session_.Target();
        if (foreground != nullptr && target != nullptr && foreground != target &&
            !textins::IsShellWindow(foreground)) {
            StopDictation(L"фокус перешёл в другое окно");
        }
    }

    DeliverReadyText();
    session_.FinalizeIfDone();

    const SessionSnapshot snapshot = session_.Snapshot();
    const bool overlayVisible = overlay_.IsVisible();

    if (overlayVisible && snapshot.phase != SessionPhase::Idle) {
        OverlayModel model;
        if (!snapshot.error.IsEmpty()) {
            model.state = OverlayState::Error;
            model.error = snapshot.error;
        } else {
            model.state = (snapshot.phase == SessionPhase::Processing) ? OverlayState::Processing
                                                                        : OverlayState::Listening;
            model.level = snapshot.level;
            model.speaking = snapshot.speaking;
            model.status = snapshot.status;
        }
        overlay_.Update(model);
    } else if (overlayVisible && !active_ && !snapshot.error.IsEmpty()) {
        // Capture died (no usable microphone, device unplugged): keep the panel
        // up long enough to say why instead of blinking out of existence.
        OverlayModel model;
        model.state = OverlayState::Error;
        model.error = snapshot.error;
        overlay_.Update(model);
        if (errorTicks_ == 0) {
            errorTicks_ = kErrorTicks * 2;
            ShowTrayBalloon(L"WinWhisper — микрофон", snapshot.error);
        }
    }

    if (errorTicks_ > 0) {
        if (--errorTicks_ == 0) {
            overlay_.Hide();
            errorTicks_ = 0;
        }
    } else if (overlayVisible && snapshot.phase == SessionPhase::Idle && !active_) {
        overlay_.Hide();
    }
}

// ------------------------------------------------------------- messages ----

LRESULT CHostWindow::OnTray(WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    switch (lParam) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        OnToggle(0, 0);
        break;
    case WM_RBUTTONUP: {
        POINT pt = {};
        ::GetCursorPos(&pt);
        OnContextMenu(this, CPoint(pt.x, pt.y));
        break;
    }
    default:
        break;
    }
    return 0;
}

LRESULT CHostWindow::OnToggle(WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    (void)lParam;
    if (session_.IsActive()) {
        StopDictation(L"горячая клавиша");
    } else {
        errorTicks_ = 0;
        StartDictation();
    }
    return 0;
}

LRESULT CHostWindow::OnWinCommand(WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    HandleCommand(LOWORD(wParam));
    return 0;
}

void CHostWindow::OnHotKey(UINT id, UINT hotKeyId, UINT lParam)
{
    (void)id;
    (void)hotKeyId;
    (void)lParam;
    if (session_.IsActive()) {
        StopDictation(L"горячая клавиша");
    } else {
        errorTicks_ = 0;
        StartDictation();
    }
}

LRESULT CHostWindow::OnToggleSettings(WPARAM, LPARAM)
{
    HandleCommand(ID_TRAY_SETTINGS);
    return 0;
}

void CHostWindow::HandleCommand(UINT id)
{
    switch (id) {
    case ID_TRAY_LISTEN:
        OnToggle(0, 0);
        break;
    case ID_TRAY_SETTINGS:
        StopDictation(L"настройки");
        OpenSettings();
        break;
    case ID_TRAY_EXIT:
        DestroyWindow();
        break;
    default:
        break;
    }
}

LRESULT CHostWindow::OnTaskbarCreated(WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    (void)lParam;
    // Explorer restarts after an update: the icon is gone, put it back.
    AddTrayIcon();
    UpdateTray(session_.IsActive());
    return 0;
}

LRESULT CHostWindow::OnQueryEndSession(WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    (void)lParam;
    StopDictation(L"завершение сеанса Windows");
    return TRUE;
}

void CHostWindow::OnTimer(UINT_PTR timerId)
{
    if (timerId == TIMER_UI)
        Poll();
}

void CHostWindow::OnContextMenu(CWnd* parent, CPoint point)
{
    (void)parent;
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr)
        return;

    const bool listening = session_.IsActive();
    ::AppendMenuW(menu, MF_STRING | (listening ? MF_GRAYED : 0), ID_TRAY_LISTEN,
                  listening ? L"Диктовка идёт…" : L"Начать диктовку");
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Настройки…");

    if (hotkeyActive_) {
        CStringW caption = L"Горячая клавиша: " + settings_.hotkey;
        ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, caption.GetString());
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Выход");

    const UINT cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                      point.x, point.y, 0, GetSafeHwnd(), nullptr);
    ::DestroyMenu(menu);
    if (cmd != 0)
        HandleCommand(cmd);
}

void CHostWindow::OpenSettings()
{
    // A modal dialog that fails to create is completely silent, so record what
    // the resource lookup and the call actually did.
    const HINSTANCE resources = ::AfxGetResourceHandle();
    const HRSRC templateResource =
        ::FindResourceW(resources, MAKEINTRESOURCEW(IDD_SETTINGS), RT_DIALOG);
    util::Log(L"[settings] resources=0x%p, RT_DIALOG(%u)=0x%p", static_cast<void*>(resources),
              static_cast<unsigned>(IDD_SETTINGS), static_cast<void*>(templateResource));

    CSettingsDialog dialog(settings_, this);
    const INT_PTR result = dialog.DoModal();
    util::Log(L"[settings] DoModal returned %d, lastError=%lu", static_cast<int>(result),
              ::GetLastError());
    if (result != IDOK)
        return;

    Settings next = dialog.Result();
    if (!dialog.Changed())
        return;

    const bool autoStartChanged = next.autoStart != settings_.autoStart;
    settings_ = next;
    settings_.Save();
    util::SetLogEnabled(settings_.debugLog);

    if (autoStartChanged)
        SetAutoStartEnabled(settings_.autoStart);

    session_.SetSettings(settings_);

    CStringW error;
    if (!RegisterHotkey(settings_.hotkey, error))
        ShowTrayBalloon(L"WinWhisper", error);
}

// ------------------------------------------------------------ selftest -----

// Reads a RIFF/WAVE file into 16 kHz mono PCM using the same resampler the live
// path uses, so a recorded clip exercises the real pipeline.
static bool LoadWavAs16kMono(const CStringW& path, std::vector<int16_t>* out, CStringW* error)
{
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path, L"rb") != 0 || file == nullptr) {
        *error = L"Не удалось открыть файл: " + path;
        return false;
    }
    ::fseek(file, 0, SEEK_END);
    const long size = ::ftell(file);
    ::fseek(file, 0, SEEK_SET);
    if (size <= 44) {
        ::fclose(file);
        *error = L"Файл слишком мал для WAV";
        return false;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    const size_t read = ::fread(data.data(), 1, data.size(), file);
    ::fclose(file);
    data.resize(read);

    if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        *error = L"Это не WAV-файл (нет RIFF/WAVE)";
        return false;
    }

    uint16_t channels = 1;
    uint32_t rate = 16000;
    uint16_t bits = 16;
    size_t dataOffset = 0;
    size_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const char* id = reinterpret_cast<const char*>(data.data() + pos);
        uint32_t chunkSize = 0;
        std::memcpy(&chunkSize, data.data() + pos + 4, 4);
        const size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= data.size()) {
            uint16_t tag = 0;
            std::memcpy(&tag, data.data() + body, 2);
            std::memcpy(&channels, data.data() + body + 2, 2);
            std::memcpy(&rate, data.data() + body + 4, 4);
            std::memcpy(&bits, data.data() + body + 14, 2);
            if (tag != 1 && tag != 0xFFFE) {
                *error = L"Поддерживается только несжатый PCM WAV";
                return false;
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOffset = body;
            dataSize = std::min<size_t>(chunkSize, data.size() - body);
        }
        pos = body + chunkSize + (chunkSize & 1);
    }

    if (dataOffset == 0 || dataSize == 0) {
        *error = L"В WAV нет блока data";
        return false;
    }
    if (bits != 16) {
        CStringW text;
        text.Format(L"Поддерживается только 16-битный PCM (в файле %u бит)", bits);
        *error = text;
        return false;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(data.data() + dataOffset);
    const size_t frames = dataSize / (2 * channels);

    std::vector<float> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (uint16_t c = 0; c < channels; ++c)
            sum += samples[f * channels + c] / 32768.0f;
        mono[f] = sum / static_cast<float>(channels);
    }

    out->clear();
    if (rate == 16000) {
        out->resize(frames);
        for (size_t f = 0; f < frames; ++f)
            (*out)[f] = static_cast<int16_t>(std::lround(
                std::max(-1.0f, std::min(1.0f, mono[f])) * 32767.0f));
    } else {
        Resampler resampler;
        resampler.Configure(static_cast<double>(rate), 16000.0);
        resampler.Process(mono.data(), mono.size(), *out);
    }
    return true;
}

// Feeds a recorded clip through the very same detector and HTTP client the
// microphone path uses. Handy before a first real dictation, and the only way to
// verify the chain on a machine whose speakers the microphone cannot hear.
int RunInject(const CStringW& path)
{
    util::SetLogEnabled(true);

    WCHAR tempDir[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(tempDir), tempDir);
    const CStringW reportPath = CStringW(tempDir) + L"WinWhisper-inject.txt";
    FILE* file = nullptr;
    ::_wfopen_s(&file, reportPath, L"w, ccs=UTF-8");

    auto emit = [&](const wchar_t* fmt, ...) {
        wchar_t line[2048] = {};
        va_list args;
        va_start(args, fmt);
        ::_vsnwprintf_s(line, _TRUNCATE, fmt, args);
        va_end(args);
        if (file != nullptr) {
            ::fputws(line, file);
            ::fputws(L"\r\n", file);
            ::fflush(file);
        }
    };

    Settings settings;
    settings.Load();
    emit(L"file          : %s", path.GetString());
    emit(L"server        : %s", settings.baseUrl.GetString());
    emit(L"model         : %s", settings.model.GetString());

    std::vector<int16_t> pcm;
    CStringW error;
    if (!LoadWavAs16kMono(path, &pcm, &error)) {
        emit(L"RESULT        : FAIL - %s", error.GetString());
        if (file != nullptr)
            fclose(file);
        return 1;
    }
    emit(L"audio         : %.2f s at 16 kHz mono", pcm.size() / 16000.0);

    VadConfig cfg;
    cfg.silenceMs = settings.silenceMs;
    cfg.minSpeechMs = settings.minSpeechMs;
    cfg.maxSegmentMs = settings.maxSegmentMs;
    cfg.startRatio = settings.thresholdRatio;
    cfg.stopRatio = std::max(1.1, settings.thresholdRatio * 0.55);

    Vad vad;
    vad.Configure(cfg);
    vad.SetSampleRate(16000);

    struct Segment {
        std::vector<int16_t> pcm;
    };
    std::vector<Segment> segments;
    vad.SetSegmentSink([&](std::vector<int16_t>&& segment) {
        segments.push_back(Segment{std::move(segment)});
    });

    // Same 20 ms frames the live capture hands over.
    constexpr size_t kFrame = 320;
    for (size_t offset = 0; offset < pcm.size(); offset += kFrame) {
        const size_t take = std::min(kFrame, pcm.size() - offset);
        vad.Process(pcm.data() + offset, take);
    }
    vad.Flush();

    emit(L"segments      : %d", static_cast<int>(segments.size()));
    if (segments.empty()) {
        emit(L"RESULT        : FAIL - детектор пауз не нашёл речи");
        if (file != nullptr)
            fclose(file);
        return 1;
    }

    std::string accumulated;
    int failures = 0;
    for (size_t i = 0; i < segments.size(); ++i) {
        WhisperClient injectClient;
        TranscriptRequest request;
        request.baseUrl = settings.baseUrl;
        request.apiKey = settings.apiKey;
        request.model = settings.model;
        request.language = settings.language;
        request.connectTimeoutMs = settings.connectTimeoutMs;
        request.receiveTimeoutMs = settings.receiveTimeoutMs;
        request.wav = MakeWav16kMono(segments[i].pcm);
        request.fileName = L"audio.wav";
        request.contentType = L"audio/wav";

        // Honour the configured container so --inject measures the same thing a
        // live dictation would send.
        if (audioenc::ParseFormat(settings.audioFormat) == audioenc::Format::M4a) {
            audioenc::Upload upload = audioenc::BuildUpload(
                segments[i].pcm, audioenc::Format::M4a, settings.aacBitrate);
            request.wav = std::move(upload.bytes);
            request.fileName = upload.fileName;
            request.contentType = upload.contentType;
        }

        // Same prompt assembly as the live path: configured hint first, then the
        // tail of what has already been recognised.
        CStringW prompt = settings.prompt;
        if (settings.usePromptContext && !accumulated.empty()) {
            std::string context = accumulated;
            if (context.size() > 200)
                context = context.substr(context.size() - 200);
            if (!prompt.IsEmpty())
                prompt += L" ";
            prompt += CStringW(util::Utf8ToWide(context).c_str());
        }
        if (!settings.glossary.IsEmpty()) {
            if (!prompt.IsEmpty())
                prompt += L" ";
            prompt += settings.glossary;
        }
        // Same cap as the live path: the decoder keeps the last ~224 tokens.
        if (prompt.GetLength() > 600)
            prompt = prompt.Right(600);
        request.usePrompt = !prompt.IsEmpty();
        request.prompt = prompt;
        if (request.usePrompt)
            emit(L"prompt        : %s", prompt.GetString());

        const double seconds = segments[i].pcm.size() / 16000.0;
        const TranscriptResult result = injectClient.Send(request);
        if (!result.ok) {
            emit(L"[%d] %.2f s -> ОШИБКА: %s", static_cast<int>(i), seconds,
                 result.error.GetString());
            ++failures;
            continue;
        }
        CStringW text = result.text;
        text.Trim();
        CStringW invented;
        if (LooksInvented(text, seconds, result, &invented))
            emit(L"      ОТБРОШЕНО как выдумка модели: %s", invented.GetString());
        // Printing the confidence makes it visible whether the server reports it
        // at all: without it only the physics half of the guard can work.
        if (result.hasAvgLogprob)
            emit(L"[%d] %.2f s, avg_logprob %.2f -> %s", static_cast<int>(i), seconds,
                 result.avgLogprob, text.GetString());
        else
            emit(L"[%d] %.2f s -> %s", static_cast<int>(i), seconds, text.GetString());
        accumulated += util::WideToUtf8(text) + " ";
    }

    emit(L"report        : %s", reportPath.GetString());
    emit(L"RESULT        : %s", failures == 0 ? L"PASS" : L"FAIL");
    if (file != nullptr)
        fclose(file);
    return failures == 0 ? 0 : 1;
}

// A hidden command line switch that exercises the whole chain headlessly:
// settings -> DPAPI -> endpoint -> endpointing. Handy on machines without a
// microphone, and it keeps the pipeline verifiable in CI.
int RunSelfTest()
{
    util::SetLogEnabled(true);

    // Report goes to a file first: the app may be launched with no console at
    // all, and writing to a detached stdout is not worth the risk.
    WCHAR tempDir[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(tempDir), tempDir);
    const CStringW reportPath = CStringW(tempDir) + L"WinWhisper-selftest.txt";

    FILE* file = nullptr;
    ::_wfopen_s(&file, reportPath, L"w, ccs=UTF-8");
    ::fputws(L"WinWhisper self test\r\n", file);
    ::fflush(file);

    auto emit = [&](const wchar_t* fmt, ...) {
        wchar_t line[1024] = {};
        va_list args;
        va_start(args, fmt);
        ::_vsnwprintf_s(line, _TRUNCATE, fmt, args);
        va_end(args);
        if (file != nullptr) {
            ::fputws(line, file);
            ::fputws(L"\r\n", file);
            ::fflush(file);
        }
    };

    int failures = 0;

    Settings settings;
    settings.Load();
    emit(L"settings file : %s", settings.ConfigPath().GetString());
    emit(L"base url      : %s", settings.baseUrl.GetString());
    emit(L"model         : %s", settings.model.GetString());
    emit(L"hotkey        : %s", settings.hotkey.GetString());
    emit(L"api key       : %s (%d chars)",
         settings.apiKey.IsEmpty() ? L"NOT SET" : L"decrypted", settings.apiKey.GetLength());

    // --- endpointing: 1 s tone, 1.2 s silence, 0.8 s tone, 1 s silence -------
    {
        const int rate = 16000;
        VadConfig cfg;
        cfg.silenceMs = 700;
        cfg.minSpeechMs = 250;
        cfg.maxSegmentMs = 20000;
        cfg.startRatio = 3.0;
        cfg.stopRatio = 1.6;
        cfg.absFloorRms = 0.004;
        cfg.prerollMs = 300;

        Vad vad;
        vad.Configure(cfg);
        vad.SetSampleRate(rate);

        std::vector<int16_t> captured;
        vad.SetSegmentSink([&](std::vector<int16_t>&& segment) {
            if (!captured.empty()) {
                captured.push_back('#');           // separator between segments
            }
            captured.insert(captured.end(), segment.begin(), segment.end());
        });

        std::vector<int16_t> pcm;
        auto addTone = [&](double seconds) {
            const size_t n = static_cast<size_t>(seconds * rate);
            for (size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / rate;
                pcm.push_back(static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * 220.0 * t)));
            }
        };
        auto addSilence = [&](double seconds) {
            const size_t n = static_cast<size_t>(seconds * rate);
            for (size_t i = 0; i < n; ++i)
                pcm.push_back(0);
        };

        addSilence(0.5);
        addTone(1.0);
        addSilence(1.2);
        addTone(0.8);
        addSilence(1.0);

        vad.Process(pcm.data(), pcm.size());
        vad.Flush();

        size_t segments = 1;
        size_t total = 0;
        for (int16_t sample : captured) {
            if (sample == '#')
                ++segments;
            else
                ++total;
        }
        const double seconds = total / static_cast<double>(rate);
        emit(L"vad           : %d segment(s), %.2f s of speech (expected 2, ~1.5-2.6 s)",
             static_cast<int>(segments), seconds);
        if (segments != 2 || seconds < 1.5 || seconds > 2.6)
            ++failures;
    }

    // --- endpoint ---------------------------------------------------------
    // In CI there is no key and no reachable server, so the network half can be
    // skipped: everything else still gets verified.
    if (::wcsstr(::GetCommandLineW(), L"--offline") != nullptr) {
        emit(L"endpoint      : пропущено (--offline)");
    } else {
        int status = 0;
        CStringW error;
        const bool ok = WhisperClient::Probe(settings.baseUrl, settings.apiKey, settings.model,
                                             15000, &error, &status);
        emit(L"endpoint      : %s (HTTP %d) %s", ok ? L"OK" : L"FAILED", status,
             error.GetString());
        if (!ok)
            ++failures;

        // The same call the settings dialog makes to fill its model list.
        std::vector<CStringW> models;
        CStringW listError;
        if (WhisperClient::ListModels(settings.baseUrl, settings.apiKey, 10000, &models,
                                      &listError)) {
            CStringW joined;
            for (size_t i = 0; i < models.size(); ++i) {
                if (i > 0)
                    joined += L", ";
                joined += models[i];
            }
            emit(L"models        : %d — %s", static_cast<int>(models.size()), joined.GetString());
        } else {
            emit(L"models        : FAILED %s", listError.GetString());
        }
    }

    emit(L"report        : %s", reportPath.GetString());
    emit(L"RESULT        : %s", failures == 0 ? L"PASS" : L"FAIL");

    if (file != nullptr)
        fclose(file);
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------ lifecycle ----

void CHostWindow::OnDestroy()
{
    KillTimer(TIMER_UI);
    StopDictation(L"выход");
    ArmHook(false);
    overlay_.Hide();
    if (hotkeyActive_) {
        ::UnregisterHotKey(GetSafeHwnd(), HOTKEY_ID);
        hotkeyActive_ = false;
    }
    if (iconAdded_) {
        ::Shell_NotifyIconW(NIM_DELETE, &nid_);
        iconAdded_ = false;
    }
    ::PostQuitMessage(0);
    CWnd::OnDestroy();
}

BEGIN_MESSAGE_MAP(CWinWhisperApp, CWinApp)
END_MESSAGE_MAP()

BOOL CWinWhisperApp::InitInstance()
{
    CWinApp::InitInstance();

    // Command-line modes finish without ever entering the message loop.
    if (::GetCommandLineW() != nullptr && ::wcsstr(::GetCommandLineW(), L"--selftest") != nullptr)
        ::ExitProcess(static_cast<UINT>(RunSelfTest()));

    if (const wchar_t* inject = ::wcsstr(::GetCommandLineW(), L"--inject")) {
        CStringW path(inject + 8);
        path.Trim();
        if (path.GetLength() >= 2 && path[0] == L'"' && path[path.GetLength() - 1] == L'"')
            path = path.Mid(1, path.GetLength() - 2);
        ::ExitProcess(static_cast<UINT>(RunInject(path)));
    }

    // Renders the panel to a PNG. Lets the look be checked (and screenshotted)
    // without opening the panel on somebody's desktop.
    if (const wchar_t* shot = ::wcsstr(::GetCommandLineW(), L"--panel-shot")) {
        CStringW path(shot + 12);
        // Stop at the next switch: a path may contain spaces, but it never
        // contains " --".
        const int nextSwitch = path.Find(L" --");
        if (nextSwitch > 0)
            path = path.Left(nextSwitch);
        path.Trim();
        if (path.GetLength() >= 2 && path[0] == L'"' && path[path.GetLength() - 1] == L'"')
            path = path.Mid(1, path.GetLength() - 2);
        const int dpi = (::wcsstr(::GetCommandLineW(), L"--dpi=144") != nullptr) ? 144 : 96;
        CStringW error;
        const bool ok = overlay::RenderPanelToPng(path, dpi, &error);
        util::Log(L"[panel-shot] %s -> %s %s", ok ? L"ok" : L"failed", path.GetString(),
                  error.GetString());
        ::ExitProcess(ok ? 0 : 1);
    }

    // Only one copy may own the global hotkey.
    HWND existing = ::FindWindowW(L"WinWhisperHiddenHost", nullptr);
    if (existing != nullptr) {
        // A second launch just pokes the running copy. WM_COMMAND cannot cross a
        // process boundary for messages below WM_USER, so the request travels as
        // a WM_APP message instead.
        if (::wcsstr(::GetCommandLineW(), L"--settings") != nullptr)
            ::PostMessageW(existing, WM_TOGGLE_SETTINGS, 0, 0);
        else
            ::PostMessageW(existing, WM_TOGGLE_DICTATION, 0, 0);
        return FALSE;
    }

    auto* host = new CHostWindow();
    g_host = host;

    Settings settings;
    settings.Load();
    util::SetLogEnabled(settings.debugLog);
    util::Log(L"WinWhisper 1.1.1 start");
    util::InstallCrashHandler();

    host->ApplyInitialSettings(settings);

    if (!host->CreateAndInit()) {
        util::Log(L"initialisation failed");
        return FALSE;
    }

    m_pMainWnd = host;
    return TRUE;
}

int CWinWhisperApp::ExitInstance()
{
    util::Log(L"WinWhisper exit");
    return CWinApp::ExitInstance();
}

// MFC's entry point looks the application object up through AfxGetApp(); without
// this global the CRT reaches AfxWinMain with a null pointer.
CWinWhisperApp theApp;