// windows.h defines min/max macros that fight <algorithm>; this must come
// before any Windows header is pulled in.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "session.h"

#include "audio_encode.h"
#include "util.h"
#include "wav.h"
#include "whisper_client.h"

namespace {

constexpr int kTargetSampleRate = 16000;
constexpr size_t kMaxPromptChars = 400;
// How much of the previous text is replayed as context. Kept small so the
// glossary at the end of the prompt keeps its influence.
constexpr size_t kContextChars = 200;
// Whisper keeps only the last ~224 prompt tokens (n_text_ctx / 2 - 1), about 600
// Russian characters; sending more just gets thrown away from the front.
constexpr int kPromptChars = 600;
constexpr int kGlossaryChars = 400;

}  // namespace

Session::Session()
{
    vad_.SetSegmentSink([this](std::vector<int16_t>&& segment) { OnSegment(std::move(segment)); });
    worker_ = std::thread(&Session::WorkerMain, this);
}

Session::~Session()
{
    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerStop_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable())
        worker_.join();

    capture_.Stop();
}

void Session::SetSettings(const Settings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    settings_.ClampToSaneRanges();
}

Settings Session::SettingsCopy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

bool Session::Start(HWND target, CStringW* error)
{
    if (active_)
        return true;

    const Settings settings = SettingsCopy();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = target;
        phase_ = SessionPhase::Listening;
        error_.Empty();
        nextSeq_ = 0;
        nextToDeliver_ = 0;
        results_.clear();
        queue_.clear();
        accumulatedText_.clear();
        active_ = true;
    }

    vad_.Configure([&] {
        VadConfig cfg;
        cfg.silenceMs = settings.silenceMs;
        cfg.minSpeechMs = settings.minSpeechMs;
        cfg.maxSegmentMs = settings.maxSegmentMs;
        cfg.prerollMs = 300.0;
        cfg.startRatio = settings.thresholdRatio;
        cfg.stopRatio = std::max(1.1, settings.thresholdRatio * 0.55);
        return cfg;
    }());
    vad_.SetSampleRate(kTargetSampleRate);
    vad_.Reset();

    level_.store(0.0f);
    speaking_.store(false);
    networkBusy_.store(false);

    // The resampler picks up the real device rate from the first audio block.
    const bool started = capture_.Start(
        [this](const float* samples, size_t frames) { OnAudio(samples, frames); },
        [this](const CStringW& message) {
            CStringW text;
            text.Format(L"%s", message.GetString());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = text;
                phase_ = SessionPhase::Idle;
                active_ = false;
            }
            util::Log(L"[session] capture failure: %s", message.GetString());
        });

    if (!started) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        phase_ = SessionPhase::Idle;
        if (error != nullptr)
            *error = L"Не удалось запустить захват звука с микрофона";
        return false;
    }

    util::Log(L"[session] started, target=%p, device=%.0f Hz", static_cast<void*>(target_),
              capture_.sampleRate());
    return true;
}

void Session::Stop()
{
    if (!active_)
        return;

    // Stopping the capture thread first makes it safe to touch the detector.
    capture_.Stop();
    vad_.Flush();

    level_.store(0.0f);
    speaking_.store(false);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = SessionPhase::Processing;
    }
    util::Log(L"[session] stopped, flushing");
}

void Session::Cancel()
{
    // Stopping the capture thread first makes it safe to touch the detector.
    capture_.Stop();
    // No vad_.Flush(): the tail of the phrase being recorded is dropped, not
    // turned into a segment.
    vad_.Reset();

    level_.store(0.0f);
    speaking_.store(false);

    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        dropped = queue_.size() + results_.size();
        queue_.clear();
        results_.clear();
        nextSeq_ = 0;
        nextToDeliver_ = 0;
        phase_ = SessionPhase::Idle;
        active_ = false;
        target_ = nullptr;
        error_.Empty();
    }
    util::Log(L"[session] отменено пользователем, отброшено сегментов: %d",
              static_cast<int>(dropped));
}

bool Session::IsActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

HWND Session::Target() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return target_;
}

void Session::OnAudio(const float* samples, size_t frames)
{
    // The device rate is only known once the engine is up, so configure lazily
    // on the first block instead of guessing at Start() time.
    const double rate = capture_.sampleRate();
    if (rate > 0.0 && resampler_.inputRate() != rate)
        resampler_.Configure(rate, kTargetSampleRate);

    std::vector<int16_t> pcm;
    resampler_.Process(samples, frames, pcm);
    if (pcm.empty())
        return;

    vad_.Process(pcm.data(), pcm.size());

    level_.store(vad_.level());
    speaking_.store(vad_.speaking());

    // Every couple of seconds, say what the detector saw. Without this, "the
    // first phrase did nothing" is indistinguishable from "the microphone heard
    // nothing at all" and "the gate was set too high".
    levelFrames_ += pcm.size();
    if (levelFrames_ >= kTargetSampleRate * 2) {
        util::Log(L"[vad] floor=%.4f peak=%.4f speech=%d", vad_.noiseFloor(),
                  vad_.TakePeakRms(), vad_.speaking() ? 1 : 0);
        levelFrames_ = 0;
    }
}

void Session::OnSegment(std::vector<int16_t>&& pcm)
{
    // Runs on the audio thread: hand the finished utterance to the uploader.
    Enqueue(pcm);
}

void Session::Enqueue(const std::vector<int16_t>& pcm)
{
    if (pcm.empty())
        return;

    Job job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.seq = nextSeq_++;
        job.generation = generation_;
        job.pcm = pcm;
        queue_.push_back(std::move(job));
    }
    queueCv_.notify_one();

    util::Log(L"[vad] segment queued: %.2f s", pcm.size() / static_cast<double>(kTargetSampleRate));
}

void Session::WorkerMain()
{
    // Media Foundation and WinHTTP both want COM on this thread.
    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queueCv_.wait(lock, [this] { return workerStop_ || !queue_.empty(); });
            if (workerStop_ && queue_.empty())
                return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        networkBusy_.store(true);

        TranscriptRequest request;
        audioenc::Format format = audioenc::Format::Wav;
        int bitrate = 48000;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request.baseUrl = settings_.baseUrl;
            request.apiKey = settings_.apiKey;
            request.model = settings_.model;
            request.language = settings_.language;
            request.connectTimeoutMs = settings_.connectTimeoutMs;
            request.receiveTimeoutMs = settings_.receiveTimeoutMs;
            format = audioenc::ParseFormat(settings_.audioFormat);
            bitrate = settings_.aacBitrate;

            // The configured hint (style, brand names, terminology) always goes
            // out; the tail of what we already recognised is appended when the
            // user wants that context, and the glossary goes last because the
            // end of the prompt is what influences decoding most.
            CStringW prompt = settings_.prompt;
            prompt.Trim();
            if (settings_.usePromptContext && !accumulatedText_.empty()) {
                std::string context = accumulatedText_;
                if (context.size() > kContextChars)
                    context = context.substr(context.size() - kContextChars);
                CStringW tail(util::Utf8ToWide(context).c_str());
                tail.Trim();
                if (!tail.IsEmpty()) {
                    if (!prompt.IsEmpty())
                        prompt += L" ";
                    prompt += tail;
                }
            }
            if (!settings_.glossary.IsEmpty()) {
                if (!prompt.IsEmpty())
                    prompt += L" ";
                CStringW terms = settings_.glossary;
                terms.Trim();
                prompt += terms;
                if (settings_.glossary.GetLength() > kGlossaryChars) {
                    CStringW warning;
                    warning.Format(L"глоссарий %d символов, а в подсказку влезает примерно %d",
                                   settings_.glossary.GetLength(), kGlossaryChars);
                    util::Log(L"[prompt] %s", warning.GetString());
                }
            }

            // The decoder keeps only the last ~224 tokens (roughly 600 Russian
            // characters) and silently drops the rest, so trim the head: the tail
            // is where the glossary sits.
            if (prompt.GetLength() > kPromptChars) {
                prompt = prompt.Right(kPromptChars);
                util::Log(L"[prompt] обрезано до %d символов (хвост сохранён)", kPromptChars);
            }

            request.usePrompt = !prompt.IsEmpty();
            request.prompt = prompt;
            if (request.usePrompt)
                util::Log(L"[prompt] %s", prompt.GetString());
        }

        // Encoding happens here, off the capture thread.
        audioenc::Upload upload = audioenc::BuildUpload(job.pcm, format, bitrate);
        request.wav = std::move(upload.bytes);
        request.fileName = upload.fileName;
        request.contentType = upload.contentType;

        TranscriptResult result = client_.Send(request);

        // A server that does not take this container says so with 400/415; send
        // the same speech as plain WAV rather than losing the phrase.
        if (!result.ok && (result.httpStatus == 400 || result.httpStatus == 415) &&
            request.contentType != L"audio/wav") {
            util::Log(L"[http] server rejected %s (HTTP %d), retrying as WAV",
                      request.contentType.GetString(), result.httpStatus);
            audioenc::Upload fallback =
                audioenc::BuildUpload(job.pcm, audioenc::Format::Wav, bitrate);
            request.wav = std::move(fallback.bytes);
            request.fileName = fallback.fileName;
            request.contentType = fallback.contentType;
            result = client_.Send(request);
        }

        // HTTP 200 with an empty text means the server found no speech. For a
        // segment that really is silence that is the correct answer, but the same
        // reply also shows up when the compressed upload hid the speech - and
        // then the phrase disappears without a trace. Retry those uncompressed
        // once before believing the silence.
        const double seconds = job.pcm.size() / static_cast<double>(kTargetSampleRate);
        if (result.ok && result.text.Trim().IsEmpty() && request.contentType != L"audio/wav" &&
            seconds >= 1.2) {
            util::Log(L"[http] пустой ответ на %.2f с (M4A), повторяю без сжатия", seconds);
            audioenc::Upload fallback =
                audioenc::BuildUpload(job.pcm, audioenc::Format::Wav, bitrate);
            request.wav = std::move(fallback.bytes);
            request.fileName = fallback.fileName;
            request.contentType = fallback.contentType;
            const TranscriptResult retry = client_.Send(request);
            if (retry.ok && !retry.text.IsEmpty()) {
                util::Log(L"[http] без сжатия речь нашлась — дело было в M4A");
                result = retry;
            } else {
                util::Log(L"[http] и без сжатия тишина: похоже, в сегменте правда нет речи");
            }
        }

        Result stored;
        stored.ok = result.ok;
        CStringW text = result.text;
        stored.text = text.Trim();
        stored.error = result.error;

        // Refuse invented text. Whisper happily returns a confident-looking
        // sentence for audio that holds no speech at all, and that sentence goes
        // straight into the user's document if nobody checks it.
        if (stored.ok && !stored.text.IsEmpty()) {
            CStringW why;
            if (LooksInvented(stored.text, seconds, result, &why)) {
                util::Log(L"[stt] похоже на выдумку модели (%.2f с): %s — текст отброшен: \"%s\"",
                          seconds, why.GetString(), stored.text.GetString());
                stored.text.Empty();
            }
        }

        if (stored.ok && stored.text.IsEmpty())
            util::Log(L"[stt] сегмент %.2f с: сервер не нашёл речи", seconds);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // The user cancelled while this request was in flight: the audio did
            // reach the server and cannot be recalled, but its answer must never
            // be typed anywhere.
            if (job.generation != generation_) {
                util::Log(L"[stt] ответ отброшен: сегмент отменён пользователем");
                networkBusy_.store(false);
                continue;
            }
            if (stored.ok && !stored.text.IsEmpty())
                accumulatedText_ += util::WideToUtf8(stored.text) + " ";
            results_[job.seq] = std::move(stored);
        }
        networkBusy_.store(false);
    }

    if (SUCCEEDED(comResult))
        ::CoUninitialize();
}

SessionSnapshot Session::Snapshot() const
{
    SessionSnapshot snapshot;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.phase = phase_;
    snapshot.networkBusy = networkBusy_.load();
    snapshot.error = error_;
    if (phase_ == SessionPhase::Idle)
        return snapshot;

    snapshot.level = level_.load();
    snapshot.speaking = speaking_.load();
    snapshot.status = (phase_ == SessionPhase::Processing)
                          ? CStringW(L"Распознаю…")
                          : (snapshot.speaking ? CStringW(L"Слушаю…") : CStringW(L"Жду речь…"));
    return snapshot;
}

bool Session::TakeReadyText(CStringW& text, bool& isError, CStringW& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = results_.find(nextToDeliver_);
    if (it == results_.end())
        return false;

    text = it->second.text;
    isError = !it->second.ok;
    error = it->second.error;
    results_.erase(it);
    ++nextToDeliver_;
    return true;
}

bool Session::FinalizeIfDone()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != SessionPhase::Processing)
        return phase_ == SessionPhase::Idle;
    if (!results_.empty() || !queue_.empty() || networkBusy_.load())
        return false;

    phase_ = SessionPhase::Idle;
    active_ = false;
    target_ = nullptr;
    error_.Empty();
    return true;
}

bool Session::HasPendingWork() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !queue_.empty() || !results_.empty() || networkBusy_.load();
}