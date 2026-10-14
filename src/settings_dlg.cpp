#include "settings_dlg.h"

#include <cwctype>
#include <memory>

#include "resource.h"
#include "util.h"
#include "whisper_client.h"

namespace {

constexpr UINT WM_PROBE_DONE = WM_APP + 77;

struct LanguageEntry {
    const wchar_t* label;
    const wchar_t* code;
};

const LanguageEntry kLanguages[] = {
    {L"Автоопределение", L""}, {L"Русский", L"ru"},     {L"Английский", L"en"},
    {L"Украинский", L"uk"},    {L"Немецкий", L"de"},    {L"Французский", L"fr"},
    {L"Испанский", L"es"},     {L"Польский", L"pl"},    {L"Турецкий", L"tr"},
    {L"Китайский", L"zh"},     {L"Японский", L"ja"},    {L"Корейский", L"ko"},
};

// Value first: the combo stores the code in its item data.
struct AudioFormatEntry {
    const wchar_t* label;
    const wchar_t* code;
};

const AudioFormatEntry kAudioFormats[] = {
    {L"M4A (AAC) — компактнее", L"m4a"},
    {L"WAV — без потерь", L"wav"},
};

struct ProbeJob {
    HWND owner = nullptr;
    CStringW baseUrl;
    CStringW apiKey;
    CStringW model;
    bool ok = false;
    CStringW message;
};

DWORD WINAPI ProbeThread(LPVOID param)
{
    ProbeJob* job = static_cast<ProbeJob*>(param);
    int status = 0;
    CStringW error;
    job->ok = WhisperClient::Probe(job->baseUrl, job->apiKey, job->model, 10000, &error, &status);
    job->message = error;
    ::PostMessageW(job->owner, WM_PROBE_DONE, 0, reinterpret_cast<LPARAM>(job));
    return 0;
}

// ---------------------------------------------------------------------------
//  Model list, fetched in the background so opening the dialog is instant.
//  The list is a convenience: the combo stays editable, so a server that does
//  not implement /models (or a name that is not listed) still works.
// ---------------------------------------------------------------------------
constexpr UINT WM_MODELS_DONE = WM_APP + 78;

struct ModelsJob {
    HWND owner = nullptr;
    CStringW baseUrl;
    CStringW apiKey;
    std::vector<CStringW> models;
    CStringW error;
};

DWORD WINAPI ModelsThread(LPVOID param)
{
    ModelsJob* job = static_cast<ModelsJob*>(param);
    CStringW error;
    if (!WhisperClient::ListModels(job->baseUrl, job->apiKey, 8000, &job->models, &error))
        job->error = error;
    ::PostMessageW(job->owner, WM_MODELS_DONE, 0, reinterpret_cast<LPARAM>(job));
    return 0;
}

CStringW ControlText(CWnd& dlg, int id)
{
    CStringW text;
    dlg.GetDlgItemText(id, text);
    return text;
}

bool IsBoxChecked(CWnd& dlg, int id)
{
    CButton* button = static_cast<CButton*>(dlg.GetDlgItem(id));
    return button != nullptr && button->GetCheck() == BST_CHECKED;
}

}  // namespace

BEGIN_MESSAGE_MAP(CSettingsDialog, CDialogEx)
    ON_BN_CLICKED(IDC_TEST, &CSettingsDialog::OnTest)
    ON_BN_CLICKED(IDC_REFRESH_MODELS, &CSettingsDialog::OnRefreshModels)
    ON_BN_CLICKED(IDOK, &CSettingsDialog::OnOk)
    ON_MESSAGE(WM_PROBE_DONE, &CSettingsDialog::OnProbeResult)
    ON_MESSAGE(WM_MODELS_DONE, &CSettingsDialog::OnModelsResult)
END_MESSAGE_MAP()


void CSettingsDialog::DoDataExchange(CDataExchange* dx)
{
    CDialogEx::DoDataExchange(dx);
}

BOOL CSettingsDialog::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    FillControls();

    // The owner is a hidden tray window, so the dialog can otherwise open behind
    // whatever the user is looking at and appear to have done nothing at all.
    CenterWindow();
    ::SetForegroundWindow(GetSafeHwnd());
    ::BringWindowToTop(GetSafeHwnd());

    // Fill the model list from the server in the background: the dialog must not
    // wait on the network, and the user may already be typing.
    StartModelFetch();
    return TRUE;
}

void CSettingsDialog::StartModelFetch()
{
    ModelsJob* job = new ModelsJob();
    job->owner = GetSafeHwnd();
    // Take whatever is in the fields right now: the user may have just changed
    // the address or the key and expects the list to match.
    job->baseUrl = ControlText(*this, IDC_BASE_URL);
    job->apiKey = ControlText(*this, IDC_API_KEY);

    if (CWnd* refresh = GetDlgItem(IDC_REFRESH_MODELS))
        refresh->EnableWindow(FALSE);

    HANDLE thread = ::CreateThread(nullptr, 0, ModelsThread, job, 0, nullptr);
    if (thread != nullptr) {
        ::CloseHandle(thread);
    } else {
        delete job;
        if (CWnd* refresh = GetDlgItem(IDC_REFRESH_MODELS))
            refresh->EnableWindow(TRUE);
    }
}

void CSettingsDialog::OnRefreshModels()
{
    StartModelFetch();
}

LRESULT CSettingsDialog::OnModelsResult(WPARAM, LPARAM lParam)
{
    std::unique_ptr<ModelsJob> job(reinterpret_cast<ModelsJob*>(lParam));

    if (CWnd* refresh = GetDlgItem(IDC_REFRESH_MODELS))
        refresh->EnableWindow(TRUE);

    CComboBox* combo = static_cast<CComboBox*>(GetDlgItem(IDC_MODEL));
    if (combo == nullptr)
        return 0;

    if (job->models.empty()) {
        // No list is not an error the user must act on: the field still works.
        util::Log(L"[models] список не получен: %s",
                  job->error.IsEmpty() ? L"сервер не сообщил моделей" : job->error.GetString());
        return 0;
    }

    const CStringW current = ControlText(*this, IDC_MODEL);
    combo->ResetContent();

    // A gateway lists everything it serves - chat, embeddings, image models. The
    // ones that can transcribe are the only interesting ones here, so they go to
    // the top and the rest follow in server order.
    std::vector<CStringW> speech;
    std::vector<CStringW> others;
    for (const CStringW& model : job->models) {
        CStringW lower;
        for (int i = 0; i < model.GetLength(); ++i)
            lower.AppendChar(static_cast<wchar_t>(::towlower(model[i])));

        const bool transcribes =
            lower.Find(L"whisper") >= 0 || lower.Find(L"gigaam") >= 0 ||
            lower.Find(L"parakeet") >= 0 || lower.Find(L"asr") >= 0 ||
            lower.Find(L"transcri") >= 0 || lower.Find(L"podlodka") >= 0;
        (transcribes ? speech : others).push_back(model);
    }

    for (const CStringW& model : speech)
        combo->AddString(model);
    for (const CStringW& model : others)
        combo->AddString(model);

    // Keep what the user had; if the field was empty, select the first entry.
    SetDlgItemText(IDC_MODEL, current.IsEmpty() ? job->models.front() : current);
    util::Log(L"[models] получено %d моделей (%d речевых)", static_cast<int>(job->models.size()),
              static_cast<int>(speech.size()));
    return 0;
}

void CSettingsDialog::FillControls()
{
    CComboBox* languages = static_cast<CComboBox*>(GetDlgItem(IDC_LANGUAGE));
    if (languages != nullptr) {
        languages->ResetContent();
        for (const LanguageEntry& entry : kLanguages)
            languages->AddString(entry.label);

        int selected = 0;
        for (size_t i = 0; i < ARRAYSIZE(kLanguages); ++i) {
            if (settings_.language.CompareNoCase(kLanguages[i].code) == 0) {
                selected = static_cast<int>(i);
                break;
            }
        }
        languages->SetCurSel(selected);
    }

    SetDlgItemText(IDC_BASE_URL, settings_.baseUrl);
    SetDlgItemText(IDC_API_KEY, settings_.apiKey);
    SetDlgItemText(IDC_MODEL, settings_.model);
    SetDlgItemText(IDC_PROMPT, settings_.prompt);
    SetDlgItemText(IDC_GLOSSARY, settings_.glossary);

    if (CComboBox* formats = static_cast<CComboBox*>(GetDlgItem(IDC_AUDIO_FORMAT))) {
        formats->ResetContent();
        int selected = 0;
        for (size_t i = 0; i < ARRAYSIZE(kAudioFormats); ++i) {
            const int index = formats->AddString(kAudioFormats[i].label);
            formats->SetItemData(index, i);
            if (settings_.audioFormat.CompareNoCase(kAudioFormats[i].code) == 0)
                selected = index;
        }
        formats->SetCurSel(selected);
    }
    SetDlgItemText(IDC_HOTKEY, settings_.hotkey);
    SetDlgItemText(IDC_SILENCE_MS, util::FormatDouble(settings_.silenceMs, 0));
    SetDlgItemText(IDC_MIN_SPEECH_MS, util::FormatDouble(settings_.minSpeechMs, 0));
    SetDlgItemText(IDC_MAX_SEGMENT_MS, util::FormatDouble(settings_.maxSegmentMs, 0));
    SetDlgItemText(IDC_THRESHOLD, util::FormatDouble(settings_.thresholdRatio, 2));

    CButton* check = static_cast<CButton*>(GetDlgItem(IDC_APPEND_SPACE));
    if (check != nullptr)
        check->SetCheck(settings_.appendSpace ? BST_CHECKED : BST_UNCHECKED);
    check = static_cast<CButton*>(GetDlgItem(IDC_SHIFT_ENTER));
    if (check != nullptr)
        check->SetCheck(settings_.newlineAsShiftEnter ? BST_CHECKED : BST_UNCHECKED);
    check = static_cast<CButton*>(GetDlgItem(IDC_CLIPBOARD_FB));
    if (check != nullptr)
        check->SetCheck(settings_.clipboardFallback ? BST_CHECKED : BST_UNCHECKED);
    check = static_cast<CButton*>(GetDlgItem(IDC_USE_PROMPT));
    if (check != nullptr)
        check->SetCheck(settings_.usePromptContext ? BST_CHECKED : BST_UNCHECKED);
    check = static_cast<CButton*>(GetDlgItem(IDC_AUTOSTART));
    if (check != nullptr)
        check->SetCheck(settings_.autoStart ? BST_CHECKED : BST_UNCHECKED);
    check = static_cast<CButton*>(GetDlgItem(IDC_DEBUG_LOG));
    if (check != nullptr)
        check->SetCheck(settings_.debugLog ? BST_CHECKED : BST_UNCHECKED);

    SetHotkeyLabel();
}

void CSettingsDialog::SetHotkeyLabel()
{
    CButton* button = static_cast<CButton*>(GetDlgItem(IDC_APPLY_HOTKEY));
    if (button == nullptr)
        return;
    CStringW hotkey = ControlText(*this, IDC_HOTKEY);
    CStringW caption = L"Применить";
    UINT mods = 0, vk = 0;
    if (ParseHotkey(hotkey, mods, vk))
        caption = HotkeyDisplayName(mods, vk);
    button->SetWindowText(caption);
}

bool CSettingsDialog::ReadControls(CStringW* error)
{
    Settings next = settings_;

    next.baseUrl = ControlText(*this, IDC_BASE_URL);
    next.apiKey = ControlText(*this, IDC_API_KEY);
    next.model = ControlText(*this, IDC_MODEL);
    next.prompt = ControlText(*this, IDC_PROMPT);
    next.glossary = ControlText(*this, IDC_GLOSSARY);
    next.hotkey = ControlText(*this, IDC_HOTKEY);

    if (CComboBox* languages = static_cast<CComboBox*>(GetDlgItem(IDC_LANGUAGE))) {
        const int index = languages->GetCurSel();
        next.language.Empty();
        if (index >= 0 && index < static_cast<int>(ARRAYSIZE(kLanguages)))
            next.language = kLanguages[index].code;
    }

    if (CComboBox* formats = static_cast<CComboBox*>(GetDlgItem(IDC_AUDIO_FORMAT))) {
        const int index = formats->GetCurSel();
        if (index >= 0) {
            const DWORD_PTR item = formats->GetItemData(index);
            if (item < ARRAYSIZE(kAudioFormats))
                next.audioFormat = kAudioFormats[item].code;
        }
    }

    double value = 0;
    if (!util::ParseDouble(ControlText(*this, IDC_SILENCE_MS), value)) {
        if (error != nullptr)
            *error = L"Пауза должна быть числом (миллисекунды)";
        return false;
    }
    next.silenceMs = value;

    if (!util::ParseDouble(ControlText(*this, IDC_MIN_SPEECH_MS), value)) {
        if (error != nullptr)
            *error = L"Минимальная длительность речи должна быть числом";
        return false;
    }
    next.minSpeechMs = value;

    if (!util::ParseDouble(ControlText(*this, IDC_MAX_SEGMENT_MS), value)) {
        if (error != nullptr)
            *error = L"Максимальная длина сегмента должна быть числом";
        return false;
    }
    next.maxSegmentMs = value;

    if (!util::ParseDouble(ControlText(*this, IDC_THRESHOLD), value)) {
        if (error != nullptr)
            *error = L"Порог должен быть числом";
        return false;
    }
    next.thresholdRatio = value;

    UINT mods = 0, vk = 0;
    if (!ParseHotkey(next.hotkey, mods, vk)) {
        if (error != nullptr)
            *error = L"Горячая клавиша задаётся как «Ctrl+Alt+Shift+клавиша», например Win+Shift+H";
        return false;
    }

    if (next.model.IsEmpty()) {
        if (error != nullptr)
            *error = L"Укажите имя модели (например whisper-1 или whisper-podlodka-turbo)";
        return false;
    }
    if (next.baseUrl.IsEmpty()) {
        if (error != nullptr)
            *error = L"Укажите адрес сервера";
        return false;
    }

    next.appendSpace = IsBoxChecked(*this, IDC_APPEND_SPACE);
    next.newlineAsShiftEnter = IsBoxChecked(*this, IDC_SHIFT_ENTER);
    next.clipboardFallback = IsBoxChecked(*this, IDC_CLIPBOARD_FB);
    next.usePromptContext = IsBoxChecked(*this, IDC_USE_PROMPT);
    next.autoStart = IsBoxChecked(*this, IDC_AUTOSTART);
    next.debugLog = IsBoxChecked(*this, IDC_DEBUG_LOG);

    next.ClampToSaneRanges();
    settings_ = next;
    changed_ = true;
    return true;
}

void CSettingsDialog::OnTest()
{
    CStringW error;
    if (!ReadControls(&error)) {
        AfxMessageBox(error, MB_OK | MB_ICONWARNING);
        return;
    }

    auto* job = new ProbeJob();
    job->owner = GetSafeHwnd();
    job->baseUrl = settings_.baseUrl;
    job->apiKey = settings_.apiKey;
    job->model = settings_.model;

    CButton* button = static_cast<CButton*>(GetDlgItem(IDC_TEST));
    if (button != nullptr)
        button->EnableWindow(FALSE);

    HANDLE thread = ::CreateThread(nullptr, 0, &ProbeThread, job, 0, nullptr);
    if (thread == nullptr) {
        delete job;
        if (button != nullptr)
            button->EnableWindow(TRUE);
        AfxMessageBox(L"Не удалось запустить проверку", MB_OK | MB_ICONERROR);
        return;
    }
    ::CloseHandle(thread);
}

LRESULT CSettingsDialog::OnProbeResult(WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    std::unique_ptr<ProbeJob> job(reinterpret_cast<ProbeJob*>(lParam));

    CButton* button = static_cast<CButton*>(GetDlgItem(IDC_TEST));
    if (button != nullptr)
        button->EnableWindow(TRUE);

    if (job && job->ok)
        AfxMessageBox(job->message, MB_OK | MB_ICONINFORMATION);
    else if (job)
        AfxMessageBox(L"Проверка не удалась:\n\n" + job->message, MB_OK | MB_ICONERROR);
    return 0;
}

void CSettingsDialog::OnOk()
{
    CStringW error;
    if (!ReadControls(&error)) {
        AfxMessageBox(error, MB_OK | MB_ICONWARNING);
        return;
    }
    CDialogEx::OnOK();
}