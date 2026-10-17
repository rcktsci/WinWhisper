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
constexpr UINT ID_TRAY_AUTOSTART = 40004;
constexpr int HOTKEY_ID = 1;
constexpr UINT_PTR TIMER_UI = 1;

// The version in one place for the log; the resource script and the manifest
// carry the same number for the file properties Windows shows.
constexpr wchar_t kVersion[] = L"1.1.9";

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
    void ApplyInitialSettings(const Settings& settings)
    {
        settings_ = settings;
        // Restore the panel's home before it is created for the first time.
        overlay_.SetStickyPosition(settings_.panelPlaced, settings_.panelX, settings_.panelY);
    }
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
    afx_msg LRESULT OnEndSession(WPARAM wParam, LPARAM lParam);
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
    void ToggleAutoStart();
    void TouchAlive(const wchar_t* state);
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
    ULONGLONG startedTick_ = 0;
    ULONGLONG aliveTick_ = 0;
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
    ON_MESSAGE(WM_ENDSESSION, &CHostWindow::OnEndSession)
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
    overlay_.onMoved = [this](int x, int y) {
        // The panel reports where it landed: the first time it ever appears and
        // every time it is dragged. Remembered so the next dictation does not fly
        // back to the caret and cover the field.
        if (settings_.panelPlaced && settings_.panelX == x && settings_.panelY == y)
            return;
        settings_.panelPlaced = true;
        settings_.panelX = x;
        settings_.panelY = y;
        settings_.Save();
        util::Log(L"[panel] положение запомнено: %d,%d", x, y);
    };

    AddTrayIcon();

    CStringW error;
    if (!RegisterHotkey(settings_.hotkey, error)) {
        ShowTrayBalloon(L"WinWhisper — не удалось включить горячую клавишу", error);
        util::Log(L"hotkey failed: %s", error.GetString());
    }

    session_.SetSettings(settings_);
    SetTimer(TIMER_UI, 33, nullptr);

    startedTick_ = ::GetTickCount64();
    aliveTick_ = startedTick_;
    TouchAlive(L"запуск");
    util::Log(L"[alive] маркер жизни: %s", (util::EnsureAppDataDir() + L"\\alive.txt").GetString());
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
        model.error = L"Не расслышал";
        overlay_.Update(model);
        errorTicks_ = kErrorTicks;
        util::Log(L"[stt] пустой ответ сервера, вставлять нечего");
        return;
    }

    InsertOptions options;
    options.appendSpace = settings_.appendSpace;
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
    // Refresh the alive marker about once a minute. If the process is ever killed
    // outright - which leaves no dump and no event - the file's timestamp is the
    // only evidence of when it happened.
    const ULONGLONG now = ::GetTickCount64();
    if (now - aliveTick_ >= 60000) {
        aliveTick_ = now;
        TouchAlive(active_ ? L"диктовка" : L"ожидание");
    }

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
    case ID_TRAY_AUTOSTART:
        ToggleAutoStart();
        break;
    case ID_TRAY_EXIT:
        DestroyWindow();
        break;
    default:
        break;
    }
}

// The label states what the registry says, because a tick alone would lie for the
// two half-enabled cases: an entry switched off in Task Manager, and one left
// behind by a copy in another folder.
CStringW AutoStartMenuLabel(const AutoStartStatus& status)
{
    const bool on = status.registered && status.approved;
    if (on && status.pointsHere)
        return L"Автозапуск при входе: включён";
    if (on)
        return L"Автозапуск при входе: включён для другой копии";
    if (status.registered)
        return L"Автозапуск при входе: выключен в Диспетчере задач";
    return L"Автозапуск при входе: выключен";
}

void CHostWindow::ToggleAutoStart()
{
    const AutoStartStatus before = GetAutoStartStatus();
    // Only a fully working entry is switched off. Anything else (missing, blocked
    // by Task Manager, pointing at another copy) is repaired to "on, here", which
    // is also how a stale path after moving the exe gets fixed.
    const bool enable = !(before.registered && before.approved && before.pointsHere);

    const bool ok = SetAutoStartEnabled(enable);
    settings_.autoStart = enable;
    settings_.Save();

    const AutoStartStatus after = GetAutoStartStatus();
    const bool live = after.registered && after.approved;
    util::Log(L"[autostart] tray toggle: requested %s, registry now registered=%d approved=%d "
              L"pointsHere=%d",
              enable ? L"on" : L"off", after.registered ? 1 : 0, after.approved ? 1 : 0,
              after.pointsHere ? 1 : 0);

    if (!ok || enable != live) {
        ShowTrayBalloon(L"WinWhisper — автозапуск",
                        L"Не удалось изменить запись автозапуска.\r\n"
                        L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        return;
    }
    if (after.registered) {
        ShowTrayBalloon(L"WinWhisper — автозапуск",
                        CStringW(L"Включён: приложение запускается при входе в Windows.\r\n") +
                            after.command);
    } else {
        ShowTrayBalloon(L"WinWhisper — автозапуск",
                        L"Выключен: приложение больше не запускается само.");
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

LRESULT CHostWindow::OnEndSession(WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    // Windows is really ending the session this time (logoff or shutdown). Say so
    // in the log: otherwise a process that goes away with the session looks
    // exactly like one that was killed behind the user's back.
    if (wParam != FALSE) {
        util::Log(L"[exit] сеанс Windows завершается, приложение закрывается");
        TouchAlive(L"завершение сеанса Windows");
    }
    return 0;
}

void CHostWindow::TouchAlive(const wchar_t* state)
{
    const ULONGLONG now = ::GetTickCount64();
    const unsigned long long seconds = (now - startedTick_) / 1000;
    CStringW text;
    text.Format(L"pid=%lu up=%lluс hotkey=%s диктовка=%d — %s",
                static_cast<unsigned long>(::GetCurrentProcessId()), seconds,
                hotkeyActive_ ? L"да" : L"нет", active_ ? 1 : 0, state);
    util::WriteAliveMarker(text);
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

    const AutoStartStatus autostart = GetAutoStartStatus();
    ::AppendMenuW(menu, MF_STRING | (autostart.registered && autostart.approved ? MF_CHECKED : 0),
                  ID_TRAY_AUTOSTART, AutoStartMenuLabel(autostart));

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

    // The checkbox must show what Windows will really do at logon, not what the
    // INI remembers: Task Manager can switch the entry off behind our back.
    // Without this the dialog would silently resurrect a disabled entry.
    const AutoStartStatus autostart = GetAutoStartStatus();
    settings_.autoStart = autostart.registered && autostart.approved;
    util::Log(L"[settings] автозапуск: registered=%d approved=%d pointsHere=%d cmd=%s",
              autostart.registered ? 1 : 0, autostart.approved ? 1 : 0, autostart.pointsHere ? 1 : 0,
              autostart.command.GetString());

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

// Reads the value that follows a switch. A path may contain spaces, but it never
// contains " --", which is where the next switch starts.
CStringW PathAfterSwitch(const wchar_t* value)
{
    CStringW path(value);
    const int nextSwitch = path.Find(L" --");
    if (nextSwitch > 0)
        path = path.Left(nextSwitch);
    path.Trim();
    if (path.GetLength() >= 2 && path[0] == L'"' && path[path.GetLength() - 1] == L'"')
        path = path.Mid(1, path.GetLength() - 2);
    return path;
}

// Renders the settings dialog into a PNG without putting it on the desktop.
// Dialog layout is easy to get wrong and looking at the result is the only
// honest check, so this mirrors --panel-shot for the panel.
// `neutral` fills the dialog with example values instead of reading the INI: a
// screenshot for the README must not publish somebody's gateway address or key.
int RunSettingsShot(const CStringW& path, bool neutral)
{
    util::SetLogEnabled(true);

    Settings settings;
    if (neutral) {
        settings.baseUrl = L"https://api.openai.com/v1";
        settings.apiKey = L"sk-demo";
        settings.model = L"whisper-1";
        settings.prompt = L"Русский текст, обычная пунктуация, короткие предложения.";
        settings.glossary = L"Grafana, Prometheus, Kubernetes";
        settings.autoStart = true;
    } else {
        settings.Load();
    }

    // Render at 100% whatever this machine's display scaling is. The dialog is
    // laid out in dialog units, so a 125% desktop would otherwise produce an
    // image 25% larger than the dialog really is - and the README has no business
    // showing an enlarged screenshot.
    ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);

    CSettingsDialog dialog(settings, nullptr);
    if (!dialog.Create(IDD_SETTINGS, nullptr)) {
        util::Log(L"[settings-shot] Create failed, err=%lu", ::GetLastError());
        return 1;
    }
    // Create shows the window; park it outside every monitor so nothing flashes
    // on the desktop of whoever is working.
    dialog.SetWindowPos(nullptr, -32000, -32000, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    CRect rect;
    dialog.GetWindowRect(&rect);
    const int width = rect.Width();
    const int height = rect.Height();
    if (width <= 0 || height <= 0) {
        util::Log(L"[settings-shot] пустой размер диалога");
        dialog.DestroyWindow();
        return 1;
    }

    CClientDC screen(nullptr);
    CDC mem;
    mem.CreateCompatibleDC(&screen);
    CBitmap bitmap;
    bitmap.CreateCompatibleBitmap(&screen, width, height);
    CBitmap* old = mem.SelectObject(&bitmap);

    // PRF_CHILDREN is the point of the exercise: every control paints itself, so
    // a missing or overlapping one is visible in the image.
    ::SendMessageW(dialog.GetSafeHwnd(), WM_PRINT, reinterpret_cast<WPARAM>(mem.GetSafeHdc()),
                   PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    mem.SelectObject(old);

    CStringW error;
    const bool ok = overlay::SaveBitmapToPng(static_cast<HBITMAP>(bitmap.GetSafeHandle()), path,
                                             &error);
    // The DPI belongs in the log: the same dialog template comes out a different
    // size on a different monitor scale, and that is the first thing to check
    // when a screenshot does not match an earlier one.
    util::Log(L"[settings-shot] %s -> %s %dx%d при %u dpi%s%s", ok ? L"ok" : L"failed",
              path.GetString(), width, height, ::GetDpiForWindow(dialog.GetSafeHwnd()),
              neutral ? L" (нейтральные значения)" : L"", error.GetString());

    bitmap.DeleteObject();
    mem.DeleteDC();
    dialog.DestroyWindow();
    return ok ? 0 : 1;
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

    // --- start-up entry ------------------------------------------------------
    // Read-only on purpose: a self test must not change what the machine does at
    // the next logon. The write path is checked through --autostart=on|off.
    {
        const AutoStartStatus autostart = GetAutoStartStatus();
        const bool live = autostart.registered && autostart.approved;
        CStringW state = live ? L"включён" : L"выключен";
        if (autostart.registered && !autostart.approved)
            state = L"выключен в Диспетчере задач";
        else if (live && !autostart.pointsHere)
            state = L"включён для другой копии";
        emit(L"autostart     : %s%s%s", state.GetString(),
             autostart.command.IsEmpty() ? L"" : L" — ", autostart.command.GetString());
        if (settings.autoStart != live)
            emit(L"                (в INI записано %s)", settings.autoStart ? L"вкл" : L"выкл");
    }

    // --- panel position ------------------------------------------------------
    // The panel must come back exactly where it was left, and a spot that no
    // longer exists (monitor unplugged, resolution changed) must not hide it.
    // Checked through the same code that places it on screen, without showing
    // anything.
    {
        OverlayWindow preview;
        const CSize size = preview.PanelSizeForDpi(96);
        CStringW saved = L"ещё не запоминалось";
        if (settings.panelPlaced)
            saved.Format(L"%d,%d", settings.panelX, settings.panelY);
        emit(L"panel         : в INI %s", saved.GetString());

        preview.SetStickyPosition(true, 100, 200);
        const RECT exact = preview.PreviewRect();
        const bool honoured = exact.left == 100 && exact.top == 200;
        emit(L"panel         : сохранённое 100,200 -> %ld,%ld %s", exact.left, exact.top,
             honoured ? L"OK" : L"FAIL");
        if (!honoured)
            ++failures;

        preview.SetStickyPosition(true, 50000, 50000);
        const RECT clamped = preview.PreviewRect();
        const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const bool inside = clamped.left >= vx && clamped.top >= vy &&
                            clamped.right <= vx + ::GetSystemMetrics(SM_CXVIRTUALSCREEN) &&
                            clamped.bottom <= vy + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        emit(L"panel         : потерянное 50000,50000 -> %ld,%ld, размер %dx%d — %s", clamped.left,
             clamped.top, size.cx, size.cy, inside ? L"осталось на экране" : L"FAIL");
        if (!inside)
            ++failures;
    }

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
    util::Log(L"[exit] приложение закрывается штатно (DestroyWindow)");
    TouchAlive(L"штатный выход");
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
        const CStringW path = PathAfterSwitch(shot + 12);
        util::SetLogEnabled(true);
        // Any scale, not just one hard-coded value: the README wants a true 2x
        // asset (192 dpi) next to the 1x one.
        int dpi = 96;
        if (const wchar_t* dpiSwitch = ::wcsstr(::GetCommandLineW(), L"--dpi=")) {
            dpi = util::ParseInt(CStringW(dpiSwitch + 6), 96);
            if (dpi < 72 || dpi > 384)
                dpi = 96;
        }
        CStringW error;
        const bool ok = overlay::RenderPanelToPng(path, dpi, &error);
        util::Log(L"[panel-shot] %s -> %s при %d dpi %s", ok ? L"ok" : L"failed", path.GetString(),
                  dpi, error.GetString());
        ::ExitProcess(ok ? 0 : 1);
    }

    // The same idea for the settings dialog: it is checked without opening it on
    // somebody's screen, and the PNG can be shown to whoever asked for the change.
    if (const wchar_t* shot = ::wcsstr(::GetCommandLineW(), L"--settings-shot")) {
        const CStringW path = PathAfterSwitch(shot + 15);
        const bool neutral = ::wcsstr(::GetCommandLineW(), L"--neutral") != nullptr;
        ::ExitProcess(static_cast<UINT>(RunSettingsShot(path, neutral)));
    }

    // Windows launches the Run entry as: "C:\...\WinWhisper.exe" --autostart
    // The bare switch means "start quietly at logon". The =on/=off forms drive
    // exactly the code the tray toggle uses, which makes the feature scriptable -
    // and testable without clicking a tray menu.
    const wchar_t* const cmdLine = ::GetCommandLineW();
    if (cmdLine != nullptr && ::wcsstr(cmdLine, L"--autostart=") != nullptr) {
        const bool enable = ::wcsstr(cmdLine, L"--autostart=on") != nullptr;
        util::SetLogEnabled(true);
        util::Log(L"WinWhisper %s autostart-cli: %s", kVersion, enable ? L"on" : L"off");
        const bool ok = SetAutoStartEnabled(enable);
        const AutoStartStatus status = GetAutoStartStatus();
        const bool live = status.registered && status.approved;
        if (enable == live) {
            ::wprintf(L"autostart: %s\r\ncommand  : %s\r\n", enable ? L"on" : L"off",
                      status.command.GetString());
        } else {
            ::wprintf(L"autostart: FAILED (requested %s, registered=%d approved=%d)\r\n",
                      enable ? L"on" : L"off", status.registered ? 1 : 0, status.approved ? 1 : 0);
        }
        // ExitProcess does not flush stdio: without this a redirected stdout is
        // empty, which looks exactly like the command having done nothing.
        ::fflush(stdout);
        ::ExitProcess(ok && enable == live ? 0 : 1);
    }

    // Sets or clears the remembered panel position: "300,400" pins it, "reset"
    // goes back to opening next to the caret. Also the only way to check the write
    // path without dragging a window with the mouse.
    if (cmdLine != nullptr && ::wcsstr(cmdLine, L"--panel-place") != nullptr) {
        util::SetLogEnabled(true);
        Settings settings;
        settings.Load();
        const wchar_t* valueStart = ::wcsstr(cmdLine, L"--panel-place") + 13;
        if (valueStart[0] == L'=')
            ++valueStart;
        const CStringW value = PathAfterSwitch(valueStart);
        bool ok = true;
        if (value.CompareNoCase(L"reset") == 0) {
            settings.panelPlaced = false;
            util::Log(L"[panel] положение сброшено по --panel-place=reset");
        } else {
            const int comma = value.Find(L',');
            const int x = comma > 0 ? util::ParseInt(value.Left(comma), -999999) : -999999;
            const int y = comma > 0 ? util::ParseInt(value.Mid(comma + 1), -999999) : -999999;
            if (x == -999999 || y == -999999) {
                ok = false;
            } else {
                settings.panelPlaced = true;
                settings.panelX = x;
                settings.panelY = y;
                util::Log(L"[panel] положение задано вручную: %d,%d", x, y);
            }
        }
        if (ok) {
            settings.Save();
            ::wprintf(L"panel: %s %d,%d\r\n", settings.panelPlaced ? L"placed" : L"reset",
                      settings.panelX, settings.panelY);
        } else {
            // ASCII on purpose: a redirected stdout is a byte stream, so a
            // Cyrillic message would arrive as mojibake.
            ::wprintf(L"panel: FAILED (usage: --panel-place=X,Y or --panel-place=reset)\r\n");
        }
        ::fflush(stdout);
        ::ExitProcess(ok ? 0 : 2);
    }

    // Only one copy may own the global hotkey.
    HWND existing = ::FindWindowW(L"WinWhisperHiddenHost", nullptr);
    if (existing != nullptr) {
        // A second launch just pokes the running copy. WM_COMMAND cannot cross a
        // process boundary for messages below WM_USER, so the request travels as
        // a WM_APP message instead.
        if (cmdLine != nullptr && ::wcsstr(cmdLine, L"--settings") != nullptr)
            ::PostMessageW(existing, WM_TOGGLE_SETTINGS, 0, 0);
        else if (cmdLine != nullptr && ::wcsstr(cmdLine, L"--autostart") != nullptr)
            (void)0;   // already running: a logon launch has nothing left to do
        else
            ::PostMessageW(existing, WM_TOGGLE_DICTATION, 0, 0);
        // Say it out loud: otherwise this short-lived process leaves a bare
        // "WinWhisper exit" in the log with no start line above it, which looks
        // exactly like the running copy having died on its own.
        util::SetLogEnabled(true);
        util::Log(L"[exit] другая копия уже работает — эта выходит");
        return FALSE;
    }

    auto* host = new CHostWindow();
    g_host = host;
    Settings settings;
    settings.Load();
    util::SetLogEnabled(settings.debugLog);
    const bool launchedAtLogon = cmdLine != nullptr && ::wcsstr(cmdLine, L"--autostart") != nullptr;
    util::Log(L"WinWhisper %s start (pid=%lu)%s", kVersion,
              static_cast<unsigned long>(::GetCurrentProcessId()),
              launchedAtLogon ? L" (автозапуск при входе)" : L"");
    util::InstallCrashHandler();

    // Ask Windows to bring the app back if it is closed for an update or fails
    // with a crash. A manual kill (TerminateProcess) is not covered - that is
    // what the alive marker is for. RESTART_NO_REBOOT: logon start-up already
    // covers reboots, no need to be relaunched twice.
    const DWORD restartStatus = ::RegisterApplicationRestart(L"--autostart", RESTART_NO_REBOOT);
    util::Log(L"[alive] RegisterApplicationRestart -> %lu", restartStatus);

    // The registry decides what Windows runs; the INI field only mirrors it, so
    // the two cannot drift apart after a change made in Task Manager.
    const AutoStartStatus autostart = GetAutoStartStatus();
    util::Log(L"[autostart] registered=%d approved=%d pointsHere=%d cmd=%s",
              autostart.registered ? 1 : 0, autostart.approved ? 1 : 0, autostart.pointsHere ? 1 : 0,
              autostart.command.GetString());
    const bool autostartLive = autostart.registered && autostart.approved;
    if (settings.autoStart != autostartLive) {
        settings.autoStart = autostartLive;
        settings.Save();
    }

    util::Log(L"[panel] запомненное положение: placed=%d %d,%d", settings.panelPlaced ? 1 : 0,
              settings.panelX, settings.panelY);

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