#pragma once

#include <cstdint>
#include <vector>

// Band-limited polyphase resampler (windowed sinc + Blackman), used to bring the
// microphone stream to the 16 kHz mono that Whisper expects without the aliasing
// a naive linear interpolation would fold into the speech band.
class Resampler {
public:
    Resampler();

    void Reset();
    void Configure(double inRate, double outRate);

    // Feeds interleaved mono samples; appends whatever is ready to `out`.
    void Process(const float* input, size_t count, std::vector<int16_t>& out);

    double inputRate() const { return inRate_; }
    double outputRate() const { return outRate_; }

private:
    static constexpr int kTaps = 33;      // odd, symmetric around the centre sample
    static constexpr int kPhases = 64;    // fractional-delay resolution

    void BuildTable();

    double inRate_ = 48000.0;
    double outRate_ = 16000.0;
    double step_ = 3.0;                  // input samples consumed per output sample
    long long sourceIndex_ = 0;          // absolute index of buf_[0]

    std::vector<float> buf_;
    std::vector<float> table_;           // kPhases * kTaps

    // Absolute input-sample position of the next output sample. It starts half a
    // kernel into the stream, because the windowed sinc needs `kTaps / 2`
    // samples of history before its first centre - starting at zero would mean
    // the first output never becomes computable and nothing is ever produced.
    double pos_ = kTaps / 2.0;
};