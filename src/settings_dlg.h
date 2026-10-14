#pragma once

#include <afxdialogex.h>

#include "resource.h"
#include "settings.h"

class CSettingsDialog : public CDialogEx {
public:
    enum { IDD = IDD_SETTINGS };

    CSettingsDialog(const Settings& current, CWnd* parent)
    : CDialogEx(IDD_SETTINGS, parent), settings_(current)
{
}

    const Settings& Result() const { return settings_; }
    bool Changed() const { return changed_; }

protected:
    void DoDataExchange(CDataExchange* dx) override;
    BOOL OnInitDialog() override;
    LRESULT OnProbeResult(WPARAM wParam, LPARAM lParam);
    afx_msg void OnTest();
    afx_msg void OnOk();
    DECLARE_MESSAGE_MAP()

private:
    void FillControls();
    bool ReadControls(CStringW* error);
    void SetHotkeyLabel();

    Settings settings_;
    bool changed_ = false;
};