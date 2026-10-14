#include "text_inserter.h"

#include <atomic>
#include <cstring>

#include "util.h"

namespace {

// Set while we are typing, so the application's keyboard hook can tell our
// injected characters from the user's own.
std::atomic<bool> g_typing{false};

// RAII guard: the flag must be cleared even if typing bails out early.
class TypingGuard {
public:
    TypingGuard() { g_typing.store(true); }
    ~TypingGuard() { g_typing.store(false); }
    TypingGuard(const TypingGuard&) = delete;
    TypingGuard& operator=(const TypingGuard&) = delete;
};

// Bringing another process' window to the foreground is subject to the
// "foreground lock". Attaching to the foreground thread and nudging the input
// state is the documented way round it.
bool ActivateWindow(HWND hwnd)
{
    if (hwnd == nullptr || !::IsWindow(hwnd))
        return false;
    if (::GetForegroundWindow() == hwnd)
        return true;

    const DWORD targetThread = ::GetWindowThreadProcessId(hwnd, nullptr);
    const DWORD currentThread = ::GetCurrentThreadId();
    HWND foreground = ::GetForegroundWindow();
    const DWORD foregroundThread = foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;

    bool attachedForeground = false;
    if (foregroundThread != 0 && foregroundThread != currentThread)
        attachedForeground = ::AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    bool attachedTarget = false;
    if (targetThread != 0 && targetThread != currentThread)
        attachedTarget = ::AttachThreadInput(currentThread, targetThread, TRUE) != FALSE;

    ::BringWindowToTop(hwnd);
    ::SetActiveWindow(hwnd);
    ::SetForegroundWindow(hwnd);
    ::SetFocus(hwnd);

    if (attachedTarget)
        ::AttachThreadInput(currentThread, targetThread, FALSE);
    if (attachedForeground)
        ::AttachThreadInput(currentThread, foregroundThread, FALSE);

    return ::GetForegroundWindow() == hwnd;
}

INPUT MakeVkInput(WORD vk, bool keyUp)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = 0;
    in.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    in.ki.time = 0;
    in.ki.dwExtraInfo = 0;
    return in;
}

INPUT MakeUnicodeInput(wchar_t ch, bool keyUp)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = ch;                       // surrogate pairs pass through untouched
    in.ki.dwFlags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0);
    in.ki.time = 0;
    in.ki.dwExtraInfo = 0;
    return in;
}

bool SendKey(WORD vk, bool withShift)
{
    INPUT inputs[4];
    UINT count = 0;
    if (withShift)
        inputs[count++] = MakeVkInput(VK_SHIFT, false);
    inputs[count++] = MakeVkInput(vk, false);
    inputs[count++] = MakeVkInput(vk, true);
    if (withShift)
        inputs[count++] = MakeVkInput(VK_SHIFT, true);

    return ::SendInput(count, inputs, sizeof(INPUT)) == count;
}

bool SendChar(wchar_t ch)
{
    INPUT inputs[2] = {MakeUnicodeInput(ch, false), MakeUnicodeInput(ch, true)};
    return ::SendInput(2, inputs, sizeof(INPUT)) == 2;
}

// Returns false as soon as Windows refuses a keystroke (UIPI: the target runs
// elevated), so the caller can fall back to the clipboard.
bool TypeUnicode(const CStringW& text, const InsertOptions& options)
{
    TypingGuard guard;
    for (int i = 0; i < text.GetLength(); ++i) {
        const wchar_t ch = text[i];
        bool ok;
        if (ch == L'\r') {
            continue;
        } else if (ch == L'\n') {
            ok = SendKey(VK_RETURN, options.newlineAsShiftEnter);
        } else if (ch == L'\t') {
            ok = SendKey(VK_TAB, false);
        } else {
            ok = SendChar(ch);
        }
        if (!ok)
            return false;
    }
    return true;
}

void SetClipboardText(const CStringW& text)
{
    ::EmptyClipboard();
    if (text.IsEmpty())
        return;
    const SIZE_T bytes = (text.GetLength() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr)
        return;
    void* p = ::GlobalLock(mem);
    if (p != nullptr) {
        memcpy(p, text.GetString(), bytes);
        ::GlobalUnlock(mem);
        if (::SetClipboardData(CF_UNICODETEXT, mem) == nullptr)
            ::GlobalFree(mem);
    }
}

CStringW TakeClipboardText()
{
    CStringW text;
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    if (h != nullptr) {
        const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h));
        if (p != nullptr) {
            text = p;
            ::GlobalUnlock(h);
        }
    }
    return text;
}

// Fallback path: UIPI blocks SendInput into elevated windows, the clipboard
// does not. Restores whatever the user had copied before.
bool PasteViaClipboard(HWND target, const CStringW& text, CStringW* error)
{
    CStringW saved;
    bool hadText = false;

    if (::OpenClipboard(target ? target : nullptr)) {
        hadText = ::IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
        if (hadText)
            saved = TakeClipboardText();
        SetClipboardText(text);
        ::CloseClipboard();
    } else {
        if (error != nullptr)
            *error = L"Не удалось открыть буфер обмена";
        return false;
    }

    if (::GetForegroundWindow() != target)
        ActivateWindow(target);
    SendKey('V', true);

    // Give the target app a moment to consume the paste before restoring.
    ::Sleep(120);
    if (::OpenClipboard(nullptr)) {
        SetClipboardText(hadText ? saved : CStringW());
        ::CloseClipboard();
    }
    return true;
}

}  // namespace

namespace textins {

bool IsShellWindow(HWND window)
{
    if (window == nullptr)
        return true;
    wchar_t cls[128] = {};
    if (::GetClassNameW(window, cls, ARRAYSIZE(cls)) == 0)
        return true;

    static const wchar_t* kShellClasses[] = {
        L"Shell_TrayWnd",              // taskbar
        L"Shell_SecondaryTrayWnd",     // taskbar on other monitors
        L"Windows.UI.Core.CoreWindow", // Start menu, search, UWP flyouts
        L"XamlExplorerHostIslandWindow",
        L"SearchHost",
        L"Progman",                    // desktop
        L"WorkerW",
        L"SysShadow",
        L"WinWhisperOverlayWindow",
        L"WinWhisperHiddenHost",
    };
    for (const wchar_t* known : kShellClasses) {
        if (::_wcsicmp(cls, known) == 0)
            return true;
    }
    return false;
}

bool IsTyping()
{
    return g_typing.load();
}

Delivery InsertVerbose(HWND target, const CStringW& rawText, const InsertOptions& options,
                       CStringW* error)
{
    if (error != nullptr)
        error->Empty();

    CStringW text = rawText;
    text.Trim();
    if (text.IsEmpty())
        return Delivery::Typed;

    // Typing blind is worse than not typing: pick a target that can actually
    // take the keystrokes.
    HWND resolved = target;
    if (resolved == nullptr || !::IsWindow(resolved) || IsShellWindow(resolved)) {
        HWND foreground = ::GetForegroundWindow();
        if (foreground != nullptr && !IsShellWindow(foreground))
            resolved = foreground;
        else
            resolved = nullptr;
    }

    // Bringing a window forward is not always immediate: a hotkey that uses the
    // Windows key can leave the Start menu holding the foreground for a moment,
    // and it lets go by itself. Retry briefly instead of dumping the phrase into
    // the clipboard at the first refusal.
    bool foregroundIsTarget = false;
    for (int attempt = 0; resolved != nullptr && attempt < 10; ++attempt) {
        if (::GetForegroundWindow() == resolved) {
            foregroundIsTarget = true;
            break;
        }
        ActivateWindow(resolved);
        if (::GetForegroundWindow() == resolved) {
            foregroundIsTarget = true;
            break;
        }
        if ((attempt == 0 || attempt == 3) && IsShellWindow(::GetForegroundWindow())) {
            // Dismiss the Start menu / search flyout with a harmless ESC. It is
            // injected, so the application's own hook ignores it.
            SendKey(VK_ESCAPE, false);
        }
        ::Sleep(120);
    }

    if (resolved != nullptr && !foregroundIsTarget) {
        // The window still refuses to come forward: leave the text where the
        // user can at least paste it rather than typing into the void.
        if (::OpenClipboard(nullptr)) {
            SetClipboardText(text);
            ::CloseClipboard();
        }
        util::Log(L"[insert] target could not be focused, text left on the clipboard");
        if (error != nullptr)
            *error = L"Окно не удалось активировать — текст скопирован в буфер обмена";
        return Delivery::ClipboardOnly;
    }

    if (foregroundIsTarget && TypeUnicode(text, options)) {
        const wchar_t last = text.GetAt(text.GetLength() - 1);
        if (options.appendSpace && last != L' ' && last != L'\n')
            SendChar(L' ');
        return Delivery::Typed;
    }

    util::Log(L"[insert] SendInput blocked or target lost (UIPI?), using clipboard");
    if (!options.clipboardFallback) {
        if (error != nullptr)
            *error = L"Не удалось ввести текст: целевое окно недоступно";
        return Delivery::ClipboardOnly;
    }
    if (!PasteViaClipboard(resolved, text, error)) {
        util::Log(L"[insert] clipboard fallback failed");
        return Delivery::ClipboardOnly;
    }
    if (options.appendSpace)
        SendChar(L' ');
    return Delivery::Pasted;
}

bool Insert(HWND target, const CStringW& text, const InsertOptions& options, CStringW* error)
{
    return InsertVerbose(target, text, options, error) != Delivery::ClipboardOnly;
}

}  // namespace textins