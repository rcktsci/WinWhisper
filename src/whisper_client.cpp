#include "whisper_client.h"

#include <winhttp.h>

#include <cstring>

#include "util.h"
#include "wav.h"

namespace {

constexpr wchar_t kBoundary[] = L"----WinWhisperBoundary8f2c1a";

// ------------------------------------------------------------ tiny JSON ----

// A deliberately small scanner: Whisper servers answer with a flat object, so a
// full parser would be a dependency we do not need.
class JsonScanner {
public:
    explicit JsonScanner(const std::string& text) : s_(text) {}

    void SkipWs()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool AtEnd()
    {
        SkipWs();
        return i_ >= s_.size();
    }

    bool Peek(char c)
    {
        SkipWs();
        return i_ < s_.size() && s_[i_] == c;
    }

    bool Consume(char c)
    {
        if (!Peek(c))
            return false;
        ++i_;
        return true;
    }

    bool ReadString(std::wstring& out)
    {
        out.clear();
        if (!Consume('"'))
            return false;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out.push_back(static_cast<unsigned char>(c));  // UTF-8 passthrough
                continue;
            }
            if (i_ >= s_.size())
                break;
            const char esc = s_[i_++];
            switch (esc) {
            case 'n': out.push_back(L'\n'); break;
            case 't': out.push_back(L'\t'); break;
            case 'r': out.push_back(L'\r'); break;
            case 'b': out.push_back(L'\b'); break;
            case 'f': out.push_back(L'\f'); break;
            case '/': out.push_back(L'/'); break;
            case '"': out.push_back(L'"'); break;
            case '\\': out.push_back(L'\\'); break;
            case 'u': {
                if (i_ + 4 > s_.size())
                    return false;
                unsigned int cp = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s_[i_ + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                i_ += 4;
                out.push_back(static_cast<wchar_t>(cp & 0xFFFF));  // surrogates pass through
                break;
            }
            default:
                out.push_back(static_cast<wchar_t>(esc));
                break;
            }
        }
        return false;
    }

    bool ReadKey(std::wstring& key)
    {
        return ReadString(key);
    }

    // Numbers, true/false/null: everything up to the next structural character.
    bool ReadBareToken(std::wstring& out)
    {
        SkipWs();
        const size_t start = i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
                c == '\r' || c == '\n')
                break;
            ++i_;
        }
        if (i_ == start)
            return false;
        const std::string raw = s_.substr(start, i_ - start);
        out.assign(raw.begin(), raw.end());
        return true;
    }

    bool SkipValue()
    {
        SkipWs();
        if (i_ >= s_.size())
            return false;

        const char c = s_[i_];
        if (c == '"') {
            std::wstring ignored;
            return ReadString(ignored);
        }
        if (c == '{' || c == '[') {
            const char open = c;
            const char close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (i_ < s_.size()) {
                const char ch = s_[i_];
                if (ch == '"') {
                    std::wstring ignored;
                    if (!ReadString(ignored))
                        return false;
                    continue;
                }
                if (ch == open)
                    ++depth;
                else if (ch == close) {
                    --depth;
                    ++i_;
                    if (depth == 0)
                        return true;
                    continue;
                }
                ++i_;
            }
            return false;
        }
        while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']')
            ++i_;
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
};

std::wstring DecodeUtf8(const std::wstring& raw)
{
    // The scanner keeps raw UTF-8 bytes for anything that was not an escape.
    std::string narrow;
    narrow.reserve(raw.size());
    bool ascii = true;
    for (wchar_t ch : raw) {
        if (ch > 0x7F) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        for (wchar_t ch : raw)
            narrow.push_back(static_cast<char>(ch));
        return util::Utf8ToWide(narrow);
    }
    // Mixed content: rebuild a byte stream (escapes already produced ASCII).
    for (wchar_t ch : raw) {
        if (ch <= 0xFF) {
            narrow.push_back(static_cast<char>(ch));
        } else {
            // Non-ASCII came from a \u escape: encode it as UTF-8.
            const unsigned int cp = static_cast<unsigned int>(ch);
            if (cp < 0x800) {
                narrow.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                narrow.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                narrow.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                narrow.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                narrow.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
    }
    return util::Utf8ToWide(narrow);
}

// Pulls a string field out of the top level object, e.g. {"detail": "..."}.
std::string FindTopLevelString(const std::string& json, const char* key)
{
    JsonScanner sc(json);
    if (!sc.Consume('{'))
        return std::string();

    while (!sc.AtEnd()) {
        std::wstring name;
        if (!sc.ReadKey(name))
            break;
        if (!sc.Consume(':'))
            break;

        const bool match = util::WideToUtf8(name) == key;
        if (match && sc.Peek('"')) {
            std::wstring value;
            if (!sc.ReadString(value))
                break;
            return util::WideToUtf8(DecodeUtf8(value));
        }
        if (!sc.SkipValue())
            break;
        if (!sc.Consume(','))
            break;
    }
    return std::string();
}

// ----------------------------------------------------------------- URL ------

struct Url {
    bool valid = false;
    bool secure = false;
    CStringW host;
    INTERNET_PORT port = 443;
    CStringW path;                 // includes the query, starts with '/'
};

Url ParseUrl(const CStringW& text)
{
    Url url;
    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    components.lpszHostName = host;
    components.dwHostNameLength = ARRAYSIZE(host) - 1;
    components.lpszUrlPath = path;
    components.dwUrlPathLength = ARRAYSIZE(path) - 1;

    if (!::WinHttpCrackUrl(text, static_cast<DWORD>(text.GetLength()), 0, &components))
        return url;

    url.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    url.host = host;
    url.port = components.nPort;
    url.path = path;
    if (url.path.IsEmpty())
        url.path = L"/";
    url.valid = !url.host.IsEmpty();
    return url;
}

// ------------------------------------------------------------ multipart ----

void AppendText(std::string& body, const std::string& text)
{
    body.append(text);
}

void AppendPartHeader(std::string& body, const char* name, const char* fileName,
                      const char* contentType)
{
    AppendText(body, "--");
    AppendText(body, util::WideToUtf8(CStringW(kBoundary)));
    AppendText(body, "\r\nContent-Disposition: form-data; name=\"");
    AppendText(body, name);
    AppendText(body, "\"");
    if (fileName != nullptr) {
        AppendText(body, "; filename=\"");
        AppendText(body, fileName);
        AppendText(body, "\"");
    }
    AppendText(body, "\r\n");
    if (contentType != nullptr) {
        AppendText(body, "Content-Type: ");
        AppendText(body, contentType);
        AppendText(body, "\r\n");
    }
    AppendText(body, "\r\n");
}

void AppendPart(std::string& body, const char* name, const CStringW& value)
{
    AppendPartHeader(body, name, nullptr, nullptr);
    AppendText(body, util::WideToUtf8(value));
    AppendText(body, "\r\n");
}

std::string BuildMultipart(const TranscriptRequest& request)
{
    std::string body;
    body.reserve(request.wav.size() + 2048);

    AppendPartHeader(body, "file", util::WideToUtf8(request.fileName).c_str(),
                     util::WideToUtf8(request.contentType).c_str());
    body.append(reinterpret_cast<const char*>(request.wav.data()), request.wav.size());
    AppendText(body, "\r\n");

    AppendPart(body, "model", request.model);
    if (!request.language.IsEmpty())
        AppendPart(body, "language", request.language);
    if (request.usePrompt && !request.prompt.IsEmpty())
        AppendPart(body, "prompt", request.prompt);
    AppendPart(body, "response_format", CStringW(L"json"));
    AppendPart(body, "temperature", CStringW(L"0"));

    AppendText(body, "--");
    AppendText(body, util::WideToUtf8(CStringW(kBoundary)));
    AppendText(body, "--\r\n");
    return body;
}

// -------------------------------------------------------------- plumbing ----

CStringW DescribeHttpFailure(int status, const std::string& body)
{
    // Prefer the server's own explanation - these gateways speak JSON.
    std::string detail = FindTopLevelString(body, "detail");
    if (detail.empty())
        detail = FindTopLevelString(body, "message");
    if (detail.empty())
        detail = FindTopLevelString(body, "error");

    CStringW suffix;
    if (!detail.empty()) {
        const std::string trimmed = detail.size() > 200 ? detail.substr(0, 200) : detail;
        suffix = L" (" + CStringW(util::Utf8ToWide(trimmed).c_str()) + L")";
    }

    CStringW base;
    switch (status) {
    case 400: base = L"Сервер отклонил запрос (400)"; break;
    case 401: base = L"API-ключ отклонён (401)"; break;
    case 403: base = L"Доступ запрещён (403) - ключ не подписан на эту модель"; break;
    case 404: base = L"Эндпоинт не найден (404) - проверьте base URL"; break;
    case 413: base = L"Аудио слишком большое (413)"; break;
    case 415: base = L"Сервер не принимает этот формат аудио (415)"; break;
    case 429: base = L"Лимит запросов исчерпан (429)"; break;
    case 500:
    case 502:
    case 503:
    case 504: base = L"Ошибка сервера распознавания"; break;
    default: {
        CStringW s;
        s.Format(L"HTTP %d", status);
        base = s;
        break;
    }
    }
    return base + suffix;
}

std::string ReadAll(HINTERNET request)
{
    std::string body;
    char buffer[8192];
    DWORD read = 0;
    while (::WinHttpQueryDataAvailable(request, &read) && read > 0) {
        const DWORD chunk = read < sizeof(buffer) ? read : static_cast<DWORD>(sizeof(buffer));
        if (::WinHttpReadData(request, buffer, chunk, &read) == FALSE)
            break;
        body.append(buffer, read);
        if (read < chunk)
            break;
    }
    return body;
}

std::string LastErrorText(DWORD error)
{
    CStringW text;
    text.Format(L"WinHTTP ошибка 0x%08X", static_cast<unsigned long>(error));
    return util::WideToUtf8(text);
}

}  // namespace

CStringW ExtractTranscriptText(const std::string& json)
{
    return ExtractTranscriptText(json, nullptr);
}

CStringW ExtractTranscriptText(const std::string& json, TranscriptResult* out)
{
    JsonScanner sc(json);
    if (!sc.Consume('{'))
        return CStringW();

    CStringW topText;
    CStringW joined;

    while (!sc.AtEnd()) {
        std::wstring name;
        if (!sc.ReadKey(name))
            break;
        if (!sc.Consume(':'))
            break;

        const std::string key = util::WideToUtf8(name);

        if (key == "text" && sc.Peek('"')) {
            std::wstring value;
            if (sc.ReadString(value))
                topText = CStringW(DecodeUtf8(value).c_str());
        } else if (key == "segments" && sc.Peek('[')) {
            // Fallback for verbose_json: concatenate the per-segment text.
            sc.Consume('[');
            while (!sc.AtEnd()) {
                if (sc.Peek(']')) {
                    sc.Consume(']');
                    break;
                }
                if (sc.Consume('{')) {
                    while (!sc.AtEnd()) {
                        std::wstring segKey;
                        if (!sc.ReadKey(segKey) || !sc.Consume(':'))
                            break;
                        if (util::WideToUtf8(segKey) == "text" && sc.Peek('"')) {
                            std::wstring value;
                            if (sc.ReadString(value))
                                joined += CStringW(DecodeUtf8(value).c_str());
                        } else if (out != nullptr && util::WideToUtf8(segKey) == "avg_logprob" &&
                                   !sc.Peek('"') && !sc.Peek('{') && !sc.Peek('[')) {
                            // Keep the worst segment: one invented line is enough
                            // to poison the phrase.
                            std::wstring number;
                            if (sc.ReadBareToken(number)) {
                                const double value = _wtof(number.c_str());
                                if (!out->hasAvgLogprob || value < out->avgLogprob)
                                    out->avgLogprob = value;
                                out->hasAvgLogprob = true;
                            }
                        } else if (!sc.SkipValue()) {
                            break;
                        }
                        if (!sc.Consume(','))
                            break;
                    }
                    sc.Consume('}');
                } else if (!sc.SkipValue()) {
                    break;
                }
                if (!sc.Consume(','))
                    break;
            }
        } else if (!sc.SkipValue()) {
            break;
        }

        if (!sc.Consume(','))
            break;
    }

    CStringW result = !topText.Trim().IsEmpty() ? topText : joined;
    return result;
}

bool LooksInvented(const CStringW& text, double seconds, const TranscriptResult& result,
                   CStringW* reason)
{
    CStringW trimmed = text;
    trimmed.Trim();
    if (trimmed.IsEmpty())
        return false;

    // --- physics: how fast can a person talk? -------------------------------
    int words = 0;
    bool inWord = false;
    for (int i = 0; i < trimmed.GetLength(); ++i) {
        const bool separator = iswspace(trimmed[i]) != 0;
        if (!separator && !inWord)
            ++words;
        inWord = !separator;
    }

    if (seconds >= 0.3) {
        const double perSecond = words / seconds;
        if (perSecond > 3.5) {
            if (reason != nullptr) {
                reason->Format(L"%d слов за %.1f с — так не говорят", words, seconds);
            }
            return true;
        }
    }

    // --- degeneration: one word repeated over and over ------------------------
    int repeats = 1;
    int best = 1;
    CStringW previous;
    int start = 0;
    while (start <= trimmed.GetLength()) {
        int end = trimmed.Find(L' ', start);
        if (end < 0)
            end = trimmed.GetLength();
        CStringW word = trimmed.Mid(start, end - start);
        word.Trim();
        if (!word.IsEmpty()) {
            if (!previous.IsEmpty() && word.CompareNoCase(previous) == 0) {
                ++repeats;
                if (repeats > best)
                    best = repeats;
            } else {
                repeats = 1;
            }
            previous = word;
        }
        if (end >= trimmed.GetLength())
            break;
        start = end + 1;
    }
    if (best >= 3) {
        if (reason != nullptr)
            reason->Format(L"одно слово повторено %d раза подряд", best);
        return true;
    }

    // --- the model's own confidence ------------------------------------------
    if (result.hasAvgLogprob && result.avgLogprob < -0.70) {
        if (reason != nullptr) {
            reason->Format(L"уверенность модели %.2f (норма выше -0.55; на выдумке бывает -0.7…-1.0)",
                           result.avgLogprob);
        }
        return true;
    }

    return false;
}

WhisperClient::~WhisperClient()
{
    CloseSession();
}

void WhisperClient::CloseSession()
{
    if (session_ != nullptr) {
        ::WinHttpCloseHandle(static_cast<HINTERNET>(session_));
        session_ = nullptr;
    }
    host_.Empty();
    port_ = 0;
    secure_ = false;
}

bool WhisperClient::EnsureSession(const CStringW& host, unsigned short port, bool secure,
                                  int connectTimeoutMs, int receiveTimeoutMs)
{
    // Reuse the session - and with it the pooled TCP/TLS connection - whenever
    // the endpoint has not changed. A fresh session costs a full handshake,
    // which on a slow link is most of the latency of a short phrase.
    if (session_ != nullptr && host_ == host && port_ == port && secure_ == secure &&
        connectTimeout_ == connectTimeoutMs && receiveTimeout_ == receiveTimeoutMs) {
        return true;
    }
    CloseSession();

    HINTERNET session = ::WinHttpOpen(L"WinWhisper/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return false;

    const int connect = connectTimeoutMs > 0 ? connectTimeoutMs : 10000;
    const int receive = receiveTimeoutMs > 0 ? receiveTimeoutMs : 120000;
    ::WinHttpSetTimeouts(session, connect, connect, receive, receive);

    session_ = session;
    host_ = host;
    port_ = port;
    secure_ = secure;
    connectTimeout_ = connectTimeoutMs;
    receiveTimeout_ = receiveTimeoutMs;
    return true;
}

TranscriptResult WhisperClient::Send(const TranscriptRequest& request)
{
    TranscriptResult result;
    const ULONGLONG requestStart = ::GetTickCount64();

    const Url url = ParseUrl(request.baseUrl);
    if (!url.valid) {
        result.error = L"Некорректный адрес сервера: " + request.baseUrl;
        return result;
    }

    CStringW endpoint = url.path;
    if (endpoint.IsEmpty() || endpoint.GetAt(endpoint.GetLength() - 1) != L'/')
        endpoint += L"/";
    endpoint += L"audio/transcriptions";

    std::string body = BuildMultipart(request);

    if (!EnsureSession(url.host, url.port, url.secure, request.connectTimeoutMs,
                       request.receiveTimeoutMs)) {
        result.error = L"Не удалось инициализировать WinHTTP";
        return result;
    }

    HINTERNET connect = ::WinHttpConnect(static_cast<HINTERNET>(session_), url.host, url.port, 0);
    if (connect == nullptr) {
        CloseSession();
        result.error = L"Не удалось подключиться к серверу";
        return result;
    }

    const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET httpRequest = ::WinHttpOpenRequest(connect, L"POST", endpoint, nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (httpRequest == nullptr) {
        ::WinHttpCloseHandle(connect);
        result.error = L"Не удалось создать HTTP-запрос";
        return result;
    }

    auto closeBoth = [&]() {
        ::WinHttpCloseHandle(httpRequest);
        ::WinHttpCloseHandle(connect);
    };

    CStringW headers;
    headers.Format(L"Content-Type: multipart/form-data; boundary=%s\r\n", kBoundary);
    if (!request.apiKey.IsEmpty())
        headers += L"Authorization: Bearer " + request.apiKey + L"\r\n";
    headers += L"Accept: application/json\r\n";

    // Modifiers 0 = "add if new": REPLACE fails outright when the header is not
    // there yet, and WinHTTP pre-populates none of ours.
    if (!::WinHttpAddRequestHeaders(httpRequest, headers, static_cast<DWORD>(-1), 0)) {
        result.error = L"Не удалось сформировать заголовки запроса";
        closeBoth();
        return result;
    }

    // Segments are capped at ~29 s, so the whole body fits comfortably in one
    // send; passing it up front lets WinHTTP set Content-Length itself and
    // avoids the chunked-write path entirely.
    const ULONGLONG sendStart = ::GetTickCount64();
    const DWORD bodyBytes = static_cast<DWORD>(body.size());
    if (!::WinHttpSendRequest(httpRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.data(), bodyBytes,
                              bodyBytes, 0)) {
        const DWORD error = ::GetLastError();
        util::Log(L"[http] send failed, error 0x%08X", error);
        result.error = L"Не удалось отправить запрос";
        closeBoth();
        return result;
    }

    const ULONGLONG sentAt = ::GetTickCount64();
    if (!::WinHttpReceiveResponse(httpRequest, nullptr)) {
        util::Log(L"[http] ReceiveResponse failed: 0x%08X after uploading %u KB",
                  ::GetLastError(), bodyBytes / 1024);
        result.error = L"Сервер не ответил вовремя";
        closeBoth();
        return result;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (::WinHttpQueryHeaders(httpRequest,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX)) {
        result.httpStatus = static_cast<int>(status);
    }

    const std::string response = ReadAll(httpRequest);
    const ULONGLONG doneAt = ::GetTickCount64();
    closeBoth();

    util::Log(L"[http] status=%d, upload %llu ms (%u KB), server %llu ms, total %llu ms, response=%d bytes",
              result.httpStatus, sentAt - sendStart, bodyBytes / 1024, doneAt - sentAt,
              doneAt - requestStart, static_cast<int>(response.size()));

    if (result.httpStatus >= 200 && result.httpStatus < 300) {
        result.text = ExtractTranscriptText(response, &result);
        result.ok = true;
        return result;
    }

    result.error = DescribeHttpFailure(result.httpStatus, response);
    return result;
}



bool WhisperClient::ListModels(const CStringW& baseUrl, const CStringW& apiKey, int timeoutMs,
                               std::vector<CStringW>* models, CStringW* error)
{
    if (models != nullptr)
        models->clear();

    // GET {base}/models. Built by hand like the rest of the client, so the app
    // keeps zero third-party dependencies.
    CStringW url = baseUrl;
    url.Trim();
    while (url.GetLength() > 0 && url[url.GetLength() - 1] == L'/')
        url = url.Left(url.GetLength() - 1);
    url += L"/models";

    Url parsed = ParseUrl(url);
    if (!parsed.valid) {
        if (error != nullptr)
            *error = L"Некорректный адрес сервера";
        return false;
    }

    HINTERNET session = ::WinHttpOpen(L"WinWhisper/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        if (error != nullptr)
            *error = L"Не удалось открыть WinHTTP";
        return false;
    }

    const int timeout = timeoutMs > 0 ? timeoutMs : 8000;
    ::WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);

    bool ok = false;
    HINTERNET connection = ::WinHttpConnect(session, parsed.host.GetString(), parsed.port, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr) {
        const wchar_t* verb = L"GET";
        request = ::WinHttpOpenRequest(connection, verb, parsed.path.GetString(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parsed.secure ? WINHTTP_FLAG_SECURE : 0);
    }

    if (request != nullptr) {
        CStringW headers = L"Accept: application/json\r\n";
        if (!apiKey.IsEmpty()) {
            CStringW auth;
            auth.Format(L"Authorization: Bearer %s\r\n", apiKey.GetString());
            headers += auth;
        }
        if (::WinHttpSendRequest(request, headers.GetString(), static_cast<DWORD>(-1),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            ::WinHttpReceiveResponse(request, nullptr)) {
            std::string body;
            DWORD available = 0;
            do {
                available = 0;
                if (!::WinHttpQueryDataAvailable(request, &available) || available == 0)
                    break;
                const size_t offset = body.size();
                body.resize(offset + available);
                DWORD read = 0;
                if (!::WinHttpReadData(request, &body[offset], available, &read))
                    break;
                body.resize(offset + read);
            } while (available > 0);

            // "data":[{"id":"whisper-1", ...}, ...] - an id is all we need.
            // The gateway answers {"data":[{"id":"whisper-1",...}]}, but a bare
            // array or a different wrapper is just as legal, and the body may be
            // pretty-printed. Rather than depend on a shape, collect every "id"
            // value: nothing else in a model listing carries that key.
            const std::string& text = body;
            size_t at = 0;
            while ((at = text.find("\"id\"", at)) != std::string::npos) {
                at += 4;
                auto skipSpace = [&text, &at] {
                    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' ||
                                                text[at] == '\r' || text[at] == '\n'))
                        ++at;
                };
                skipSpace();
                if (at >= text.size() || text[at] != ':')
                    continue;
                ++at;
                skipSpace();
                if (at >= text.size() || text[at] != '"')
                    continue;
                const size_t valueStart = ++at;
                while (at < text.size() && text[at] != '"') {
                    if (text[at] == '\\' && at + 1 < text.size())
                        ++at;
                    ++at;
                }
                if (at >= text.size())
                    break;
                if (models != nullptr) {
                    // DecodeUtf8 wants the raw UTF-8 bytes held one per wchar_t.
                    const std::string raw = text.substr(valueStart, at - valueStart);
                    const std::wstring wide(raw.begin(), raw.end());
                    models->push_back(CStringW(DecodeUtf8(wide).c_str()));
                }
                ++at;
            }
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                  WINHTTP_NO_HEADER_INDEX);
            util::Log(L"[models] HTTP %lu, %d байт, моделей: %d", status,
                      static_cast<int>(body.size()),
                      models != nullptr ? static_cast<int>(models->size()) : 0);
            ok = models == nullptr || !models->empty();
            if (!ok && error != nullptr)
                *error = L"Сервер ответил, но список моделей пуст";
        } else if (error != nullptr) {
            error->Format(L"Не удалось запросить список моделей (код %lu)", ::GetLastError());
        }
    } else if (error != nullptr) {
        error->Format(L"Не удалось подключиться к %s", parsed.host.GetString());
    }

    if (request != nullptr)
        ::WinHttpCloseHandle(request);
    if (connection != nullptr)
        ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    return ok;
}

bool WhisperClient::Probe(const CStringW& baseUrl, const CStringW& apiKey, const CStringW& model,
                          int timeoutMs, CStringW* error, int* httpStatus)
{
    TranscriptRequest request;
    request.baseUrl = baseUrl;
    request.apiKey = apiKey;
    request.model = model;
    request.usePrompt = false;
    request.connectTimeoutMs = timeoutMs > 0 ? timeoutMs : 10000;
    request.receiveTimeoutMs = timeoutMs > 0 ? timeoutMs : 20000;

    // Half a second of digital silence: enough for the server to accept the
    // upload and answer, without pretending to be a real utterance.
    const std::vector<int16_t> silence(16000 / 2, 0);
    request.wav = MakeWav16kMono(silence);

    WhisperClient client;
    const TranscriptResult result = client.Send(request);
    if (httpStatus != nullptr)
        *httpStatus = result.httpStatus;

    if (!result.ok) {
        if (error != nullptr)
            *error = result.error;
        return false;
    }
    if (error != nullptr) {
        CStringW text;
        text.Format(L"Сервер отвечает (HTTP %d), модель «%s» доступна.", result.httpStatus,
                    model.GetString());
        *error = text;
    }
    return true;
}
