#pragma once

#include <afxwin.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_capture.h"
#include "overlay.h"
#include "resampler.h"
#include "settings.h"
#include "vad.h"
#include "whisper_client.h"

enum class SessionPhase {
    Idle,
    Listening,
    Processing        // stopping, waiting for the last uploads
};

struct SessionSnapshot {
    SessionPhase phase = SessionPhase::Idle;
    float level = 0.0f;
    bool speaking = false;
    bool networkBusy = false;
    CStringW status;
    CStringW error;
};

// ---------------------------------------------------------------------------
//  Owns the whole dictation pipeline for one "hotkey pressed -> text typed"
//  cycle: capture thread feeds the endpointing detector, which hands finished
//  segments to a single upload worker so they come back in order.
// ---------------------------------------------------------------------------
class Session {
public:
    Session();
    ~Session();

    void SetSettings(const Settings& settings);

    // Starts capture immediately; `error` receives a user-readable reason on
    // failure. The target window is where the text will eventually land.
    bool Start(HWND target, CStringW* error);

    // Closes the microphone, flushes the tail and keeps returning results until
    // everything queued has been delivered.
    void Stop();

    // Drops everything: the phrase still being recorded is not even turned into a
    // segment, queued segments are discarded, and a reply that is already on its
    // way from the server is thrown away when it arrives. Nothing gets typed.
    void Cancel();

    bool IsActive() const;
    HWND Target() const;

    SessionSnapshot Snapshot() const;

    // Ordered delivery: hands the UI the next finished segment, or false when
    // nothing is ready yet.
    bool TakeReadyText(CStringW& text, bool& isError, CStringW& error);

    // True while segments are still queued or being uploaded.
    bool HasPendingWork() const;

    // Closes the session once the microphone is stopped and the queue is empty -
    // including the case where no segment was ever produced.
    bool FinalizeIfDone();

private:
    struct Job {
        uint64_t seq = 0;
        // Which cancel "epoch" this job belongs to: a reply that comes back after
        // the user cancelled carries an old number and is thrown away.
        uint64_t generation = 0;
        std::vector<int16_t> pcm;   // encoded on the worker, not the audio thread
    };

    struct Result {
        bool ok = false;
        CStringW text;
        CStringW error;
    };

    void OnAudio(const float* samples, size_t frames);
    void OnSegment(std::vector<int16_t>&& pcm);
    void WorkerMain();
    void Enqueue(const std::vector<int16_t>& pcm);
    Settings SettingsCopy() const;

    AudioCapture capture_;
    Resampler resampler_;
    Vad vad_;
    WhisperClient client_;

    mutable std::mutex mutex_;
    Settings settings_;
    HWND target_ = nullptr;
    SessionPhase phase_ = SessionPhase::Idle;
    CStringW error_;

    std::deque<Job> queue_;
    std::condition_variable queueCv_;
    std::map<uint64_t, Result> results_;
    uint64_t nextSeq_ = 0;
    uint64_t nextToDeliver_ = 0;
    uint64_t generation_ = 0;
    bool workerStop_ = false;
    bool active_ = false;
    std::string accumulatedText_;

    std::thread worker_;
    size_t levelFrames_ = 0;   // diagnostics: how much audio since the last report
    std::atomic<float> level_{0.0f};
    std::atomic<bool> speaking_{false};
    std::atomic<bool> networkBusy_{false};
};