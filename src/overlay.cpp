// windows.h defines min/max macros that fight <algorithm>; ask for the STL ones.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "util.h"

namespace {

constexpr wchar_t kOverlayClass[] = L"WinWhisperOverlayWindow";

// Palette close to the built-in typist bar: neutral surface, one accent.
constexpr COLORREF kSurface = RGB(0xF9, 0xF9, 0xFB);
constexpr COLORREF kBorder = RGB(0xD6, 0xD7, 0xDC);
constexpr COLORREF kGlyph = RGB(0x3A, 0x3A, 0x3C);
constexpr COLORREF kGlyphDim = RGB(0x9A, 0x9C, 0xA3);
constexpr COLORREF kAccent = RGB(0x1F, 0x7A, 0xE8);
constexpr COLORREF kDanger = RGB(0xD1, 0x36, 0x36);
constexpr COLORREF kDangerSurface = RGB(0xFD, 0xEC, 0xEC);

// The built-in typist draws its microphone in a warm red; matching it makes the
// panel read as the same kind of control.
constexpr COLORREF kMicInk = RGB(0xC0, 0x39, 0x3E);

// Windows ships these two fonts, so the panel gets properly hinted icons at
// every scale factor without shipping a single bitmap.
constexpr const wchar_t* kIconFont = L"Segoe MDL2 Assets";
constexpr const wchar_t* kTextFont = L"Segoe UI";
constexpr wchar_t kGlyphSettings = static_cast<wchar_t>(0xE713);  // gear
constexpr wchar_t kGlyphClose = static_cast<wchar_t>(0xE711);     // cancel / X

int Scale(int value, int dpi) { return MulDiv(value, dpi, 96); }

// Win32's Arc() wants start/end as points on the ellipse; this draws a sweep
// given in degrees, which is what a level meter actually needs.
void ArcDegrees(CDC& dc, const CRect& box, int startDeg, int endDeg)
{
    const double cx = (box.left + box.right) / 2.0;
    const double cy = (box.top + box.bottom) / 2.0;
    const double rx = (box.right - box.left) / 2.0;
    const double ry = (box.bottom - box.top) / 2.0;

    auto pointOnEllipse = [&](int degrees) {
        const double rad = degrees * 3.14159265358979323846 / 180.0;
        POINT p;
        p.x = static_cast<int>(cx + rx * std::cos(rad));
        p.y = static_cast<int>(cy + ry * std::sin(rad));
        return p;
    };

    const POINT start = pointOnEllipse(startDeg);
    const POINT end = pointOnEllipse(endDeg);
    dc.Arc(box.left, box.top, box.right, box.bottom, start.x, start.y, end.x, end.y);
}

}  // namespace

BEGIN_MESSAGE_MAP(OverlayWindow, CWnd)
    ON_WM_PAINT()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_NCHITTEST()
    ON_WM_MOUSEACTIVATE()
END_MESSAGE_MAP()

bool OverlayWindow::Create()
{
    WNDCLASS wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = AfxGetInstanceHandle();
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    if (::RegisterClassW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    if (!CreateEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kOverlayClass, L"",
                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, AfxGetInstanceHandle())) {
        return false;
    }

    // A window that never activates would keep painting in the task manager's
    // eyes, but it must also never appear in Alt+Tab - WS_EX_TOOLWINDOW covers it.
    return true;
}

bool OverlayWindow::IsVisible() const
{
    return GetSafeHwnd() != nullptr && ::IsWindowVisible(GetSafeHwnd());
}

void OverlayWindow::BuildMetrics()
{
    const HMONITOR monitor = ::MonitorFromWindow(GetSafeHwnd(), MONITOR_DEFAULTTONEAREST);
    UINT dpi = 96;
    if (monitor != nullptr) {
        // GetDpiForWindow honours the per-monitor v2 awareness from the manifest.
        if (UINT w = ::GetDpiForWindow(GetSafeHwnd()); w != 0)
            dpi = w;
    }
    dpi_ = static_cast<int>(dpi);

    Metrics m;
    m.width = Scale(340, dpi_);
    m.height = Scale(152, dpi_);
    m.micRadius = Scale(30, dpi_);
    m.micCx = m.width / 2;
    m.micCy = Scale(70, dpi_);
    m.closeX = m.width - Scale(21, dpi_);
    m.closeY = Scale(17, dpi_);
    m.gearX = Scale(28, dpi_);
    m.gearY = m.height - Scale(25, dpi_);
    m.helpX = m.width - Scale(28, dpi_);
    m.helpY = m.height - Scale(25, dpi_);
    m.buttonRadius = Scale(13, dpi_);
    m.statusTop = Scale(110, dpi_);
    m.statusHeight = Scale(22, dpi_);
    m.handleY = Scale(10, dpi_);
    m.handleWidth = Scale(36, dpi_);
    metrics_ = m;

    BuildFonts();
}

void OverlayWindow::ApplyRegion()
{
    if (GetSafeHwnd() == nullptr)
        return;
    HRGN region = ::CreateRoundRectRgn(0, 0, metrics_.width + 1, metrics_.height + 1,
                                       Scale(14, dpi_), Scale(14, dpi_));
    ::SetWindowRgn(GetSafeHwnd(), region, TRUE);  // window owns it after this call
}

RECT OverlayWindow::ScreenRect() const
{
    // Anchor to the caret so the panel appears where the user is typing, the
    // same place the built-in typist puts it.
    HWND target = ::GetForegroundWindow();
    RECT anchor = {};
    bool haveCaret = false;

    if (target != nullptr && target != GetSafeHwnd()) {
        GUITHREADINFO info = {};
        info.cbSize = sizeof(info);
        const DWORD thread = ::GetWindowThreadProcessId(target, nullptr);
        if (thread != 0 && ::GetGUIThreadInfo(thread, &info)) {
            if (info.rcCaret.right > info.rcCaret.left || info.rcCaret.bottom > info.rcCaret.top) {
                anchor = info.rcCaret;
                haveCaret = true;
            }
        }
    }

    if (!haveCaret) {
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        anchor.left = cursor.x;
        anchor.right = cursor.x + 1;
        anchor.top = cursor.y;
        anchor.bottom = cursor.y + 1;
    }

    const HMONITOR monitor = ::MonitorFromPoint(POINT{anchor.left, anchor.top},
                                                MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(monitor, &mi);

    const int workLeft = mi.rcWork.left;
    const int workTop = mi.rcWork.top;
    const int workRight = mi.rcWork.right;
    const int workBottom = mi.rcWork.bottom;

    const int centreX = (anchor.left + anchor.right) / 2;
    int x = centreX - metrics_.width / 2;
    int y = anchor.bottom + Scale(8, dpi_);

    // Prefer the space under the caret; flip above when it does not fit.
    if (y + metrics_.height > workBottom)
        y = anchor.top - metrics_.height - Scale(8, dpi_);
    if (y < workTop)
        y = workBottom - metrics_.height - Scale(8, dpi_);

    x = std::max(workLeft, std::min(x, workRight - metrics_.width));
    y = std::max(workTop, std::min(y, workBottom - metrics_.height));

    RECT r = {x, y, x + metrics_.width, y + metrics_.height};
    return r;
}

void OverlayWindow::ShowNear(HWND target)
{
    (void)target;
    if (GetSafeHwnd() == nullptr && !Create())
        return;

    BuildMetrics();
    ApplyRegion();

    const RECT rect = ScreenRect();
    ::SetWindowPos(GetSafeHwnd(), HWND_TOPMOST, rect.left, rect.top, metrics_.width,
                   metrics_.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Invalidate();
}

void OverlayWindow::Hide()
{
    if (GetSafeHwnd() != nullptr && ::IsWindowVisible(GetSafeHwnd()))
        ::ShowWindow(GetSafeHwnd(), SW_HIDE);
}

void OverlayWindow::Update(const OverlayModel& model)
{
    model_ = model;
    if (GetSafeHwnd() != nullptr && ::IsWindowVisible(GetSafeHwnd()))
        Invalidate(FALSE);
}



LRESULT OverlayWindow::OnNcHitTest(CPoint point)
{
    // Everything is handled with client messages: a non-activating window is a
    // poor citizen for the system's caption/move loop, so dragging is done by
    // hand below.
    return HTCLIENT;
}

int OverlayWindow::OnMouseActivate(CWnd* pWnd, UINT hitCode, UINT flags)
{
    (void)pWnd;
    (void)hitCode;
    (void)flags;
    return MA_NOACTIVATE;   // never steal focus from the edit control
}

void OverlayWindow::OnLButtonDown(UINT flags, CPoint point)
{
    (void)flags;

    const int buttonRadius = metrics_.buttonRadius * 2;
    const bool onButton = std::abs(point.x - metrics_.closeX) <= buttonRadius &&
                          std::abs(point.y - metrics_.closeY) <= buttonRadius;

    if (!onButton && point.y <= metrics_.handleY + Scale(14, dpi_)) {
        // Start moving the panel: remember where inside it the grab happened.
        dragging_ = true;
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        RECT rect = {};
        ::GetWindowRect(GetSafeHwnd(), &rect);
        dragOffsetX_ = cursor.x - rect.left;
        dragOffsetY_ = cursor.y - rect.top;
        ::SetCapture(GetSafeHwnd());
        return;
    }

    CWnd::OnLButtonDown(flags, point);
}

void OverlayWindow::OnMouseMove(UINT flags, CPoint point)
{
    if (dragging_) {
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        ::SetWindowPos(GetSafeHwnd(), HWND_TOPMOST, cursor.x - dragOffsetX_,
                       cursor.y - dragOffsetY_, 0, 0,
                       SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        return;
    }
    CWnd::OnMouseMove(flags, point);
}

void OverlayWindow::OnLButtonUp(UINT flags, CPoint point)
{
    (void)flags;

    if (dragging_) {
        dragging_ = false;
        ::ReleaseCapture();
        return;
    }
    ::ReleaseCapture();

    const int hit = metrics_.buttonRadius * 2;
    if (std::abs(point.x - metrics_.closeX) <= hit && std::abs(point.y - metrics_.closeY) <= hit) {
        if (onClose)
            onClose();
        return;
    }
    if (std::abs(point.x - metrics_.gearX) <= hit && std::abs(point.y - metrics_.gearY) <= hit) {
        if (onSettings)
            onSettings();
        return;
    }
    if (std::abs(point.x - metrics_.helpX) <= hit && std::abs(point.y - metrics_.helpY) <= hit) {
        if (onHelp)
            onHelp();
    }
}

// ----------------------------------------------------------------- paint ---

OverlayWindow::~OverlayWindow()
{
    iconSmall_.DeleteObject();
    textFont_.DeleteObject();
    helpFont_.DeleteObject();
}

void OverlayWindow::BuildFonts()
{
    iconSmall_.DeleteObject();
    textFont_.DeleteObject();
    helpFont_.DeleteObject();

    const auto pointSize = [](int pixels, int dpi) {
        // CFont::CreatePointFont wants tenths of a point.
        return MulDiv(pixels, 720, dpi);
    };

    // Segoe MDL2 Assets ships with Windows 10/11 and gives crisp, correctly
    // hinted icons at any DPI - much nicer than hand-rolled GDI shapes.
    iconSmall_.CreatePointFont(pointSize(Scale(15, dpi_), dpi_), kIconFont);
    textFont_.CreatePointFont(pointSize(Scale(13, dpi_), dpi_), kTextFont);
    helpFont_.CreatePointFont(pointSize(Scale(15, dpi_), dpi_), kTextFont);
    fontsReady_ = iconSmall_.GetSafeHandle() != nullptr;
}

void OverlayWindow::DrawGlyph(CDC& dc, CFont& font, wchar_t code, const CRect& box, COLORREF color)
{
    if (!fontsReady_)
        return;
    CFont* old = dc.SelectObject(&font);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(color);
    CRect target = box;
    dc.DrawText(CStringW(code), target, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    dc.SelectObject(old);
}

void OverlayWindow::OnPaint()
{
    CPaintDC dc(this);
    CRect client;
    GetClientRect(client);

    CDC mem;
    mem.CreateCompatibleDC(&dc);
    CBitmap bitmap;
    bitmap.CreateCompatibleBitmap(&dc, std::max(1, client.Width()), std::max(1, client.Height()));
    CBitmap* old = mem.SelectObject(&bitmap);

    mem.FillSolidRect(client, kSurface);
    DrawBackground(mem);
    DrawHandle(mem);

    const int r = metrics_.buttonRadius;
    CRect closeBox(metrics_.closeX - r, metrics_.closeY - r, metrics_.closeX + r,
                    metrics_.closeY + r);
    DrawGlyph(mem, iconSmall_, kGlyphClose, closeBox, kGlyphDim);

    DrawMic(mem);

    CRect gearBox(metrics_.gearX - r, metrics_.gearY - r, metrics_.gearX + r,
                  metrics_.gearY + r);
    DrawGlyph(mem, iconSmall_, kGlyphSettings, gearBox, kGlyphDim);

    DrawStatus(mem);

    dc.BitBlt(0, 0, client.Width(), client.Height(), &mem, 0, 0, SRCCOPY);

    mem.SelectObject(old);
    bitmap.DeleteObject();
    mem.DeleteDC();
}

void OverlayWindow::DrawBackground(CDC& dc)
{
    CPen pen(PS_SOLID, 1, kBorder);
    CPen* oldPen = dc.SelectObject(&pen);
    dc.SelectObject(::GetStockObject(NULL_BRUSH));
    dc.RoundRect(0, 0, metrics_.width, metrics_.height, Scale(12, dpi_), Scale(12, dpi_));
    dc.SelectObject(oldPen);
}

void OverlayWindow::DrawHandle(CDC& dc)
{
    const int thickness = std::max(2, Scale(3, dpi_));
    CPen pen(PS_SOLID, thickness, RGB(0xC6, 0xC7, 0xCE));
    CPen* oldPen = dc.SelectObject(&pen);
    dc.RoundRect(metrics_.width / 2 - metrics_.handleWidth / 2, metrics_.handleY,
                 metrics_.width / 2 + metrics_.handleWidth / 2,
                 metrics_.handleY + thickness, Scale(3, dpi_), Scale(3, dpi_));
    dc.SelectObject(oldPen);
}

// The big button: a light disc with a red microphone, exactly like the built-in
// typist bar. Drawn with primitives so it scales cleanly at any DPI.
void OverlayWindow::DrawMic(CDC& dc)
{
    const bool error = model_.state == OverlayState::Error;
    const bool processing = model_.state == OverlayState::Processing;
    const COLORREF ink = error ? kDanger : kMicInk;

    const CRect circle(metrics_.micCx - metrics_.micRadius, metrics_.micCy - metrics_.micRadius,
                       metrics_.micCx + metrics_.micRadius, metrics_.micCy + metrics_.micRadius);

    CBrush fill(error ? kDangerSurface : RGB(0xF1, 0xF2, 0xF5));
    CBrush* oldBrush = dc.SelectObject(&fill);
    CPen edge(PS_SOLID, 1, error ? RGB(0xF3, 0xC9, 0xC9) : RGB(0xE3, 0xE4, 0xE9));
    CPen* oldPen = dc.SelectObject(&edge);
    dc.Ellipse(circle);
    dc.SelectObject(oldPen);
    dc.SelectObject(oldBrush);

    // --- microphone silhouette, sized from the button radius -----------------
    const double r = metrics_.micRadius;
    const double cx = metrics_.micCx;
    const double cy = metrics_.micCy;

    const double capHalf = r * 0.19;          // capsule half-width = cap radius
    const double capTop = cy - r * 0.58;
    const double capBottom = cy + r * 0.12;

    const double cradleR = r * 0.42;
    const double cradleCy = cy - r * 0.06;
    const double cradleBottom = cradleCy + cradleR;

    const int stroke = std::max(2, static_cast<int>(r * 0.10));
    CPen micPen(PS_SOLID, stroke, ink);
    oldPen = dc.SelectObject(&micPen);
    // Strokes must be hollow, otherwise RoundRect/Arc fill themselves with the
    // DC's default brush and the glyph turns into a blob.
    CBrush* oldBrush2 = dc.SelectObject(
        CBrush::FromHandle(static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH))));

    dc.RoundRect(static_cast<int>(cx - capHalf), static_cast<int>(capTop),
                 static_cast<int>(cx + capHalf), static_cast<int>(capBottom),
                 static_cast<int>(capHalf * 2), static_cast<int>(capHalf * 2));

    ArcDegrees(dc, CRect(static_cast<int>(cx - cradleR), static_cast<int>(cradleCy - cradleR),
                         static_cast<int>(cx + cradleR), static_cast<int>(cradleCy + cradleR)),
               0, 180);

    dc.MoveTo(static_cast<int>(cx), static_cast<int>(cradleBottom));
    dc.LineTo(static_cast<int>(cx), static_cast<int>(cy + r * 0.52));
    dc.MoveTo(static_cast<int>(cx - r * 0.28), static_cast<int>(cy + r * 0.52));
    dc.LineTo(static_cast<int>(cx + r * 0.28), static_cast<int>(cy + r * 0.52));

    dc.SelectObject(oldBrush2);
    dc.SelectObject(oldPen);

    // --- level ring: a quiet arc that only shows while something is heard ----
    if (model_.state == OverlayState::Listening) {
        const float level = std::min(1.0f, std::max(0.0f, model_.level));
        if (level > 0.05f) {
            const int thickness2 = std::max(2, Scale(3, dpi_));
            const CRect outer(circle.left - thickness2, circle.top - thickness2,
                              circle.right + thickness2, circle.bottom + thickness2);
            CPen ring(PS_SOLID, thickness2, kAccent);
            oldPen = dc.SelectObject(&ring);
            ArcDegrees(dc, outer, -90, -90 + static_cast<int>(359.0f * level));
            dc.SelectObject(oldPen);
        }
    } else if (processing) {
        const int thickness3 = std::max(2, Scale(3, dpi_));
        const CRect outer(circle.left - thickness3, circle.top - thickness3,
                          circle.right + thickness3, circle.bottom + thickness3);
        CPen ring(PS_SOLID, thickness3, RGB(0xC2, 0xC4, 0xCC));
        oldPen = dc.SelectObject(&ring);
        const int angle = static_cast<int>(::GetTickCount64() / 8) % 360;
        ArcDegrees(dc, outer, angle, angle + 80);
        dc.SelectObject(oldPen);
    }
}

void OverlayWindow::DrawStatus(CDC& dc)
{
    CStringW text = model_.error.IsEmpty() ? model_.status : model_.error;
    if (text.IsEmpty()) {
        switch (model_.state) {
        case OverlayState::Listening: text = L"Слушаю…"; break;
        case OverlayState::Processing: text = L"Распознаю…"; break;
        case OverlayState::Error: text = L"Ошибка"; break;
        }
    }

    CFont* old = fontsReady_ ? dc.SelectObject(&textFont_) : nullptr;
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(model_.state == OverlayState::Error ? kDanger : RGB(0x55, 0x57, 0x5C));

    CRect box(metrics_.width / 8, metrics_.statusTop, metrics_.width - metrics_.width / 8,
              metrics_.statusTop + metrics_.statusHeight);
    dc.DrawText(text, box, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (old != nullptr)
        dc.SelectObject(old);

    const int r = metrics_.buttonRadius;
    CRect help(metrics_.helpX - r, metrics_.helpY - r, metrics_.helpX + r,
               metrics_.helpY + r);
    DrawGlyph(dc, helpFont_, L'?', help, kGlyphDim);
}