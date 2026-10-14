#pragma once

#include <afxwin.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TranscriptRequest {
    CStringW baseUrl;              // e.g. https://api.neuraldeep.ru/v1
    CStringW apiKey;               // may be empty for servers that need none
    std::vector<uint8_t> wav;      // payload: RIFF/WAVE or M4A/AAC, see fileName
    CStringW fileName = L"audio.wav";
    CStringW contentType = L"audio/wav";
    CStringW model;
    CStringW language;             // empty = auto
    CStringW prompt;               // style hint + previous text
    bool usePrompt = true;
    int connectTimeoutMs = 10000;
    int receiveTimeoutMs = 120000;
};

struct TranscriptResult {
    bool ok = false;
    CStringW text;
    CStringW error;                // already user-readable
    int httpStatus = 0;
};

// ---------------------------------------------------------------------------
//  OpenAI-compatible /audio/transcriptions client built on WinHTTP.
//  The multipart body is assembled by hand so the app keeps zero dependencies.
// ---------------------------------------------------------------------------
class WhisperClient {
public:
    WhisperClient() = default;
    ~WhisperClient();
    WhisperClient(const WhisperClient&) = delete;
    WhisperClient& operator=(const WhisperClient&) = delete;

    // Synchronous: call from a single worker thread. The WinHTTP session is
    // kept alive between calls so consecutive phrases reuse the same TCP/TLS
    // connection instead of paying for a handshake every time.
    TranscriptResult Send(const TranscriptRequest& request);

    // Cheap connectivity check used by the settings dialog.
    static bool Probe(const CStringW& baseUrl, const CStringW& apiKey, const CStringW& model,
                      int timeoutMs, CStringW* error, int* httpStatus);

private:
    void CloseSession();
    bool EnsureSession(const CStringW& host, unsigned short port, bool secure,
                       int connectTimeoutMs, int receiveTimeoutMs);

    void* session_ = nullptr;
    CStringW host_;
    unsigned short port_ = 0;
    bool secure_ = false;
    int connectTimeout_ = 0;
    int receiveTimeout_ = 0;
};

// Pulls "text" (or the joined "segments") out of a JSON body.
CStringW ExtractTranscriptText(const std::string& json);