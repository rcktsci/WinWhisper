#pragma once

#include <afxwin.h>

class CWinWhisperApp : public CWinApp {
public:
    BOOL InitInstance() override;
    int ExitInstance() override;

    DECLARE_MESSAGE_MAP()
};