#pragma once

#include <afxwin.h>

struct InsertOptions {
    bool appendSpace = true;
    bool newlineAsShiftEnter = true;
    bool clipboardFallback = true;
};

// ---------------------------------------------------------------------------
//  Types recognised text into whatever window was focused when the user hit the
//  hotkey. The overlay itself never takes focus, so this has to re-activate the
//  target window first - otherwise SendInput would land in the wrong place.
// ---------------------------------------------------------------------------
namespace textins {

enum class Delivery {
    Typed,        // keystrokes went into the target window
    Pasted,       // typed path was unavailable, pasted through the clipboard
    ClipboardOnly // target could not be focused: text is on the clipboard
};

// Returns false and fills `error` when the text could not be delivered at all.
bool Insert(HWND target, const CStringW& text, const InsertOptions& options, CStringW* error);

// Same, but reports how the text was delivered.
Delivery InsertVerbose(HWND target, const CStringW& text, const InsertOptions& options,
                       CStringW* error);

// True when `window` is part of the Windows shell (taskbar, Start, desktop) and
// therefore never a sensible place to type into.
bool IsShellWindow(HWND window);

// True while recognised text is being typed. The keystrokes we inject reach the
// low-level keyboard hook as well, and they must not be mistaken for the user
// going back to the keyboard.
bool IsTyping();

}  // namespace textins