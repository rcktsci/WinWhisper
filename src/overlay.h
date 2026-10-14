#pragma once

#include <afxwin.h>

#include <functional>

enum class OverlayState {
    Listening,   // microphone open, waiting for a pause
    Processing,  // audio uploaded, waiting for the recogniser
    Error
};

struct OverlayModel {
    OverlayState state = OverlayState::Listening;
    float level = 0.0f;        // 0..1 microphone level
    bool speaking = false;
    CStringW status;           // one short line under the mic
    CStringW error;
};

// ---------------------------------------------------------------------------
//  The floating panel. It never takes focus (WS_EX_NOACTIVATE), so the caret
//  stays exactly where the user left it in whatever app they were typing in.
// ---------------------------------------------------------------------------
class OverlayWindow : public CWnd {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    bool Create();
    void ShowNear(HWND target);
    void Hide();
    void Update(const OverlayModel& model);
    bool IsVisible() const;

    // Draws the panel into any DC. Shared by OnPaint and by the --panel-shot
    // preview mode, so a preview is byte-for-byte what users see on screen.
    void Render(CDC& dc, int dpi, const OverlayModel& model);

    // Panel dimensions at the given scale, needed to size an off-screen bitmap.
    CSize PanelSize() const;

    std::function<void()> onClose;
    std::function<void()> onSettings;
    std::function<void()> onHelp;

protected:
    afx_msg void OnPaint();
    afx_msg void OnLButtonDown(UINT flags, CPoint point);
    afx_msg void OnLButtonUp(UINT flags, CPoint point);
    afx_msg void OnMouseMove(UINT flags, CPoint point);
    afx_msg LRESULT OnNcHitTest(CPoint point);
    afx_msg int OnMouseActivate(CWnd* pWnd, UINT hitCode, UINT flags);
    DECLARE_MESSAGE_MAP()

private:
    struct Metrics {
        int width = 0;
        int height = 0;
        int micRadius = 0;
        int micCx = 0;
        int micCy = 0;
        int closeX = 0;
        int closeY = 0;
        int gearX = 0;
        int gearY = 0;
        int helpX = 0;
        int helpY = 0;
        int buttonRadius = 0;
        int statusTop = 0;
        int statusHeight = 0;
        int handleY = 0;
        int handleWidth = 0;
    };

    void BuildMetrics(int dpiOverride = 0);
    void ApplyRegion();
    RECT ScreenRect() const;
    void BuildFonts();
    void DrawBackground(CDC& dc);
    void DrawMic(CDC& dc);
    void DrawGlyph(CDC& dc, CFont& font, wchar_t code, const CRect& box, COLORREF color);
    void DrawStatus(CDC& dc);
    void DrawHandle(CDC& dc);

    OverlayModel model_;
    Metrics metrics_;
    CFont iconSmall_;    // Segoe MDL2 Assets, buttons
    CFont textFont_;     // Segoe UI, status line
    CFont helpFont_;     // Segoe UI, question mark
    CFont emojiFont_;    // Segoe UI Emoji, the microphone
    int dpi_ = 96;
    bool fontsReady_ = false;
    bool dragging_ = false;
    int dragOffsetX_ = 0;
    int dragOffsetY_ = 0;
};

namespace overlay {

// Renders the panel into a PNG. Used by the --panel-shot diagnostic so the
// drawing can be inspected (and shown to whoever asked for a change) without
// popping the panel up on a live desktop.
bool RenderPanelToPng(const CStringW& path, int dpi, CStringW* error);

}  // namespace overlay