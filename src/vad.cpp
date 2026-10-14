#include "vad.h"

#include <algorithm>
#include <cmath>

namespace {

double FrameRms(const int16_t* pcm, size_t count)
{
    if (count == 0)
        return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double v = pcm[i] / 32768.0;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(count));
}

double FrameZcr(const int16_t* pcm, size_t count)
{
    if (count < 2)
        return 0.0;
    size_t crossings = 0;
    for (size_t i = 1; i < count; ++i) {
        if ((pcm[i - 1] >= 0) != (pcm[i] >= 0))
            ++crossings;
    }
    return static_cast<double>(crossings) / static_cast<double>(count - 1);
}

size_t MsToSamples(double ms, int rate)
{
    const double n = ms * rate / 1000.0;
    return n > 0 ? static_cast<size_t>(n + 0.5) : 1;
}

}  // namespace

void Vad::Configure(const VadConfig& cfg)
{
    cfg_ = cfg;
    SetSampleRate(sampleRate_);
}

void Vad::SetSampleRate(int rate)
{
    if (rate <= 0)
        return;
    sampleRate_ = rate;
    frameSamples_ = MsToSamples(cfg_.frameMs, rate);
    prerollSamples_ = MsToSamples(cfg_.prerollMs, rate);
    silenceSamples_ = MsToSamples(cfg_.silenceMs, rate);
    minSpeechSamples_ = MsToSamples(cfg_.minSpeechMs, rate);
    maxSegmentSamples_ = MsToSamples(cfg_.maxSegmentMs, rate);

    frame_.resize(frameSamples_);
    pending_.reserve(std::max(pending_.capacity(), prerollSamples_ * 4));
}

void Vad::SetSegmentSink(SegmentSink sink)
{
    sink_ = std::move(sink);
}

void Vad::Reset()
{
    pending_.clear();
    speechStart_ = 0;
    speechSamples_ = 0;
    silenceRun_ = 0;
    inSpeech_ = false;
    onsetFrames_ = 0;
    // Start low and let the tracker climb if the room turns out to be noisy:
    // guessing high is what silently swallowed the first phrase of a session.
    noiseFloor_ = cfg_.absFloorRms;
    level_ = 0.0f;
    peakRms_ = 0.0;
}

void Vad::TrimPreRoll()
{
    if (inSpeech_)
        return;
    if (pending_.size() > prerollSamples_)
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<ptrdiff_t>(pending_.size() - prerollSamples_));
}

void Vad::Emit(std::vector<int16_t>&& segment)
{
    if (segment.empty() || !sink_)
        return;
    sink_(std::move(segment));
}

void Vad::CloseSegment(size_t keepFromTail)
{
    const size_t total = pending_.size();

    // Everything after the last confirmed speech frame is room tone: drop it.
    size_t cut = total - std::min(silenceRun_, total);

    if (speechSamples_ >= minSpeechSamples_ && cut > 0) {
        std::vector<int16_t> segment;
        segment.reserve(cut - speechStart_);
        segment.assign(pending_.begin() + static_cast<ptrdiff_t>(speechStart_),
                       pending_.begin() + static_cast<ptrdiff_t>(cut));
        Emit(std::move(segment));
    }

    // Keep a little of the tail as the next utterance's pre-roll.
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(cut));
    if (keepFromTail > 0 && pending_.size() > keepFromTail)
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<ptrdiff_t>(pending_.size() - keepFromTail));

    speechStart_ = 0;
    speechSamples_ = 0;
    silenceRun_ = 0;
    inSpeech_ = false;
}

void Vad::Process(const int16_t* samples, size_t count)
{
    size_t offset = 0;
    while (offset < count) {
        const size_t take = std::min(count - offset, frameSamples_);
        std::copy(samples + offset, samples + offset + take, frame_.begin());
        ProcessFrame(frame_.data(), take);
        offset += take;
    }
}

void Vad::ProcessFrame(const int16_t* samples, size_t count)
{
    const double rms = FrameRms(samples, count);
    const double zcr = FrameZcr(samples, count);
    peakRms_ = std::max(peakRms_, rms);

    // --- UI meter: fast attack, slow release, so the ring looks alive --------
    const float target = static_cast<float>(std::min(1.0, std::pow(rms / 0.09, 0.55)));
    const float k = target > level_ ? 0.55f : 0.12f;
    level_ += (target - level_) * k;

    const double startGate = std::max(noiseFloor_ * cfg_.startRatio, cfg_.absFloorRms);
    const double stopGate = std::max(noiseFloor_ * cfg_.stopRatio, cfg_.absFloorRms * 0.8);

    if (!inSpeech_) {
        // Track the room: drop fast towards a quieter level, creep up slowly.
        // A slow, symmetric average is what made the *first* phrase of a session
        // disappear - the floor started too high and needed seconds of silence to
        // come down to the real noise level. Creeping up is what keeps a noisy
        // room from being mistaken for speech.
        if (rms < noiseFloor_)
            noiseFloor_ = noiseFloor_ * 0.80 + rms * 0.20;
        else
            noiseFloor_ = noiseFloor_ * 0.995 + rms * 0.005;
        noiseFloor_ = std::max(noiseFloor_, cfg_.absFloorRms * 0.5);

        pending_.insert(pending_.end(), samples, samples + count);
        TrimPreRoll();

        // Loud clicks bypass the zero-crossing gate so a fricative-initial word
        // is not lost to a single unlucky frame. Two frames in a row are needed
        // so a lone noise spike cannot open an utterance.
        const bool aboveGate = rms > startGate;
        const bool voiced = zcr <= cfg_.maxZcr;
        onsetFrames_ = (aboveGate && (voiced || rms > startGate * 2.5)) ? onsetFrames_ + 1 : 0;
        if (onsetFrames_ >= 2) {
            inSpeech_ = true;
            speechSamples_ = 0;
            silenceRun_ = 0;
            onsetFrames_ = 0;
            speechStart_ = pending_.size() > prerollSamples_
                              ? pending_.size() - prerollSamples_
                              : 0;
            speechSamples_ += count;
        }
        return;
    }

    // --- inside an utterance ------------------------------------------------
    pending_.insert(pending_.end(), samples, samples + count);

    if (rms > stopGate) {
        speechSamples_ += count;
        silenceRun_ = 0;
    } else {
        silenceRun_ += count;
    }

    const size_t utteranceLength = pending_.size() - speechStart_;

    if (silenceRun_ >= silenceSamples_) {
        CloseSegment(prerollSamples_);
        return;
    }

    if (utteranceLength >= maxSegmentSamples_) {
        // Whisper only looks at 30 s anyway; cut at a frame boundary and keep
        // going so a long monologue arrives as several ordered chunks.
        const size_t cut = pending_.size();
        if (speechSamples_ >= minSpeechSamples_) {
            std::vector<int16_t> segment(pending_.begin() + static_cast<ptrdiff_t>(speechStart_),
                                         pending_.begin() + static_cast<ptrdiff_t>(cut));
            Emit(std::move(segment));
        }
        const size_t keep = std::min(cut, prerollSamples_);
        pending_.assign(pending_.end() - static_cast<ptrdiff_t>(keep), pending_.end());
        speechStart_ = 0;
        speechSamples_ = 0;
        silenceRun_ = 0;
    }
}

void Vad::Flush()
{
    if (inSpeech_) {
        CloseSegment(0);
    } else {
        pending_.clear();
        speechStart_ = 0;
        speechSamples_ = 0;
        silenceRun_ = 0;
    }
    level_ = 0.0f;
}