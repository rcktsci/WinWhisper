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
    // Confidence the server reported for the worst segment, when it reports one.
    bool hasAvgLogprob = false;
    double avgLogprob = 0.0;
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

    // GET {base}/models, so the settings dialog can offer the models the server
    // actually has instead of making the user guess names.
    static bool ListModels(const CStringW& baseUrl, const CStringW& apiKey, int timeoutMs,
                           std::vector<CStringW>* models, CStringW* error);

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

// Same, but also reports the worst per-segment confidence the server sent.
CStringW ExtractTranscriptText(const std::string& json, TranscriptResult* out);

// ---------------------------------------------------------------------------
//  Hallucination guard.
//
//  Whisper does not say "I did not understand"; on audio that is too short or
//  too quiet it confidently invents text. Two reproducible examples measured
//  against a live server: half a second of speech came back as
//  "во во во во во во", and 0.8 s of speech produced a four-word subtitle
//  credit. `no_speech_prob` stays at 0.0 in both cases, so the model's own
//  "was there speech" flag is useless here. What does separate the two worlds:
//
//    words per second | real speech: 0.7-1.2 | invented: 4.9 and 12.0
//    avg_logprob      | real speech: -0.04..-0.55 | invented: -0.73..-0.95
//
//  So the guard checks physics first (nobody speaks four words a second) and the
//  model's confidence second. Returns true when the text should not be trusted.
// ---------------------------------------------------------------------------
bool LooksInvented(const CStringW& text, double seconds, const TranscriptResult& result,
                   CStringW* reason);