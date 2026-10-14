// windows.h defines min/max macros that fight <algorithm>; ask for the STL ones.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "overlay.h"

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

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
// The colour emoji font. GDI paints these in colour; GDI+ cannot.
constexpr const wchar_t* kEmojiFont = L"Segoe UI Emoji";
constexpr wchar_t kGlyphSettings = static_cast<wchar_t>(0xE713);  // gear
constexpr wchar_t kGlyphClose = static_cast<wchar_t>(0xE711);     // cancel / X
// U+1F3A4 STUDIO MICROPHONE, as a surrogate pair.
constexpr wchar_t kMicEmoji[] = L"\xD83C\xDFA4";

int Scale(int value, int dpi) { return MulDiv(value, dpi, 96); }

// ---------------------------------------------------------------------------
//  Anti-aliasing.
//
//  GDI has no anti-aliased geometry at all: Arc/Ellipse/RoundRect/LinTo snap to
//  whole pixels, which is why the panel used to look like it was drawn with
//  Lego. Everything geometric therefore goes through GDI+ with smoothing on,
//  while text and the emoji stay on GDI - GDI+ cannot render colour fonts, and
//  the emoji would come out as a flat silhouette.
// ---------------------------------------------------------------------------
ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

void EnsureGdiPlus()
{
    if (g_gdiplusReady)
        return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok)
        g_gdiplusReady = true;
}

Gdiplus::Color FromColor(COLORREF value, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, GetRValue(value), GetGValue(value), GetBValue(value));
}

// Rounded rectangle as a path, so it can be both filled and stroked smoothly.
Gdiplus::GraphicsPath* RoundedPath(const Gdiplus::RectF& r, float radius)
{
    auto* path = new Gdiplus::GraphicsPath();
    const float d = radius * 2.0f;
    path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

void FillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 const Gdiplus::Color& fill, const Gdiplus::Color& border, float borderWidth)
{
    Gdiplus::GraphicsPath* path = RoundedPath(r, radius);
    Gdiplus::SolidBrush brush(fill);
    g.FillPath(&brush, path);
    if (borderWidth > 0.0f) {
        Gdiplus::Pen pen(border, borderWidth);
        g.DrawPath(&pen, path);
    }
    delete path;
}

// A GDI+ canvas bound to a DC for the duration of one call.
class SmoothCanvas {
public:
    explicit SmoothCanvas(CDC& dc) : graphics_(dc.GetSafeHdc())
    {
        graphics_.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics_.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
    Gdiplus::Graphics& get() { return graphics_; }

private:
    Gdiplus::Graphics graphics_;
};

// ---------------------------------------------------------------------------
//  Colour emoji.
//
//  Segoe UI Emoji on Windows 11 is a COLR/CPAL font and GDI cannot paint those:
//  DrawText yields a flat monochrome silhouette. DirectWrite/Direct2D can, so
//  the microphone goes through them. Both factories live for the whole process.
// ---------------------------------------------------------------------------
ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;

bool EnsureDirectWrite()
{
    if (g_d2dFactory == nullptr)
        ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
    if (g_dwriteFactory == nullptr)
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(&g_dwriteFactory));
    return g_d2dFactory != nullptr && g_dwriteFactory != nullptr;
}

// Returns false when Direct2D cannot bind the DC (a 16-bit desktop, say); the
// caller then falls back to the monochrome GDI glyph.
bool DrawEmoji(CDC& dc, const CRect& box, float size, COLORREF fallbackColor)
{
    if (!EnsureDirectWrite())
        return false;

    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);

    ID2D1DCRenderTarget* target = nullptr;
    if (FAILED(g_d2dFactory->CreateDCRenderTarget(&props, &target)))
        return false;

    const RECT bind = {box.left, box.top, box.right, box.bottom};
    bool ok = false;
    if (SUCCEEDED(target->BindDC(dc.GetSafeHdc(), &bind))) {
        IDWriteTextFormat* format = nullptr;
        if (SUCCEEDED(g_dwriteFactory->CreateTextFormat(
                L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ID2D1SolidColorBrush* brush = nullptr;
            if (SUCCEEDED(target->CreateSolidColorBrush(
                    D2D1::ColorF(GetRValue(fallbackColor) / 255.0f,
                                 GetGValue(fallbackColor) / 255.0f,
                                 GetBValue(fallbackColor) / 255.0f, 1.0f),
                    &brush))) {
                target->BeginDraw();
                target->DrawText(kMicEmoji, 2, format,
                                 D2D1::RectF(0.0f, 0.0f, static_cast<float>(box.Width()),
                                             static_cast<float>(box.Height())),
                                 brush);
                ok = SUCCEEDED(target->EndDraw());
                brush->Release();
            }
            format->Release();
        }
    }
    target->Release();
    return ok;
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

void OverlayWindow::BuildMetrics(int dpiOverride)
{
    UINT dpi = 96;
    if (dpiOverride > 0) {
        // Off-screen rendering knows the scale it wants; there is no window to
        // ask.
        dpi = static_cast<UINT>(dpiOverride);
    } else {
        const HMONITOR monitor = ::MonitorFromWindow(GetSafeHwnd(), MONITOR_DEFAULTTONEAREST);
        if (monitor != nullptr) {
            // GetDpiForWindow honours the per-monitor v2 awareness from the manifest.
            if (UINT w = ::GetDpiForWindow(GetSafeHwnd()); w != 0)
                dpi = w;
        }
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
    emojiFont_.DeleteObject();

    const auto pointSize = [](int pixels, int dpi) {
        // CFont::CreatePointFont wants tenths of a point.
        return MulDiv(pixels, 720, dpi);
    };

    // Segoe MDL2 Assets ships with Windows 10/11 and gives crisp, correctly
    // hinted icons at any DPI - much nicer than hand-rolled GDI shapes.
    iconSmall_.CreatePointFont(pointSize(Scale(15, dpi_), dpi_), kIconFont);
    textFont_.CreatePointFont(pointSize(Scale(13, dpi_), dpi_), kTextFont);
    helpFont_.CreatePointFont(pointSize(Scale(15, dpi_), dpi_), kTextFont);
    // The emoji needs a noticeably larger em than the surrounding text: colour
    // glyphs carry their own padding.
    emojiFont_.CreatePointFont(pointSize(Scale(26, dpi_), dpi_), kEmojiFont);
    fontsReady_ = iconSmall_.GetSafeHandle() != nullptr;
}

CSize OverlayWindow::PanelSize() const
{
    return CSize(metrics_.width, metrics_.height);
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

    Render(mem, dpi_, model_);

    dc.BitBlt(0, 0, client.Width(), client.Height(), &mem, 0, 0, SRCCOPY);

    mem.SelectObject(old);
    bitmap.DeleteObject();
    mem.DeleteDC();
}

// Draws the whole panel into `dc`. OnPaint and the --panel-shot preview both
// come through here, so what the preview shows is what users see.
void OverlayWindow::Render(CDC& dc, int dpi, const OverlayModel& model)
{
    EnsureGdiPlus();
    dpi_ = dpi;
    model_ = model;
    BuildMetrics(dpi);
    BuildFonts();

    dc.FillSolidRect(CRect(0, 0, metrics_.width, metrics_.height), kSurface);

    DrawBackground(dc);
    DrawHandle(dc);
    DrawMic(dc);

    // GDI from here on: text quality and the colour emoji need it.
    const int r = metrics_.buttonRadius;
    CRect closeBox(metrics_.closeX - r, metrics_.closeY - r, metrics_.closeX + r,
                    metrics_.closeY + r);
    DrawGlyph(dc, iconSmall_, kGlyphClose, closeBox, kGlyphDim);

    CRect gearBox(metrics_.gearX - r, metrics_.gearY - r, metrics_.gearX + r,
                  metrics_.gearY + r);
    DrawGlyph(dc, iconSmall_, kGlyphSettings, gearBox, kGlyphDim);

    DrawStatus(dc);
}

void OverlayWindow::DrawBackground(CDC& dc)
{
    SmoothCanvas canvas(dc);
    const float half = Scale(1, dpi_) / 2.0f;
    const Gdiplus::RectF box(half, half, static_cast<float>(metrics_.width) - Scale(1, dpi_),
                             static_cast<float>(metrics_.height) - Scale(1, dpi_));
    FillRounded(canvas.get(), box, static_cast<float>(Scale(12, dpi_)), FromColor(kSurface),
                FromColor(kBorder), static_cast<float>(Scale(1, dpi_)));
}

void OverlayWindow::DrawHandle(CDC& dc)
{
    SmoothCanvas canvas(dc);
    const float thickness = static_cast<float>(std::max(2, Scale(3, dpi_)));
    const float left = static_cast<float>(metrics_.width) / 2.0f -
                       static_cast<float>(metrics_.handleWidth) / 2.0f;
    const Gdiplus::RectF box(left, static_cast<float>(metrics_.handleY),
                             static_cast<float>(metrics_.handleWidth), thickness);
    FillRounded(canvas.get(), box, thickness / 2.0f, FromColor(RGB(0xC6, 0xC7, 0xCE)),
                FromColor(RGB(0xC6, 0xC7, 0xCE)), 0.0f);
}

// The big button: a light disc carrying the microphone emoji, like the built-in
// typist bar. The disc and the level ring are drawn through GDI+ so their edges
// are smooth; the emoji itself is drawn with GDI, because only GDI knows how to
// paint the colour font.
void OverlayWindow::DrawMic(CDC& dc)
{
    const bool error = model_.state == OverlayState::Error;
    const bool processing = model_.state == OverlayState::Processing;

    const float cx = static_cast<float>(metrics_.micCx);
    const float cy = static_cast<float>(metrics_.micCy);
    const float r = static_cast<float>(metrics_.micRadius);

    {
        SmoothCanvas canvas(dc);
        Gdiplus::SolidBrush fill(FromColor(error ? kDangerSurface : RGB(0xF1, 0xF2, 0xF5)));
        Gdiplus::Pen edge(FromColor(error ? RGB(0xF3, 0xC9, 0xC9) : RGB(0xE3, 0xE4, 0xE9)),
                          static_cast<float>(Scale(1, dpi_)));
        const Gdiplus::RectF disc(cx - r, cy - r, r * 2.0f, r * 2.0f);
        canvas.get().FillEllipse(&fill, disc);
        canvas.get().DrawEllipse(&edge, disc);

        // --- level ring: a quiet arc that only shows while something is heard --
        const float thickness = static_cast<float>(std::max(2, Scale(3, dpi_)));
        const float ringR = r + thickness * 0.6f;
        const Gdiplus::RectF ring(cx - ringR, cy - ringR, ringR * 2.0f, ringR * 2.0f);
        Gdiplus::Pen pen(Gdiplus::Color(0, 0, 0, 0), thickness);
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);

        if (model_.state == OverlayState::Listening) {
            const float level = std::min(1.0f, std::max(0.0f, model_.level));
            if (level > 0.05f) {
                pen.SetColor(FromColor(kAccent));
                canvas.get().DrawArc(&pen, ring, -90.0f, 359.0f * level);
            }
        } else if (processing) {
            // A short arc chasing its own tail while the server is thinking.
            pen.SetColor(FromColor(RGB(0xC2, 0xC4, 0xCC)));
            const float angle = static_cast<float>(::GetTickCount64() / 8 % 360);
            canvas.get().DrawArc(&pen, ring, angle, 80.0f);
        }
    }

    // --- the microphone emoji -------------------------------------------------
    // U+1F3A4 is outside the BMP, hence the surrogate pair.
    const CRect emojiBox(metrics_.micCx - metrics_.micRadius, metrics_.micCy - metrics_.micRadius,
                         metrics_.micCx + metrics_.micRadius, metrics_.micCy + metrics_.micRadius);
    const COLORREF ink = error ? kDanger : kGlyph;
    const float emojiSize = static_cast<float>(metrics_.micRadius) * 1.7f;
    if (!DrawEmoji(dc, emojiBox, emojiSize, ink)) {
        // Older desktops without a usable Direct2D binding still get a mic.
        CFont* old = fontsReady_ ? dc.SelectObject(&emojiFont_) : nullptr;
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(ink);
        CRect fallbackBox = emojiBox;
        dc.DrawText(CStringW(kMicEmoji), fallbackBox,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (old != nullptr)
            dc.SelectObject(old);
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

namespace overlay {

namespace {

// GDI+ needs the encoder GUID for the format it is asked to save.
bool PngEncoderClsid(CLSID* clsid)
{
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
        return false;

    std::vector<BYTE> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok)
        return false;

    for (UINT i = 0; i < count; ++i) {
        if (::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

}  // namespace

bool RenderPanelToPng(const CStringW& path, int dpi, CStringW* error)
{
    EnsureGdiPlus();
    if (!g_gdiplusReady) {
        if (error != nullptr)
            *error = L"GDI+ не запустился";
        return false;
    }

    // Render off-screen at the requested scale.
    OverlayWindow panel;
    OverlayModel model;
    model.state = OverlayState::Listening;
    model.level = 0.62f;
    model.speaking = true;
    model.status = L"Слушаю…";

    CClientDC screen(nullptr);
    CDC mem;
    mem.CreateCompatibleDC(&screen);

    // The panel is its own thing; measure it first so the bitmap fits.
    panel.Render(mem, dpi, model);
    const CSize size = panel.PanelSize();
    if (size.cx <= 0 || size.cy <= 0) {
        if (error != nullptr)
            *error = L"не удалось измерить панель";
        mem.DeleteDC();
        return false;
    }

    CBitmap bitmap;
    bitmap.CreateCompatibleBitmap(&screen, size.cx, size.cy);
    CBitmap* old = mem.SelectObject(&bitmap);
    panel.Render(mem, dpi, model);

    Gdiplus::Bitmap image(static_cast<HBITMAP>(bitmap.GetSafeHandle()), nullptr);
    CLSID pngEncoder{};
    bool ok = false;
    if (PngEncoderClsid(&pngEncoder))
        ok = image.Save(path.GetString(), &pngEncoder, nullptr) == Gdiplus::Ok;
    if (!ok && error != nullptr)
        *error = L"не удалось сохранить PNG";

    mem.SelectObject(old);
    bitmap.DeleteObject();
    mem.DeleteDC();
    return ok;
}

}  // namespace overlay