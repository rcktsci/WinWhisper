#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
//  Energy based endpointing with an adaptive noise floor.
//
//  Rationale: the built-in Windows typist splits on pauses too eagerly and
//  chops words in half. Here a segment only closes after a *sustained* silence
//  window, the silence itself is trimmed off the end (so Whisper never receives
//  trailing room noise as "text"), and a short pre-roll keeps the first
//  syllable that would otherwise be lost to the onset detector.
// ---------------------------------------------------------------------------

struct VadConfig {
    double frameMs = 20.0;
    double prerollMs = 300.0;
    double silenceMs = 700.0;
    double minSpeechMs = 250.0;
    double maxSegmentMs = 20000.0;

    double startRatio = 3.0;    // speech starts above noiseFloor * startRatio
    double stopRatio = 1.6;     // ... and keeps going until below noiseFloor * stopRatio
    double absFloorRms = 0.004; // ~ -48 dBFS; below this nothing counts as speech
    double maxZcr = 0.32;       // rejects clicks / keyboard noise at onset
    // Opening a WASAPI stream pops: the first buffers are broadband noise that
    // can open a phantom utterance before the user has said anything. During the
    // warm-up the detector only calibrates; the audio still goes into the
    // pre-roll, so a word spoken this early is not lost.
    double warmupMs = 250.0;
};

class Vad {
public:
    using SegmentSink = std::function<void(std::vector<int16_t>&&)>;

    void Configure(const VadConfig& cfg);
    void SetSampleRate(int rate);
    void SetSegmentSink(SegmentSink sink);

    // Both Process() and Flush() run on the audio thread.
    void Reset();
    void Process(const int16_t* samples, size_t count);
    void Flush();                        // force-close the utterance (user pressed ESC)

    float level() const { return level_; }
    bool speaking() const { return inSpeech_; }
    double noiseFloor() const { return noiseFloor_; }

    // Peak frame energy since the previous call, for diagnostics: "the first
    // phrase was ignored" looks very different in the log depending on whether
    // the microphone heard it at all.
    double TakePeakRms()
    {
        const double peak = peakRms_;
        peakRms_ = 0.0;
        return peak;
    }

private:
    void ProcessFrame(const int16_t* samples, size_t count);
    void Emit(std::vector<int16_t>&& segment);
    void CloseSegment(size_t keepSamplesFromTail);
    void TrimPreRoll();

    VadConfig cfg_{};
    int sampleRate_ = 16000;
    size_t frameSamples_ = 320;
    size_t prerollSamples_ = 4800;
    size_t silenceSamples_ = 11200;
    size_t minSpeechSamples_ = 4000;
    size_t maxSegmentSamples_ = 320000;
    size_t warmupSamples_ = 4800;        // remaining warm-up, counts down
    size_t warmupTotal_ = 4800;

    SegmentSink sink_;

    std::vector<int16_t> pending_;       // pre-roll + speech + trailing silence
    std::vector<int16_t> frame_;

    size_t speechStart_ = 0;             // offset into pending_ where the utterance began
    size_t speechSamples_ = 0;           // confirmed speech length
    size_t silenceRun_ = 0;              // trailing silence
    bool inSpeech_ = false;

    double noiseFloor_ = 0.01;
    float level_ = 0.0f;
    double peakRms_ = 0.0;
    int onsetFrames_ = 0;   // consecutive frames above the gate (onset debounce)
};